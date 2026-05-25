#!/usr/bin/env python3
"""
业务模块重启对 IF / ROUTE / BGP 业务恢复的端到端验证（public VRF）。

拓扑：r1 ←GE-1→ r2（IPv4 单链路，public VRF 10.12.0.x/30），两端拉起 EBGP。

baseline：
- r1/r2 GE-1 IPv4 10.12.0.1-2/30（top.yaml 已配）
- r1 AS 65001 ↔ r2 AS 65002 ipv4-unicast Established
- r1 静态路由 203.0.113.0/24 → 10.12.0.2

依次 reboot r1 上业务模块：
  Phase B. process reboot bgp on r1
           - r1 BGP pid 更替；会话再次 Established（IF/ROUTE 不受影响）
  Phase C. process reboot route on r1
           - r1 ROUTE pid 更替，static 从 DB 恢复，BGP 邻居最终回到 Established

清理：拆 BGP / 静态路由 / 复位接口 IPv4。
"""

from __future__ import annotations

import re
import subprocess
import time

from module_api import (  # noqa: E402
    g_top,
    mark_step_failed,
    process_reboot,
    require_devices,
    run_cmds,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"
R1_V4 = "10.12.0.1"
R2_V4 = "10.12.0.2"
V4_LEN = 30
V4_NET = "10.12.0.0"

STATIC_PREFIX_ADDR = "203.0.113.0"
STATIC_PREFIX_LEN = 24
STATIC_PREFIX = f"{STATIC_PREFIX_ADDR}/{STATIC_PREFIX_LEN}"
STATIC_NH = R2_V4

R1_AS = 65001
R2_AS = 65002
R1_RID = "1.1.1.1"
R2_RID = "2.2.2.2"

WAIT_PID_SEC = 15
WAIT_IF_READY_SEC = 30
WAIT_BGP_UP_SEC = 60
WAIT_ROUTE_SEC = 30


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
        command="show bgp neighbor af ipv4-unicast",
        timeout=timeout,
        interval=2,
        contains=[peer_v4, "AF: ipv4-unicast"],
        regex=[rf"(?im)^\s*{re.escape(peer_v4)}\s+\S+\s+\S+\s+Established\s*$"],
        label=f"{device} bgp {peer_v4} Established",
    )


def _wait_static_in_rib(rt: TopologyRuntime, device: str, *, timeout: int = WAIT_ROUTE_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {STATIC_PREFIX_ADDR} {STATIC_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        regex=[
            rf"(?im)^\s*Routing entry for {re.escape(STATIC_PREFIX)}",
            r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b",
        ],
        not_contains=["(no routes)", "(no matching routes)"],
        label=f"{device} static {STATIC_PREFIX} present",
    )


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


def _reboot_and_verify(
    rt: TopologyRuntime,
    container: str,
    *,
    module: str,
    binary: str,
) -> tuple[int, int]:
    before = _list_pids(container, binary)
    if len(before) != 1:
        mark_step_failed()
        raise AssertionError(f"expected exactly 1 {binary} pid, got {before}")
    old_pid = before[0]

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
    _wait_if_ip(rt, "r1", local_v4=R1_V4)
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
        step("cleanup: 复位 BGP / 静态路由 / 接口 IPv4")
        _cleanup(rt, baseline)

    print("IF-public business module reboot lifecycle check passed.")


def _run_inner(rt: TopologyRuntime, container: str) -> None:
    # ============================ Phase A: baseline ============================
    step("Phase A: 等接口 IPv4 就绪（top.yaml 已下发）")
    _wait_if_ip(rt, "r1", local_v4=R1_V4)
    _wait_if_ip(rt, "r2", local_v4=R2_V4)
    _wait_direct_in_rib(rt, "r1")
    _wait_direct_in_rib(rt, "r2")

    step(f"Phase A: 配置 BGP（r1 AS {R1_AS} ↔ r2 AS {R2_AS}）")
    _setup_bgp(rt, device="r1", local_as=R1_AS, rid=R1_RID, peer_v4=R2_V4, peer_as=R2_AS)
    _setup_bgp(rt, device="r2", local_as=R2_AS, rid=R2_RID, peer_v4=R1_V4, peer_as=R1_AS)
    _wait_bgp_established(rt, "r1", peer_v4=R2_V4)
    _wait_bgp_established(rt, "r2", peer_v4=R1_V4)

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
    _wait_static_in_rib(rt, "r1")

    # ======================== Phase B: reboot bgp ============================
    step("Phase B: process reboot bgp on r1")
    _reboot_and_verify(rt, container, module="bgp", binary="netnexus-bgp")
    step("Phase B: 业务全部就绪（BGP 重订阅 IF 事件后等 SMOOTHEND 再 db_restore）")
    _assert_all_business_ok(rt)

    # ======================== Phase C: reboot route ===========================
    step("Phase C: process reboot route on r1")
    _reboot_and_verify(rt, container, module="route", binary="netnexus-route")
    step("Phase C: 业务全部就绪（ROUTE 重订阅 IF 事件后等 SMOOTHEND 再 db_restore）")
    _assert_all_business_ok(rt)
