#!/usr/bin/env python3
"""
业务模块重启对 VRF / IF / ROUTE / BGP 业务恢复的端到端验证。

拓扑：r1 ←GE-1→ r2（IPv4 单链路），两端 VRF blue 内拉起 EBGP 直连邻居。

baseline 与 vrf_process_resync 相同：
- r1/r2 各创建 vrf blue + IPv4 AF + RD，GE-1 绑入 blue 并配 10.99.0.1-2/30
- r1 AS 65001 ↔ r2 AS 65002 在 vrf blue 下 IPv4-unicast Established
- r1 在 vrf blue 配静态路由 203.0.113.0/24 → 10.99.0.2

依次 reboot r1 上三个业务模块，每次 reboot 后必须满足：
  Phase B. process reboot bgp on r1
           - r1 BGP pid 更替，会话再次 Established（IF/VRF/route 不受影响）
           - vrf blue 内静态路由 / 直连路由保留
  Phase C. process reboot route on r1
           - r1 ROUTE pid 更替，静态路由从 DB 恢复
           - BGP 邻居最终回到 Established
  Phase D. process reboot if on r1
           - r1 IF pid 更替，GE-1 重新绑入 vrf blue 并恢复 10.99.0.1/30
           - 直连路由 / 静态路由 / BGP 会话 都恢复

清理：拆 BGP / 静态路由 / VRF 绑定 / VRF 配置。
"""

from __future__ import annotations

import re
import subprocess
import time

from module_api import (  # noqa: E402
    cmd,
    g_top,
    mark_step_failed,
    process_reboot,
    require_devices,
    run_cmds,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "blue"
GE_IF = "GE-1"

R1_V4 = "10.99.0.1"
R2_V4 = "10.99.0.2"
V4_LEN = 30
V4_NET = "10.99.0.0"

STATIC_PREFIX_ADDR = "203.0.113.0"
STATIC_PREFIX_LEN = 24
STATIC_PREFIX = f"{STATIC_PREFIX_ADDR}/{STATIC_PREFIX_LEN}"
STATIC_NH = R2_V4

R1_AS = 65001
R2_AS = 65002
R1_RID = "1.1.1.1"
R2_RID = "2.2.2.2"

WAIT_PID_SEC = 15
WAIT_VRF_READY_SEC = 25
WAIT_BGP_UP_SEC = 60
WAIT_ROUTE_SEC = 25


# ---------------------------------------------------------------------------
# 进程探针
# ---------------------------------------------------------------------------


def _list_pids(container: str, binary: str) -> list[int]:
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", binary],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(f"pgrep {binary} failed: {proc.stderr}")
    return [int(x) for x in proc.stdout.split() if x.strip().isdigit()]


def _wait_pid(container: str, binary: str, *, predicate, timeout: float, what: str) -> list[int]:
    deadline = time.monotonic() + timeout
    pids: list[int] = []
    while time.monotonic() < deadline:
        pids = _list_pids(container, binary)
        if predicate(pids):
            return pids
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting {what}; last {binary} pids={pids}")


# ---------------------------------------------------------------------------
# 业务断言
# ---------------------------------------------------------------------------


def _wait_if_bound(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_VRF_READY_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show if {GE_IF}",
        timeout=timeout,
        interval=2,
        contains=[
            f"Interface {GE_IF} Detail:",
            f"VRF        : {VRF_NAME}",
            f"IPv4 Addr  : {R1_V4}/{V4_LEN}" if device == "r1" else f"IPv4 Addr  : {R2_V4}/{V4_LEN}",
        ],
        regex=[r"(?im)^\s*(?:Proto\s+)?State\s*:\s*UP\s*$"],
        label=f"{device} {GE_IF} bound to vrf {VRF_NAME}",
    )


def _wait_bgp_established(
    rt: TopologyRuntime,
    device: str,
    *,
    peer_v4: str,
    timeout: int = WAIT_BGP_UP_SEC,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
        timeout=timeout,
        interval=2,
        contains=[peer_v4, "AF: ipv4-unicast"],
        regex=[rf"(?im)^\s*{re.escape(peer_v4)}\s+\S+\s+\S+\s+Established\s*$"],
        label=f"{device} vrf={VRF_NAME} ipv4 session up",
    )


def _wait_static_in_rib(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_ROUTE_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 vrf {VRF_NAME} {STATIC_PREFIX_ADDR} {STATIC_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        regex=[
            rf"(?im)^\s*Routing entry for {re.escape(STATIC_PREFIX)} \(VRF: {re.escape(VRF_NAME)}\)\s*$",
            r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b",
        ],
        not_contains=["(no routes)", "(no matching routes)"],
        label=f"{device} static {STATIC_PREFIX} in vrf {VRF_NAME} present",
    )


def _wait_direct_in_rib(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_ROUTE_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 vrf {VRF_NAME} {V4_NET} {V4_LEN}",
        timeout=timeout,
        interval=2,
        regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
        label=f"{device} direct {V4_NET}/{V4_LEN} in vrf {VRF_NAME}",
    )


def _wait_vrf_up(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_VRF_READY_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show vrf name {VRF_NAME}",
        timeout=timeout,
        interval=1,
        contains=["VRF Detail:", f"Name           : {VRF_NAME}", "OS State       : UP"],
        label=f"{device} vrf {VRF_NAME} UP",
    )


# ---------------------------------------------------------------------------
# 拓扑准备 / 清理
# ---------------------------------------------------------------------------


def _baseline(device: str) -> dict[str, str | int]:
    g = getattr(g_top, device)
    return {"v4": str(g.GE_1.ip), "v4_len": int(g.GE_1.prefix)}


def _cleanup(rt: TopologyRuntime, baseline: dict[str, dict[str, str | int]]) -> None:
    for dev in ("r1", "r2"):
        b = baseline[dev]
        commands = ["end", "config", "no bgp"]
        if dev == "r1":
            commands.append(
                f"no route static ipv4 vrf {VRF_NAME} {STATIC_PREFIX_ADDR} {STATIC_PREFIX_LEN} {STATIC_NH}"
            )
        local_v4 = R1_V4 if dev == "r1" else R2_V4
        commands += [
            f"if {GE_IF}",
            "no shutdown",
            f"no ip address {local_v4} {V4_LEN}",
            "no vrf forwarding",
            f"ip address {b['v4']} {b['v4_len']}",
            "exit",
            f"no vrf {VRF_NAME}",
            "end",
        ]
        run_cmds(rt=rt, device=dev, strict=False, commands=commands)


def _setup_vrf_and_link(rt: TopologyRuntime, *, device: str, local_v4: str, rd: str) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"route-distinguisher {rd}",
            "exit",
            "exit",
            "end",
        ],
    )
    _wait_vrf_up(rt, device, timeout=10)
    time.sleep(2)
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {local_v4} {V4_LEN}",
            "exit",
            "end",
        ],
    )
    _wait_if_bound(rt, device)


def _setup_bgp(
    rt: TopologyRuntime,
    *,
    device: str,
    local_as: int,
    rid: str,
    peer_v4: str,
    peer_as: int,
) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"bgp {local_as}",
            f"router-id {rid}",
            f"vrf {VRF_NAME}",
            f"router-id {rid}",
            f"neighbor {peer_v4} as {peer_as}",
            "af ipv4-unicast",
            f"neighbor {peer_v4} enable",
            "exit",
            "exit",
            "end",
        ],
    )


# ---------------------------------------------------------------------------
# 单个业务模块 reboot 验证
# ---------------------------------------------------------------------------


def _reboot_and_verify(
    rt: TopologyRuntime,
    container: str,
    *,
    module: str,
    binary: str,
) -> tuple[int, int]:
    """统一的：抓老 pid → reboot → 等新 pid → 验证业务恢复。返回 (old, new) pid。"""
    before = _list_pids(container, binary)
    if len(before) != 1:
        mark_step_failed()
        raise AssertionError(f"expected exactly 1 {binary} pid, got {before}")
    old_pid = before[0]

    # process_reboot 已封装 cmd + wait_modules_ready
    out = process_reboot(rt, "r1", module)
    if f"reboot {module} ok" not in out and "spawned" not in out:
        mark_step_failed()
        raise AssertionError(f"unexpected `process reboot {module}` response:\n{out}")

    _wait_pid(container, binary, predicate=lambda p: old_pid not in p,
              timeout=WAIT_PID_SEC, what=f"old {binary} pid {old_pid} to exit")
    after = _wait_pid(container, binary, predicate=lambda p: len(p) == 1 and p[0] != old_pid,
                       timeout=WAIT_PID_SEC, what=f"new {binary} pid after reboot")
    new_pid = after[0]
    print(f"[reboot {module}] r1 pid: {old_pid} → {new_pid}", flush=True)
    return old_pid, new_pid


def _assert_all_business_ok(rt: TopologyRuntime) -> None:
    """所有业务（VRF / IF binding / 直连 / BGP / 静态路由）在两端都就绪。"""
    _wait_vrf_up(rt, "r1")
    _wait_if_bound(rt, "r1")
    _wait_direct_in_rib(rt, "r1")
    _wait_bgp_established(rt, "r1", peer_v4=R2_V4)
    _wait_bgp_established(rt, "r2", peer_v4=R1_V4)
    _wait_static_in_rib(rt, "r1")


# ---------------------------------------------------------------------------
# 测试主流程
# ---------------------------------------------------------------------------


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    baseline = {"r1": _baseline("r1"), "r2": _baseline("r2")}
    container = rt.container_name("r1")

    try:
        _cleanup(rt, baseline)
        _run_inner(rt, container)
    finally:
        step("cleanup: 复位 BGP / 静态路由 / VRF 绑定 / VRF 配置")
        _cleanup(rt, baseline)

    print("Business module reboot lifecycle check passed.")


def _run_inner(rt: TopologyRuntime, container: str) -> None:
    # ============================ Phase A: baseline ============================
    step("Phase A: 两端建立 VRF blue + 绑定 GE-1 + 配 IPv4")
    _setup_vrf_and_link(rt, device="r1", local_v4=R1_V4, rd=f"{R1_AS}:1")
    _setup_vrf_and_link(rt, device="r2", local_v4=R2_V4, rd=f"{R2_AS}:1")
    _wait_direct_in_rib(rt, "r1")
    _wait_direct_in_rib(rt, "r2")

    step(f"Phase A: 配置 BGP（r1 AS {R1_AS} ↔ r2 AS {R2_AS}，vrf blue 子视图）")
    _setup_bgp(rt, device="r1", local_as=R1_AS, rid=R1_RID, peer_v4=R2_V4, peer_as=R2_AS)
    _setup_bgp(rt, device="r2", local_as=R2_AS, rid=R2_RID, peer_v4=R1_V4, peer_as=R1_AS)
    _wait_bgp_established(rt, "r1", peer_v4=R2_V4)
    _wait_bgp_established(rt, "r2", peer_v4=R1_V4)

    step(f"Phase A: r1 在 vrf {VRF_NAME} 内配置静态路由 {STATIC_PREFIX} → {STATIC_NH}")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"route static ipv4 vrf {VRF_NAME} {STATIC_PREFIX_ADDR} {STATIC_PREFIX_LEN} {STATIC_NH}",
            "end",
        ],
    )
    _wait_static_in_rib(rt, "r1")

    # ======================== Phase B: reboot bgp ============================
    step("Phase B: process reboot bgp on r1")
    _reboot_and_verify(rt, container, module="bgp", binary="netnexus-bgp")
    step("Phase B: 业务全部就绪（BGP 重订阅 VRF/IF/ROUTE 事件，DB 恢复 neighbor）")
    _assert_all_business_ok(rt)

    # ======================== Phase C: reboot route ===========================
    step("Phase C: process reboot route on r1")
    _reboot_and_verify(rt, container, module="route", binary="netnexus-route")
    step("Phase C: 业务全部就绪（ROUTE 重订阅 VRF/IF 事件，DB 恢复静态路由）")
    _assert_all_business_ok(rt)

    # ======================== Phase D: reboot if ==============================
    step("Phase D: process reboot if on r1")
    _reboot_and_verify(rt, container, module="if", binary="netnexus-if")
    step("Phase D: 业务全部就绪（IF 重订阅 VRF/ROUTE 事件，DB 恢复 GE-1 vrf 绑定 + IP）")
    _assert_all_business_ok(rt)
