#!/usr/bin/env python3
"""
VRF 配置面 + OS 下刷 全链路验证。

覆盖：
  1) vrf <name> 创建 / no vrf <name> 删除（含 OS L3VRF 设备 install/remove）
  2) af ipv4-unicast / af ipv6-unicast 进入 + no af 删除
  3) route-distinguisher / no route-distinguisher
  4) vpn-target import / export / both / no vpn-target
  5) show vrf | show vrf name <name> 详情含 RD/RT/OS State
  6) show vrf os 与内核 ip link show type vrf 一致
  7) reboot 后 DB 恢复，配置与 OS 设备同步重新生效
"""

from __future__ import annotations

import re
import subprocess
import time

from module_api import reboot_device, require_devices, run_cmds, step, wait_check  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "blue"
VRF_NAME2 = "red"
RD = "65000:100"
RT_IMP = "65000:200"
RT_EXP = "65000:300"
RT_BOTH = "65000:400"


def _docker_exec(rt: TopologyRuntime, device: str, sh_cmd: str) -> tuple[int, str]:
    proc = subprocess.run(
        ["docker", "exec", rt.container_name(device), "/bin/sh", "-lc", sh_cmd],
        text=True,
        capture_output=True,
        check=False,
    )
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def _wait_kernel_vrf(rt: TopologyRuntime, device: str, name: str, *, present: bool, table_id: int | None = None,
                     timeout: int = 10) -> None:
    """轮询内核 ip link，确认 vrf 设备存在/不存在与 table id。"""
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        rc, out = _docker_exec(rt, device, f"ip -d link show {name} 2>&1")
        last = out
        if present:
            if rc == 0 and "vrf table " in out:
                if table_id is None or f"vrf table {table_id}" in out:
                    return
        else:
            # 不存在时 ip link show 会返回非 0 + "does not exist"
            if rc != 0 and ("does not exist" in out or "Cannot find" in out):
                return
        time.sleep(1)
    raise AssertionError(
        f"kernel vrf {name} present={present} timeout; last output:\n{last}"
    )


def _wait_kernel_vrf_loopback_local_routes(
    rt: TopologyRuntime,
    device: str,
    name: str,
    *,
    timeout: int = 10,
) -> None:
    """轮询 VRF table，确认 ROUTE/FIB 下发 VRF 专属 localhost local 路由。"""
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        rc, link_out = _docker_exec(rt, device, f"ip -d link show {name} 2>&1")
        last = link_out
        match = re.search(r"vrf table (\d+)", link_out)
        if rc == 0 and match:
            table_id = match.group(1)
            rc4, out4 = _docker_exec(rt, device, f"ip -4 route show table {table_id} 2>&1")
            rc6, out6 = _docker_exec(rt, device, f"ip -6 route show table {table_id} 2>&1")
            last = f"IPv4:\n{out4}\nIPv6:\n{out6}"
            has_127_8 = re.search(r"(?m)^local\s+127\.0\.0\.0/8\s+dev\s+lo\b", out4) is not None
            has_127_1 = re.search(r"(?m)^local\s+127\.0\.0\.1(?:/32)?\s+dev\s+lo\b", out4) is not None
            has_v6_loop = re.search(r"(?m)^local\s+::1(?:/128)?\s+dev\s+lo\b", out6) is not None
            if rc4 == 0 and rc6 == 0 and has_127_8 and has_127_1 and has_v6_loop:
                return
        time.sleep(1)
    raise AssertionError(f"kernel vrf {name} missing localhost local routes; last output:\n{last}")


def _verify_show_vrf_os(rt: TopologyRuntime, *, contains_names: list[str], not_contains_names: list[str]) -> None:
    contains: list[str] = ["OS L3VRF Devices:"] + contains_names
    not_contains: list[str] = list(not_contains_names)
    wait_check(
        rt,
        device="r1",
        command="show vrf os",
        timeout=10,
        interval=1,
        contains=contains,
        not_contains=not_contains,
        label=f"r1 show vrf os ({','.join(contains_names) or 'empty'})",
    )


def _cleanup(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no vrf {VRF_NAME}",
            f"no vrf {VRF_NAME2}",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))

    try:
        _run_inner(rt)
    finally:
        step("Restore baseline (delete test VRFs)")
        _cleanup(rt)
        # 内核侧也确认清干净
        _wait_kernel_vrf(rt, "r1", VRF_NAME, present=False)
        _wait_kernel_vrf(rt, "r1", VRF_NAME2, present=False)

    print("VRF basic check passed.")


def _run_inner(rt: TopologyRuntime) -> None:
    # 起始干净状态
    _cleanup(rt)

    # ---- Phase 1: 创建 VRF + 验证 OS L3VRF 设备 ----
    step(f"Create VRF {VRF_NAME}")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device="r1",
        command=f"show vrf name {VRF_NAME}",
        timeout=10,
        contains=[
            "VRF Detail:",
            f"Name           : {VRF_NAME}",
            "OS State       : UP",
        ],
        label=f"r1 show vrf name {VRF_NAME} after create",
    )
    # 内核侧同步
    _wait_kernel_vrf(rt, "r1", VRF_NAME, present=True)
    _wait_kernel_vrf_loopback_local_routes(rt, "r1", VRF_NAME)
    _verify_show_vrf_os(rt, contains_names=[VRF_NAME], not_contains_names=[])

    # ---- Phase 2: af ipv4-unicast + RD + 多种 vpn-target 方向 ----
    step(f"Configure AF ipv4-unicast under VRF {VRF_NAME}: RD + import/export/both RT")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"route-distinguisher {RD}",
            f"vpn-target {RT_IMP} import",
            f"vpn-target {RT_EXP} export",
            f"vpn-target {RT_BOTH} both",
            "exit",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device="r1",
        command=f"show vrf name {VRF_NAME}",
        timeout=10,
        contains=[
            "af ipv4-unicast:",
            f"RD            : {RD}",
            "Import-RT     :",
            "Export-RT     :",
            RT_IMP,
            RT_EXP,
            RT_BOTH,
        ],
        label=f"r1 ipv4-unicast RD/RT visible",
    )

    step("Direct RD modification must fail before no route-distinguisher")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
        ],
    )
    out = rt.exec_cmd("r1", "route-distinguisher 65000:101", strict=False)
    rt.exec_cmd("r1", "end", strict=False)
    assert "VRF Error:" in out, f"direct RD modify unexpectedly succeeded:\n{out}"
    wait_check(
        rt,
        device="r1",
        command="show current-configuration",
        timeout=10,
        contains=["route-distinguisher 65000:100"],
        not_contains=["route-distinguisher 65000:101"],
        label="direct RD modify failure keeps original RD in DB config",
    )

    # ---- Phase 3: af ipv6-unicast 仅 RD ----
    step(f"Configure AF ipv6-unicast under VRF {VRF_NAME}: RD only")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv6-unicast",
            f"route-distinguisher {RD}",
            "exit",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device="r1",
        command=f"show vrf name {VRF_NAME}",
        timeout=10,
        contains=[
            "af ipv6-unicast:",
            f"RD            : {RD}",
        ],
        label=f"r1 ipv6-unicast RD visible",
    )

    # ---- Phase 4: 第二个 VRF + show current-configuration 文本回显 ----
    step(f"Create second VRF {VRF_NAME2} with RD + import RT")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME2}",
            "af ipv4-unicast",
            "route-distinguisher 65000:500",
            "vpn-target 65000:600 import",
            "exit",
            "exit",
            "end",
        ],
    )
    _wait_kernel_vrf(rt, "r1", VRF_NAME2, present=True)
    _wait_kernel_vrf_loopback_local_routes(rt, "r1", VRF_NAME2)
    _verify_show_vrf_os(rt, contains_names=[VRF_NAME, VRF_NAME2], not_contains_names=[])

    # show current-configuration 必须包含两个 VRF 块和 RD/RT 文本
    wait_check(
        rt,
        device="r1",
        command="show current-configuration",
        timeout=10,
        contains=[
            f"vrf {VRF_NAME}",
            f"vrf {VRF_NAME2}",
            "af ipv4-unicast",
            "af ipv6-unicast",
            "route-distinguisher 65000:100",
            "route-distinguisher 65000:500",
            f"vpn-target {RT_IMP}",
            f"vpn-target {RT_EXP}",
            f"vpn-target {RT_BOTH}",
            "vpn-target 65000:600 import",
        ],
        label="show current-configuration contains VRF blocks",
    )

    # ---- Phase 5: no vpn-target / no route-distinguisher / no af ----
    step(f"Withdraw RT then RD then AF on {VRF_NAME}/ipv4-unicast")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"no vpn-target {RT_IMP} import",
            f"no vpn-target {RT_EXP} export",
            f"no vpn-target {RT_BOTH} both",
            "no route-distinguisher",
            "exit",
            "no af ipv4-unicast",
            "exit",
            "end",
        ],
    )
    out = rt.exec_cmd("r1", f"show vrf name {VRF_NAME}", timeout=5)
    assert "af ipv4-unicast" not in out, f"ipv4-unicast still present:\n{out}"
    assert RT_IMP not in out and RT_EXP not in out and RT_BOTH not in out, (
        f"RT still present:\n{out}"
    )

    # ---- Phase 6: reboot + DB 恢复（VRF blue 的 ipv6-unicast/RD 与 VRF red 的 ipv4-unicast/RD/RT 应回来）----
    step("Reboot r1 and verify VRF DB restore + OS reapply")
    # reboot 后要验证 VRF 从 DB 恢复并回放到 OS，配置必须存活：先 save 落盘到 startup-config
    reboot_device(rt, "r1", timeout=120, save_config=True)
    wait_check(
        rt,
        device="r1",
        command="show vrf",
        timeout=20,
        contains=["VRF Table:", VRF_NAME, VRF_NAME2],
        label="r1 show vrf after reboot",
    )
    _wait_kernel_vrf(rt, "r1", VRF_NAME, present=True, timeout=30)
    _wait_kernel_vrf(rt, "r1", VRF_NAME2, present=True, timeout=30)
    _wait_kernel_vrf_loopback_local_routes(rt, "r1", VRF_NAME, timeout=30)
    _wait_kernel_vrf_loopback_local_routes(rt, "r1", VRF_NAME2, timeout=30)
    wait_check(
        rt,
        device="r1",
        command=f"show vrf name {VRF_NAME}",
        timeout=15,
        contains=[
            "af ipv6-unicast:",
            f"RD            : {RD}",
            "OS State       : UP",
        ],
        label="r1 vrf blue restored",
    )
    wait_check(
        rt,
        device="r1",
        command=f"show vrf name {VRF_NAME2}",
        timeout=15,
        contains=[
            "af ipv4-unicast:",
            "RD            : 65000:500",
            "65000:600",
        ],
        label="r1 vrf red restored",
    )

    # ---- Phase 7: 删除 VRF + 验证 OS 设备消失 ----
    step(f"Delete VRF {VRF_NAME}")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )
    out = rt.exec_cmd("r1", "show vrf", timeout=5)
    # show vrf 表头会出现 "VRF Table:" / "Name" 等关键字，VRF_NAME 不应再作为 entry 出现
    data_lines = [line for line in out.splitlines() if line.strip() and not line.startswith(("VRF", "  VRF-ID", "  --"))]
    assert all(VRF_NAME not in line for line in data_lines), f"vrf {VRF_NAME} still listed:\n{out}"
    _wait_kernel_vrf(rt, "r1", VRF_NAME, present=False)
    _verify_show_vrf_os(rt, contains_names=[VRF_NAME2], not_contains_names=[VRF_NAME])

    step(f"Delete VRF {VRF_NAME2}")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"no vrf {VRF_NAME2}",
            "end",
        ],
    )
    _wait_kernel_vrf(rt, "r1", VRF_NAME2, present=False)

    # ---- Phase 8: 公网 VRF 拒绝手动创建 / 删除 ----
    step("Public VRF rejects manual create/delete")
    rt.exec_cmd("r1", "config", strict=False)
    out_create = rt.exec_cmd("r1", "vrf public", strict=False)
    rt.exec_cmd("r1", "end", strict=False)
    assert "Public VRF cannot be manually created" in out_create, f"unexpected output:\n{out_create}"
    rt.exec_cmd("r1", "config", strict=False)
    out_del = rt.exec_cmd("r1", "no vrf public", strict=False)
    rt.exec_cmd("r1", "end", strict=False)
    assert "Public VRF cannot be deleted" in out_del, f"unexpected output:\n{out_del}"
    # 公网设备不应在 OS 侧出现
    rc, out = _docker_exec(rt, "r1", "ip link show public 2>&1 || true")
    assert "does not exist" in out or "Cannot find" in out or "Device \"public\"" not in out, (
        f"public vrf shouldn't have OS device:\n{out}"
    )
