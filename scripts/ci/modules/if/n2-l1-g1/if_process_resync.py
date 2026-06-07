#!/usr/bin/env python3
"""
IF 进程生命周期对业务模块（ROUTE / BGP）再同步的端到端验证。

拓扑：r1 ←GE-1→ r2（IPv4 单链路，public VRF 10.12.0.x/30），两端拉起 EBGP。

覆盖：
  Phase A. baseline 建立：r1/r2 GE-1 IPv4 + EBGP + r1 静态路由
           - GE-1 IPv4 10.12.0.1/30 / 10.12.0.2/30
           - BGP r1 (65001) ←→ r2 (65002) ipv4-unicast Established
           - r1 静态路由 203.0.113.0/24 → 10.12.0.2
  Phase B. process reboot if：r1 IF 进程更替
           - 旧 PID 消失，新 PID 出现
           - IF replay 完成（IF_SMOOTHEND）后业务模块 db_restore 触发
           - GE-1 IPv4 从 DB 重新恢复，直连路由就绪
           - BGP 经过短暂 DOWN 后 re-establish
           - 静态路由从 RIB 暂时移除然后随 ROUTE DB 重恢复回来
  Phase C. process stop if：r1 IF 进程离场
           - IF PID 消失
           - 业务模块清掉 IF 内存态：BGP 会话拆掉、静态路由 NH 不可达后退出 RIB
           - DB 中 GE-1 IP / 静态路由配置仍保留
  Phase D. process start if：r1 IF 重新启动
           - 新 PID 出现，GE-1 IP 重新下发
           - 直连路由 / BGP 邻居 / 静态路由 全部恢复

清理：拆 BGP / 静态路由 / 复位接口 IPv4。
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
    wait_fib_route,
)
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"

# baseline 直连地址（top.yaml 配置）
R1_V4 = "10.12.0.1"
R2_V4 = "10.12.0.2"
V4_LEN = 30
V4_NET = "10.12.0.0"

# r1 静态路由
STATIC_PREFIX_ADDR = "203.0.113.0"
STATIC_PREFIX_LEN = 24
STATIC_PREFIX = f"{STATIC_PREFIX_ADDR}/{STATIC_PREFIX_LEN}"
STATIC_NH = R2_V4

R1_AS = 65001
R2_AS = 65002
R1_RID = "1.1.1.1"
R2_RID = "2.2.2.2"

WAIT_PID_SEC = 15       # process reboot/start 进程更替超时
WAIT_GONE_SEC = 15      # process stop 进程消失超时
WAIT_IF_READY_SEC = 30  # IF 启动 + 业务再同步
WAIT_BGP_UP_SEC = 60    # BGP 回到 Established
WAIT_ROUTE_SEC = 30     # 静态/直连路由 出现/消失


# ---------------------------------------------------------------------------
# 进程探针
# ---------------------------------------------------------------------------


def _list_if_pids(container: str) -> list[int]:
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", "netnexus-if"],
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
        pids = _list_if_pids(container)
        if predicate(pids):
            return pids
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting {what}; last if pids={pids}")


# ---------------------------------------------------------------------------
# 业务断言
# ---------------------------------------------------------------------------


def _wait_if_ip(rt: TopologyRuntime, device: str, *, local_v4: str, timeout: int = WAIT_IF_READY_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show if {GE_IF}",
        timeout=timeout,
        interval=2,
        contains=[
            f"Interface {GE_IF} Detail:",
            f"IPv4 Addr  : {local_v4}/{V4_LEN}",
        ],
        regex=[r"(?im)^\s*(?:Proto\s+)?State\s*:\s*UP\s*$"],
        label=f"{device} {GE_IF} IPv4 {local_v4}/{V4_LEN}",
    )


def _wait_if_process_gone(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_GONE_SEC) -> None:
    """IF 进程不在时，show if 命令应该报失败（target not running 或 timeout）。"""
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, f"show if {GE_IF}", strict=False, timeout=5)
        last = out
        if f"Interface {GE_IF} Detail:" not in out:
            return
        time.sleep(1)
    raise AssertionError(f"timeout waiting {device} show if to fail; last:\n{last}")


def _wait_bgp_session(
    rt: TopologyRuntime,
    device: str,
    *,
    peer_v4: str,
    expect_established: bool,
    timeout: int = WAIT_BGP_UP_SEC,
) -> None:
    cmd_str = "show bgp neighbor af ipv4-unicast"
    if expect_established:
        wait_check(
            rt,
            device=device,
            command=cmd_str,
            timeout=timeout,
            interval=2,
            contains=[peer_v4, "AF: ipv4-unicast"],
            regex=[rf"(?im)^\s*{re.escape(peer_v4)}\s+\S+\s+\S+\s+Established\s*$"],
            label=f"{device} bgp {peer_v4} Established",
        )
        return

    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, cmd_str, strict=False, timeout=5)
        last = out
        if re.search(rf"(?im)^\s*{re.escape(peer_v4)}\s+\S+\s+\S+\s+Established\s*$", out) is None:
            return
        time.sleep(2)
    raise AssertionError(f"timeout waiting {device} bgp session to drop; last:\n{last}")


def _wait_static_in_rib(
    rt: TopologyRuntime,
    device: str,
    *,
    expect_present: bool,
    timeout: int = WAIT_ROUTE_SEC,
) -> None:
    cmd_str = f"show route ipv4 {STATIC_PREFIX_ADDR} {STATIC_PREFIX_LEN}"
    route_header = rf"(?im)^\s*Routing entry for {re.escape(STATIC_PREFIX)}"
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
            label=f"{device} static {STATIC_PREFIX} present",
        )
        return

    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, cmd_str, strict=False, timeout=5)
        last = out
        if re.search(static_path, out) is None:
            return
        time.sleep(2)
    raise AssertionError(f"timeout waiting static {STATIC_PREFIX} to leave RIB; last:\n{last}")


def _wait_direct_in_rib(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_ROUTE_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {V4_NET} {V4_LEN}",
        timeout=timeout,
        interval=2,
        regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
        label=f"{device} direct {V4_NET}/{V4_LEN}",
    )


def _wait_fib_absent(rt: TopologyRuntime, device: str, *, dest: str, plen: int, timeout: int = WAIT_ROUTE_SEC) -> None:
    wait_fib_route(
        rt,
        device=device,
        afi="ipv4",
        prefix_addr=dest,
        prefix_len=plen,
        expect_present=False,
        timeout=timeout,
        interval=2,
        label=f"{device} FIB {dest}/{plen} absent",
    )


def _wait_fib_present(rt: TopologyRuntime, device: str, *, dest: str, plen: int, timeout: int = WAIT_ROUTE_SEC) -> None:
    wait_fib_route(
        rt,
        device=device,
        afi="ipv4",
        prefix_addr=dest,
        prefix_len=plen,
        expect_present=True,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} FIB {dest}/{plen} present",
    )


def _wait_os_route(rt: TopologyRuntime, device: str, *, prefix: str, present: bool, timeout: int = WAIT_ROUTE_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command="show fib os ipv4",
        timeout=timeout,
        interval=2,
        contains=[prefix] if present else [],
        not_contains=[] if present else [prefix],
        label=f"{device} OS FIB {prefix} {'present' if present else 'absent'}",
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
                f"no route static ipv4 {STATIC_PREFIX_ADDR} {STATIC_PREFIX_LEN} {STATIC_NH}"
            )
        # 接口 IPv4 已是 baseline 的值，不需要改动；保险起见 reapply
        commands += [
            f"if {GE_IF}",
            "no shutdown",
            f"ip address {b['v4']} {b['v4_len']}",
            "exit",
            "end",
        ]
        run_cmds(rt=rt, device=dev, strict=False, commands=commands)


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
            f"neighbor {peer_v4} as {peer_as}",
            "af ipv4-unicast",
            f"neighbor {peer_v4} enable",
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
        step("cleanup: 复位 BGP / 静态路由 / 接口 IPv4")
        if not _list_if_pids(container):
            try:
                process_start(rt, "r1", "if")
                _wait_pid(container, predicate=lambda p: len(p) == 1, timeout=WAIT_PID_SEC,
                          what="if pid after cleanup-start")
            except Exception as e:
                print(f"cleanup warn: failed to restart if before teardown: {e}", flush=True)
        _cleanup(rt, baseline)

    print("IF process lifecycle resync (reboot/stop/start) check passed.")


def _run_inner(rt: TopologyRuntime, container: str) -> None:
    # ============================ Phase A: baseline ============================
    step("Phase A: 等接口 IPv4 就绪（top.yaml 已下发）")
    _wait_if_ip(rt, "r1", local_v4=R1_V4)
    _wait_if_ip(rt, "r2", local_v4=R2_V4)
    _wait_direct_in_rib(rt, "r1")
    _wait_direct_in_rib(rt, "r2")

    step(f"Phase A: 配置两端 BGP（r1 AS {R1_AS} ↔ r2 AS {R2_AS}）")
    _setup_bgp(rt, device="r1", local_as=R1_AS, rid=R1_RID, peer_v4=R2_V4, peer_as=R2_AS)
    _setup_bgp(rt, device="r2", local_as=R2_AS, rid=R2_RID, peer_v4=R1_V4, peer_as=R1_AS)
    _wait_bgp_session(rt, "r1", peer_v4=R2_V4, expect_established=True)
    _wait_bgp_session(rt, "r2", peer_v4=R1_V4, expect_established=True)

    step(f"Phase A: r1 配置静态路由 {STATIC_PREFIX} → {STATIC_NH}")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"route static ipv4 {STATIC_PREFIX_ADDR} {STATIC_PREFIX_LEN} {STATIC_NH}",
            "end",
        ],
    )
    _wait_static_in_rib(rt, "r1", expect_present=True)

    pids_before = _list_if_pids(container)
    if len(pids_before) != 1:
        mark_step_failed()
        raise AssertionError(f"Phase A: expected exactly 1 if pid on r1, got {pids_before}")
    pid_phase_a = pids_before[0]
    print(f"[phase A] r1 if pid = {pid_phase_a}", flush=True)

    # ============================ Phase B: reboot ============================
    step(f"Phase B: process reboot if on r1 (old pid={pid_phase_a})")
    out = process_reboot(rt, "r1", "if")
    if "reboot if ok" not in out and "spawned" not in out:
        mark_step_failed()
        raise AssertionError(f"Phase B: unexpected `process reboot if` response:\n{out}")

    _wait_pid(container, predicate=lambda p: pid_phase_a not in p, timeout=WAIT_PID_SEC,
              what=f"old if pid {pid_phase_a} to exit")
    new_pids = _wait_pid(container, predicate=lambda p: len(p) == 1 and p[0] != pid_phase_a,
                         timeout=WAIT_PID_SEC, what="new if pid after reboot")
    pid_phase_b = new_pids[0]
    print(f"[phase B] r1 if pid: {pid_phase_a} → {pid_phase_b}", flush=True)

    step("Phase B: IF smoothend 后所有业务恢复（接口 IP / 直连 / BGP / 静态路由）")
    _wait_if_ip(rt, "r1", local_v4=R1_V4)
    _wait_direct_in_rib(rt, "r1")
    _wait_bgp_session(rt, "r1", peer_v4=R2_V4, expect_established=True)
    _wait_bgp_session(rt, "r2", peer_v4=R1_V4, expect_established=True)
    _wait_static_in_rib(rt, "r1", expect_present=True)

    # ============================ Phase C: stop ============================
    step(f"Phase C: process stop if on r1 (pid={pid_phase_b})")
    out = process_stop(rt, "r1", "if")
    out_l = out.lower()
    if "stop if requested" not in out_l and "stop if ok" not in out_l:
        mark_step_failed()
        raise AssertionError(f"Phase C: unexpected `process stop if` response:\n{out}")

    _wait_pid(container, predicate=lambda p: not p, timeout=WAIT_GONE_SEC,
              what=f"if pid {pid_phase_b} to exit on stop")
    print("[phase C] r1 if process gone", flush=True)

    step("Phase C: IF 优雅退出 → 撤 OS IP → ROUTE 撤直连 → 静态路由从 FIB 撤出 → 内核路由消失")
    _wait_if_process_gone(rt, "r1")
    # ROUTE 已撤 FIB（包括直连 10.12.0.0/30 和静态 203.0.113.0/24）
    _wait_fib_absent(rt, "r1", dest=V4_NET, plen=V4_LEN)
    _wait_fib_absent(rt, "r1", dest=STATIC_PREFIX_ADDR, plen=STATIC_PREFIX_LEN)
    # 内核里两条路由也都消失
    _wait_os_route(rt, "r1", prefix=f"{V4_NET}/{V4_LEN}", present=False)
    _wait_os_route(rt, "r1", prefix=STATIC_PREFIX, present=False)

    # ============================ Phase D: start ============================
    step("Phase D: process start if on r1")
    out = process_start(rt, "r1", "if")
    if "start if ok" not in out and "already running" not in out:
        mark_step_failed()
        raise AssertionError(f"Phase D: unexpected `process start if` response:\n{out}")

    started_pids = _wait_pid(container, predicate=lambda p: len(p) == 1, timeout=WAIT_PID_SEC,
                              what="if pid after start")
    pid_phase_d = started_pids[0]
    print(f"[phase D] r1 if pid: 0 → {pid_phase_d}", flush=True)

    step("Phase D: 业务从 DB 重恢复，邻居 / 静态路由再次就绪")
    _wait_if_ip(rt, "r1", local_v4=R1_V4)
    _wait_direct_in_rib(rt, "r1")
    _wait_bgp_session(rt, "r1", peer_v4=R2_V4, expect_established=True)
    _wait_bgp_session(rt, "r2", peer_v4=R1_V4, expect_established=True)
    _wait_static_in_rib(rt, "r1", expect_present=True)

    print(
        f"PID timeline r1.if: phaseA={pid_phase_a} -> reboot -> {pid_phase_b} -> stop -> (gone) -> start -> {pid_phase_d}",
        flush=True,
    )
