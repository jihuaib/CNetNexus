#!/usr/bin/env python3
"""
LDP + ISIS IGP integration scenario.

Topology: r1 --- GE-1 --- r2

Real-world LDP deployments use loopbacks as LSR-IDs and rely on an IGP (ISIS
here) to make those loopbacks reachable across the network. This case verifies:

- ISIS converges and advertises both routers' loopback host routes
- LDP brings up its session over the GE-1 link transport (decoupled from IGP)
- LDP applies its full ROUTE subscription: labels are advertised both for the
  connected FECs and for the ISIS-learned remote loopback FECs
- Each side's remote LIB carries labels from the peer for the loopback FECs,
  including the labels for prefixes the peer learned via ISIS
- Cleanup tears down LDP, ISIS and loopback config

Tunnel candidate registration uses the simplified rule
``peer_lsr_id == route nexthop`` and is therefore best-effort here, since
ISIS injects nexthops as the peer's GE-1 IP rather than its LSR-ID.
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"

ISIS_TAG = 100
R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"

R1_LSR_ID = "1.1.1.1"
R2_LSR_ID = "2.2.2.2"

R1_LOOP_ID = 11
R2_LOOP_ID = 22
R1_LOOP_V4 = "10.255.1.1"
R2_LOOP_V4 = "10.255.2.2"
LOOP_V4_LEN = 32

# 加快收敛
HELLO_INTERVAL_MS = 1000
HOLD_TIME_MS = 3000
KEEPALIVE_INTERVAL_MS = 3000


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup LDP/ISIS/loopback config")
    for dev, loop in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"if {GE_IF}",
                "no ldp enable",
                "exit",
                f"if loop {loop}",
                "no ldp enable",
                "exit",
                "no ldp",
                f"no isis {ISIS_TAG}",
                f"no if loop {loop}",
                "end",
            ],
        )


def _configure(
    rt: TopologyRuntime,
    *,
    device: str,
    isis_net: str,
    lsr_id: str,
    loop_id: int,
    loop_v4: str,
) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if loop {loop_id}",
            f"ip address {loop_v4} {LOOP_V4_LEN}",
            "exit",
            f"isis {ISIS_TAG}",
            f"net {isis_net}",
            "is-type level-1-2",
            "af ipv4",
            "exit",
            f"if {GE_IF}",
            f"isis enable {ISIS_TAG}",
            f"isis hello-interval {ISIS_TAG} 3",
            f"isis hold-multiplier {ISIS_TAG} 3",
            "exit",
            f"if loop {loop_id}",
            f"isis enable {ISIS_TAG}",
            f"isis passive {ISIS_TAG}",
            "exit",
            "ldp",
            f"lsr-id {lsr_id}",
            f"hello-interval {HELLO_INTERVAL_MS}",
            f"hold-time {HOLD_TIME_MS}",
            f"keepalive-interval {KEEPALIVE_INTERVAL_MS}",
            "exit",
            f"if {GE_IF}",
            "ldp enable",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_ip4 = str(g_top.r1.GE_1.ip)
    r2_ip4 = str(g_top.r2.GE_1.ip)

    try:
        _cleanup(rt)

        step("Ensure GE-1 baseline up")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                    ],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "r2",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                    ],
                    "label": "r2 GE-1 up",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure ISIS + LDP + loopback on both routers")
        _configure(rt, device="r1", isis_net=R1_NET, lsr_id=R1_LSR_ID, loop_id=R1_LOOP_ID, loop_v4=R1_LOOP_V4)
        _configure(rt, device="r2", isis_net=R2_NET, lsr_id=R2_LSR_ID, loop_id=R2_LOOP_ID, loop_v4=R2_LOOP_V4)

        step("Wait ISIS neighbor up + loopback prefixes learned cross-router")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis neighbor {ISIS_TAG}",
                    "contains": [GE_IF, "Up"],
                    "label": "r1 isis neighbor up",
                },
                {
                    "device": "r2",
                    "command": f"show isis neighbor {ISIS_TAG}",
                    "contains": [GE_IF, "Up"],
                    "label": "r2 isis neighbor up",
                },
                {
                    "device": "r1",
                    "command": "show route ipv4 proto isis",
                    "contains": [f"{R2_LOOP_V4}/{LOOP_V4_LEN}"],
                    "label": "r1 has r2 loopback via ISIS",
                },
                {
                    "device": "r2",
                    "command": "show route ipv4 proto isis",
                    "contains": [f"{R1_LOOP_V4}/{LOOP_V4_LEN}"],
                    "label": "r2 has r1 loopback via ISIS",
                },
            ],
            timeout=80,
            interval=2,
        )

        step("Wait LDP session OPERATIONAL")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp neighbor",
                    "contains": [R2_LSR_ID, GE_IF, "OPERATIONAL"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(R2_LSR_ID)}\s+0\s+{re.escape(GE_IF)}\s+\S+\s+OPERATIONAL\b"
                    ],
                    "not_contains": ["No LDP adjacency"],
                    "label": "r1 sees r2 OPERATIONAL",
                },
                {
                    "device": "r2",
                    "command": "show ldp neighbor",
                    "contains": [R1_LSR_ID, GE_IF, "OPERATIONAL"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(R1_LSR_ID)}\s+0\s+{re.escape(GE_IF)}\s+\S+\s+OPERATIONAL\b"
                    ],
                    "not_contains": ["No LDP adjacency"],
                    "label": "r2 sees r1 OPERATIONAL",
                },
            ],
            timeout=60,
            interval=2,
        )

        step("Verify LDP advertises labels for both connected and ISIS-learned FECs")
        # 双方均应在 remote LIB 中持有对端发来的 r1_loop / r2_loop 两个 FEC 的 label。
        # 这是 ROUTE_PROTOCOL_MAX 订阅的关键证据：LDP 不只为本地 connected FEC 申请 label，
        # 也会为 ISIS 注入的远端 loopback FEC 申请 label 并互相通告。
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp binding",
                    "contains": ["Local Label Information Base", "Remote Label Information Base"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(R1_LOOP_V4)}/{LOOP_V4_LEN}\s+\d+\s*$",
                        rf"(?im)^\s*{re.escape(R2_LOOP_V4)}/{LOOP_V4_LEN}\s+\d+\s*$",
                        rf"(?im)^\s*{re.escape(R2_LSR_ID)}\s+{re.escape(R1_LOOP_V4)}/{LOOP_V4_LEN}\s+\d+\s*$",
                        rf"(?im)^\s*{re.escape(R2_LSR_ID)}\s+{re.escape(R2_LOOP_V4)}/{LOOP_V4_LEN}\s+\d+\s*$",
                    ],
                    "not_contains": ["No LDP label binding"],
                    "label": "r1 binding has labels for both loopback FECs (local+remote)",
                },
                {
                    "device": "r2",
                    "command": "show ldp binding",
                    "contains": ["Local Label Information Base", "Remote Label Information Base"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(R1_LOOP_V4)}/{LOOP_V4_LEN}\s+\d+\s*$",
                        rf"(?im)^\s*{re.escape(R2_LOOP_V4)}/{LOOP_V4_LEN}\s+\d+\s*$",
                        rf"(?im)^\s*{re.escape(R1_LSR_ID)}\s+{re.escape(R1_LOOP_V4)}/{LOOP_V4_LEN}\s+\d+\s*$",
                        rf"(?im)^\s*{re.escape(R1_LSR_ID)}\s+{re.escape(R2_LOOP_V4)}/{LOOP_V4_LEN}\s+\d+\s*$",
                    ],
                    "not_contains": ["No LDP label binding"],
                    "label": "r2 binding has labels for both loopback FECs (local+remote)",
                },
            ],
            timeout=80,
            interval=3,
        )

        step("TUNNEL candidate registration via M6 link-addr matching")
        # M6：peer ↔ nexthop 匹配扩展为三选一（lsr_id / transport_v4 / link_addr_v4）。
        # ISIS 注入的 nexthop 为对端 GE-1 IP（10.12.0.x），即对端 hello 包的源 IP，
        # 命中 link_addr_v4 规则，candidate 必然注册。
        for dev, dev_label in (("r1", "r1"), ("r2", "r2")):
            wait_check(
                rt,
                device=dev,
                command="show tunnel candidate",
                timeout=15,
                interval=2,
                regex=[r"(?im)\bsrc\s+ldp\b"],
                label=f"{dev_label} ldp tunnel candidate present",
            )

        # 顺便确认两个 ldp 监控展示在 IGP 帮助下的 GE-1 接口仍然 up
        step("Sanity: show ldp interface still healthy after IGP integration")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp interface",
                    "contains": [GE_IF, r1_ip4, "up"],
                    "label": "r1 ldp interface up",
                },
                {
                    "device": "r2",
                    "command": "show ldp interface",
                    "contains": [GE_IF, r2_ip4, "up"],
                    "label": "r2 ldp interface up",
                },
            ],
            timeout=20,
            interval=2,
        )

        print("LDP + ISIS integration scenario passed.")
    finally:
        _cleanup(rt)
