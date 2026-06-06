#!/usr/bin/env python3
"""
VRF 进程生命周期对业务模块（IF / ROUTE / BGP）再同步的端到端验证。

拓扑：r1 ←GE-1→ r2（IPv4 单链路），两端 VRF blue 内拉起 EBGP 直连邻居。

覆盖：
  Phase A. baseline 建立：两端 VRF blue + GE-1 IPv4 + EBGP 邻居 + r1 静态路由
           - IF 在 VRF blue 内绑定 10.99.0.1/30；VRF 内直连路由就绪
           - BGP r1 (65001) ←→ r2 (65002) 在 vrf blue 下 IPv4-unicast Established
           - r1 在 vrf blue 配置静态路由 203.0.113.0/24 → 10.99.0.2
  Phase B. process reboot vrf：r1 VRF 进程更替
           - 旧 PID 消失，新 PID 出现
           - VRF blue UP / GE-1 vrf 绑定恢复
           - BGP 会话经过 DOWN → re-establish 后回到 Established
           - 静态路由从 RIB 暂时移除（DOWN 时 worker 清非 public）然后从 DB 恢复
  Phase C. process stop vrf：r1 VRF 进程离场
           - VRF PID 消失
           - 业务模块清掉 vrf 相关内存态：GE-1 vrf_name 清空 / BGP vrf blue 邻居拆除 /
             静态路由从 RIB 撤出
           - DB 中 vrf blue / interface vrf_name / 静态路由配置仍然存在
  Phase D. process start vrf：r1 VRF 重新启动
           - 新 PID 出现，VRF blue 重新 UP
           - GE-1 重新绑入 vrf blue，重新拥有 10.99.0.1/30
           - BGP 邻居再次 Established
           - 静态路由再次出现在 vrf blue RIB

清理：删除 VRF / BGP / 静态路由 / 复位接口公网地址。
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
    process_start,
    process_stop,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "blue"
GE_IF = "GE-1"

# VRF blue 内重新分配的接口地址（绑入 VRF 后原有 IP 会被清空）
R1_V4 = "10.99.0.1"
R2_V4 = "10.99.0.2"
V4_LEN = 30
V4_NET = "10.99.0.0"  # 直连子网

# r1 在 vrf blue 内配置的静态路由
STATIC_PREFIX_ADDR = "203.0.113.0"
STATIC_PREFIX_LEN = 24
STATIC_PREFIX = f"{STATIC_PREFIX_ADDR}/{STATIC_PREFIX_LEN}"
STATIC_NH = R2_V4  # nexthop 走 vrf blue 内的直连邻居

R1_AS = 65001
R2_AS = 65002
R1_RID = "1.1.1.1"
R2_RID = "2.2.2.2"

# 等待相关常量
WAIT_PID_SEC = 12       # process reboot/start 进程更替超时
WAIT_GONE_SEC = 15      # process stop 进程消失超时
WAIT_VRF_READY_SEC = 25 # VRF 启动后 OS State UP + 业务模块再同步
WAIT_BGP_UP_SEC = 60    # BGP 会话回到 Established
WAIT_ROUTE_SEC = 25     # 静态路由 / 直连路由 出现/消失


# ---------------------------------------------------------------------------
# 进程探针
# ---------------------------------------------------------------------------


def _list_vrf_pids(container: str) -> list[int]:
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", "netnexus-vrf"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(f"pgrep failed: {proc.stderr}")
    return [int(x) for x in proc.stdout.split() if x.strip().isdigit()]


def _wait_pid(container: str, *, predicate, timeout: float, what: str) -> list[int]:
    deadline = time.monotonic() + timeout
    pids: list[int] = []
    while time.monotonic() < deadline:
        pids = _list_vrf_pids(container)
        if predicate(pids):
            return pids
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting {what}; last vrf pids={pids}")


# ---------------------------------------------------------------------------
# 业务断言：VRF / IF / BGP / 静态路由
# ---------------------------------------------------------------------------


def _wait_vrf_up(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_VRF_READY_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show vrf name {VRF_NAME}",
        timeout=timeout,
        interval=1,
        contains=[
            "VRF Detail:",
            f"Name           : {VRF_NAME}",
            "OS State       : UP",
        ],
        label=f"{device} vrf {VRF_NAME} UP",
    )


def _wait_vrf_gone(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_GONE_SEC) -> None:
    """VRF 进程不在时，show vrf name 应当报 not found / 命令无响应。"""
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, f"show vrf name {VRF_NAME}", strict=False, timeout=5)
        last = out
        if "VRF Detail:" not in out:
            return
        time.sleep(1)
    raise AssertionError(f"timeout waiting {device} vrf {VRF_NAME} to disappear; last:\n{last}")


def _wait_if_bound(rt: TopologyRuntime, device: str, local_v4: str, *, timeout: int = WAIT_VRF_READY_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show if {GE_IF}",
        timeout=timeout,
        interval=2,
        contains=[
            f"Interface {GE_IF} Detail:",
            f"VRF        : {VRF_NAME}",
            f"IPv4 Addr  : {local_v4}/{V4_LEN}",
        ],
        regex=[r"(?im)^\s*(?:Proto\s+)?State\s*:\s*UP\s*$"],
        label=f"{device} {GE_IF} bound to vrf {VRF_NAME}",
    )


def _wait_if_unbound(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_VRF_READY_SEC) -> None:
    """VRF 进程死亡后 IF worker 应清掉 vrf_name + IP（内存态）。"""
    wait_check(
        rt,
        device=device,
        command=f"show if {GE_IF}",
        timeout=timeout,
        interval=2,
        contains=[f"Interface {GE_IF} Detail:"],
        not_regex=[rf"(?im)^\s*VRF\s*:\s*{re.escape(VRF_NAME)}\s*$"],
        label=f"{device} {GE_IF} unbound from vrf {VRF_NAME}",
    )


def _wait_bgp_session(
    rt: TopologyRuntime,
    device: str,
    *,
    peer_v4: str,
    expect_established: bool,
    timeout: int = WAIT_BGP_UP_SEC,
) -> None:
    cmd_str = f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}"
    if expect_established:
        wait_check(
            rt,
            device=device,
            command=cmd_str,
            timeout=timeout,
            interval=2,
            contains=[peer_v4, "AF: ipv4-unicast"],
            regex=[rf"(?im)^\s*{re.escape(peer_v4)}\s+\S+\s+\S+\s+Established\s*$"],
            label=f"{device} vrf={VRF_NAME} ipv4 session up",
        )
        return

    # 期望非 Established（VRF 进程停掉的场景下整张 VRF 业务都会消失）
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, cmd_str, strict=False, timeout=5)
        last = out
        # vrf blue 已被 BGP worker 在 VRF DOWN 时拆掉 → 命令报 VRF not found 或邻居列表里看不到 peer
        if re.search(rf"(?im)^\s*{re.escape(peer_v4)}\s+\S+\s+\S+\s+Established\s*$", out) is None:
            return
        time.sleep(2)
    raise AssertionError(f"timeout waiting {device} bgp session in vrf {VRF_NAME} to drop; last:\n{last}")


def _wait_static_in_rib(
    rt: TopologyRuntime,
    device: str,
    *,
    expect_present: bool,
    timeout: int = WAIT_ROUTE_SEC,
) -> None:
    cmd_str = f"show route ipv4 vrf {VRF_NAME} {STATIC_PREFIX_ADDR} {STATIC_PREFIX_LEN}"
    route_header = rf"(?im)^\s*Routing entry for {re.escape(STATIC_PREFIX)} \(VRF: {re.escape(VRF_NAME)}\)\s*$"
    static_path = r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"

    if expect_present:
        wait_check(
            rt,
            device=device,
            command=cmd_str,
            timeout=timeout,
            interval=2,
            regex=[route_header, static_path],
            not_contains=["(no routes)", "(no matching routes)"],
            label=f"{device} static {STATIC_PREFIX} in vrf {VRF_NAME} present",
        )
        return

    # 期望 absent：可能整张 VRF 消失（命令直接报 VRF not found），也可能 vrf 还在但路由被 worker 拆掉
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, cmd_str, strict=False, timeout=5)
        last = out
        if re.search(static_path, out) is None:
            return
        time.sleep(2)
    raise AssertionError(f"timeout waiting static {STATIC_PREFIX} to leave RIB; last:\n{last}")


def _wait_direct_in_rib(
    rt: TopologyRuntime,
    device: str,
    *,
    timeout: int = WAIT_ROUTE_SEC,
) -> None:
    cmd_str = f"show route ipv4 vrf {VRF_NAME} {V4_NET} {V4_LEN}"
    wait_check(
        rt,
        device=device,
        command=cmd_str,
        timeout=timeout,
        interval=2,
        regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
        label=f"{device} direct {V4_NET}/{V4_LEN} in vrf {VRF_NAME}",
    )


# ---------------------------------------------------------------------------
# 拓扑准备 / 清理
# ---------------------------------------------------------------------------


def _baseline(device: str) -> dict[str, str | int]:
    g = getattr(g_top, device)
    return {
        "v4": str(g.GE_1.ip),
        "v4_len": int(g.GE_1.prefix),
    }


def _cleanup(rt: TopologyRuntime, baseline: dict[str, dict[str, str | int]]) -> None:
    """best-effort 恢复：拆 BGP / 静态路由 / VRF 绑定 / VRF 配置。"""
    for dev in ("r1", "r2"):
        b = baseline[dev]
        commands = [
            "end",
            "config",
            "no bgp",
        ]
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
    """建立 VRF + AF + RD + 绑接口 + 接口 IP。"""
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
    # VRF_ADD/AF_ENABLE/RD 需要传播到 IF/BGP cache
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
    _wait_if_bound(rt, device, local_v4)


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
        # 若 stop 阶段失败导致 VRF 未运行，先确保进程在跑，否则 cleanup 命令会失败
        if not _list_vrf_pids(container):
            try:
                process_start(rt, "r1", "vrf")
                _wait_pid(container, predicate=lambda p: len(p) == 1, timeout=WAIT_PID_SEC,
                          what="vrf pid after cleanup-start")
            except Exception as e:
                print(f"cleanup warn: failed to restart vrf before teardown: {e}", flush=True)
        _cleanup(rt, baseline)

    print("VRF process lifecycle resync (reboot/stop/start) check passed.")


def _run_inner(rt: TopologyRuntime, container: str) -> None:
    # ============================ Phase A: baseline ============================
    step("Phase A: 在两端建立 VRF blue + 绑定 GE-1 + 配 IPv4")
    _setup_vrf_and_link(rt, device="r1", local_v4=R1_V4, rd=f"{R1_AS}:1")
    _setup_vrf_and_link(rt, device="r2", local_v4=R2_V4, rd=f"{R2_AS}:1")

    step("Phase A: 等待两端 vrf blue 直连路由就绪")
    _wait_direct_in_rib(rt, "r1")
    _wait_direct_in_rib(rt, "r2")

    step(f"Phase A: 配置两端 BGP（r1 AS {R1_AS} ↔ r2 AS {R2_AS}，vrf blue 子视图）")
    _setup_bgp(rt, device="r1", local_as=R1_AS, rid=R1_RID, peer_v4=R2_V4, peer_as=R2_AS)
    _setup_bgp(rt, device="r2", local_as=R2_AS, rid=R2_RID, peer_v4=R1_V4, peer_as=R1_AS)

    step("Phase A: 等待 BGP 双向 Established")
    _wait_bgp_session(rt, "r1", peer_v4=R2_V4, expect_established=True)
    _wait_bgp_session(rt, "r2", peer_v4=R1_V4, expect_established=True)

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
    _wait_static_in_rib(rt, "r1", expect_present=True)

    pids_before = _list_vrf_pids(container)
    if len(pids_before) != 1:
        mark_step_failed()
        raise AssertionError(f"Phase A: expected exactly 1 vrf pid on r1, got {pids_before}")
    pid_phase_a = pids_before[0]
    print(f"[phase A] r1 vrf pid = {pid_phase_a}", flush=True)

    # ============================ Phase B: reboot ============================
    step(f"Phase B: process reboot vrf on r1 (old pid={pid_phase_a})")
    # process_reboot 已封装 cmd + wait_modules_ready
    out = process_reboot(rt, "r1", "vrf")
    if "reboot vrf ok" not in out and "spawned" not in out:
        mark_step_failed()
        raise AssertionError(f"Phase B: unexpected `process reboot vrf` response:\n{out}")

    _wait_pid(container, predicate=lambda p: pid_phase_a not in p, timeout=WAIT_PID_SEC,
              what=f"old vrf pid {pid_phase_a} to exit")
    new_pids = _wait_pid(container, predicate=lambda p: len(p) == 1 and p[0] != pid_phase_a,
                         timeout=WAIT_PID_SEC, what="new vrf pid after reboot")
    pid_phase_b = new_pids[0]
    print(f"[phase B] r1 vrf pid: {pid_phase_a} → {pid_phase_b}", flush=True)

    step("Phase B: r1 业务在 VRF 再同步后全部恢复")
    _wait_vrf_up(rt, "r1")
    _wait_if_bound(rt, "r1", R1_V4)
    _wait_direct_in_rib(rt, "r1")
    # BGP 会话经过短暂 DOWN 后由 worker 在 SMOOTHEND 触发的 vrf-bound restore 再次拉起
    _wait_bgp_session(rt, "r1", peer_v4=R2_V4, expect_established=True)
    _wait_bgp_session(rt, "r2", peer_v4=R1_V4, expect_established=True)
    _wait_static_in_rib(rt, "r1", expect_present=True)

    # ============================ Phase C: stop ============================
    step(f"Phase C: process stop vrf on r1 (pid={pid_phase_b})")
    out = process_stop(rt, "r1", "vrf")
    out_l = out.lower()
    if "stop vrf requested" not in out_l and "stop vrf ok" not in out_l:
        mark_step_failed()
        raise AssertionError(f"Phase C: unexpected `process stop vrf` response:\n{out}")

    _wait_pid(container, predicate=lambda p: not p, timeout=WAIT_GONE_SEC,
              what=f"vrf pid {pid_phase_b} to exit on stop")
    print("[phase C] r1 vrf process gone", flush=True)

    step("Phase C: r1 业务模块清掉非 public VRF 内存态")
    _wait_vrf_gone(rt, "r1")
    _wait_if_unbound(rt, "r1")
    _wait_bgp_session(rt, "r1", peer_v4=R2_V4, expect_established=False)
    _wait_static_in_rib(rt, "r1", expect_present=False)

    # ============================ Phase D: start ============================
    step("Phase D: process start vrf on r1")
    out = process_start(rt, "r1", "vrf")
    if "start vrf ok" not in out and "already running" not in out:
        mark_step_failed()
        raise AssertionError(f"Phase D: unexpected `process start vrf` response:\n{out}")

    started_pids = _wait_pid(container, predicate=lambda p: len(p) == 1, timeout=WAIT_PID_SEC,
                              what="vrf pid after start")
    pid_phase_d = started_pids[0]
    print(f"[phase D] r1 vrf pid: 0 → {pid_phase_d}", flush=True)

    step("Phase D: r1 业务从 DB 重恢复，邻居 / 静态路由再次就绪")
    _wait_vrf_up(rt, "r1")
    _wait_if_bound(rt, "r1", R1_V4)
    _wait_direct_in_rib(rt, "r1")
    _wait_bgp_session(rt, "r1", peer_v4=R2_V4, expect_established=True)
    _wait_bgp_session(rt, "r2", peer_v4=R1_V4, expect_established=True)
    _wait_static_in_rib(rt, "r1", expect_present=True)

    print(
        f"PID timeline r1.vrf: phaseA={pid_phase_a} -> reboot -> {pid_phase_b} -> stop -> (gone) -> start -> {pid_phase_d}",
        flush=True,
    )
