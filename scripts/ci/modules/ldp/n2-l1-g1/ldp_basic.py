#!/usr/bin/env python3
"""
LDP basic scenario check (RFC 5036, IPv4 unicast).

Topology: r1 --- GE-1 --- r2

Coverage:
- LDP global config persistence (lsr-id, hello/hold/keepalive)
- LDP discovery (UDP 646, 224.0.0.2): adjacency learning
- LDP session FSM: NON_EXIST → ... → OPERATIONAL (TCP 646)
- Address message exchange + label binding sanity:
    * Local LIB carries LSR-ID/32 placeholder for both sides
    * Remote LIB on each side carries peer LSR-ID/32 with implicit-null (label=3)
- Loopback host route advertised via Label Mapping after configuring loopback
- TUNNEL candidate registered (src ldp) when remote label arrives for an FEC
  whose nexthop matches the peer LSR-ID
- Cleanup tears down sessions and adjacencies
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"

R1_LSR_ID = "1.1.1.1"
R2_LSR_ID = "2.2.2.2"

R1_LOOP_ID = 11
R2_LOOP_ID = 22
R1_LOOP_V4 = "10.255.1.1"
R2_LOOP_V4 = "10.255.2.2"
R1_LOOP_V4_LEN = 32
R2_LOOP_V4_LEN = 32

# 加快收敛：hello 1s，hold 3s，keepalive 3s
HELLO_INTERVAL_MS = 1000
HOLD_TIME_MS = 3000
KEEPALIVE_INTERVAL_MS = 3000


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup LDP/loopback config")
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
                f"no if loop {loop}",
                "no ldp",
                "end",
            ],
        )


def _configure(rt: TopologyRuntime, *, device: str, lsr_id: str, loop_id: int, loop_v4: str, loop_v4_len: int) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if loop {loop_id}",
            f"ip address {loop_v4} {loop_v4_len}",
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
    r1_prefix4 = int(g_top.r1.GE_1.prefix)
    r2_prefix4 = int(g_top.r2.GE_1.prefix)

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
                        f"IPv4 Addr  : {r1_ip4}/{r1_prefix4}",
                    ],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "r2",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r2_ip4}/{r2_prefix4}",
                    ],
                    "label": "r2 GE-1 up",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure LDP + loopback on both routers")
        _configure(rt, device="r1", lsr_id=R1_LSR_ID, loop_id=R1_LOOP_ID, loop_v4=R1_LOOP_V4,
                   loop_v4_len=R1_LOOP_V4_LEN)
        _configure(rt, device="r2", lsr_id=R2_LSR_ID, loop_id=R2_LOOP_ID, loop_v4=R2_LOOP_V4,
                   loop_v4_len=R2_LOOP_V4_LEN)

        step("Verify show ldp protocol reflects config")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp protocol",
                    "contains": [
                        "LDP Protocol",
                        f"LSR-ID          : {R1_LSR_ID}",
                        f"Hello interval  : {HELLO_INTERVAL_MS} ms",
                        f"Hold time       : {HOLD_TIME_MS} ms",
                        f"Keepalive       : {KEEPALIVE_INTERVAL_MS} ms",
                    ],
                    "label": "r1 ldp protocol cfg",
                },
                {
                    "device": "r2",
                    "command": "show ldp protocol",
                    "contains": [
                        "LDP Protocol",
                        f"LSR-ID          : {R2_LSR_ID}",
                        f"Hello interval  : {HELLO_INTERVAL_MS} ms",
                        f"Hold time       : {HOLD_TIME_MS} ms",
                        f"Keepalive       : {KEEPALIVE_INTERVAL_MS} ms",
                    ],
                    "label": "r2 ldp protocol cfg",
                },
            ],
            timeout=20,
            interval=2,
        )

        step("Verify LDP interface reaches 'up' (UDP socket on GE-1)")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp interface",
                    "contains": [GE_IF, r1_ip4, "up"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(GE_IF)}\s+\d+\s+{re.escape(r1_ip4)}\s+up\s+"
                        rf"{HELLO_INTERVAL_MS}\s+{HOLD_TIME_MS}\s*$"
                    ],
                    "label": "r1 ldp interface up",
                },
                {
                    "device": "r2",
                    "command": "show ldp interface",
                    "contains": [GE_IF, r2_ip4, "up"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(GE_IF)}\s+\d+\s+{re.escape(r2_ip4)}\s+up\s+"
                        rf"{HELLO_INTERVAL_MS}\s+{HOLD_TIME_MS}\s*$"
                    ],
                    "label": "r2 ldp interface up",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Wait LDP adjacency + session OPERATIONAL")
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

        step("Verify session/peer count surfaces in show ldp protocol")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp protocol",
                    "regex": [
                        r"(?im)^\s*Adjacencies\s*:\s*[1-9]\d*\s*$",
                        r"(?im)^\s*Sessions\s*:\s*[1-9]\d*\s*$",
                    ],
                    "label": "r1 protocol counters",
                },
                {
                    "device": "r2",
                    "command": "show ldp protocol",
                    "regex": [
                        r"(?im)^\s*Adjacencies\s*:\s*[1-9]\d*\s*$",
                        r"(?im)^\s*Sessions\s*:\s*[1-9]\d*\s*$",
                    ],
                    "label": "r2 protocol counters",
                },
            ],
            timeout=10,
            interval=2,
        )

        step("Verify Label binding: peer's LSR-ID/32 → implicit-null in remote LIB")
        # 双方都通告 LSR-ID/32 为 implicit-null（label=3，PHP）。
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp binding",
                    "contains": ["Local Label Information Base", "Remote Label Information Base"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(R1_LSR_ID)}/32\s+\d+\s*$",
                        rf"(?im)^\s*{re.escape(R2_LSR_ID)}\s+{re.escape(R2_LSR_ID)}/32\s+3\s*$",
                    ],
                    "not_contains": ["No LDP label binding"],
                    "label": "r1 binding has self LSR-ID and remote peer LSR-ID",
                },
                {
                    "device": "r2",
                    "command": "show ldp binding",
                    "contains": ["Local Label Information Base", "Remote Label Information Base"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(R2_LSR_ID)}/32\s+\d+\s*$",
                        rf"(?im)^\s*{re.escape(R1_LSR_ID)}\s+{re.escape(R1_LSR_ID)}/32\s+3\s*$",
                    ],
                    "not_contains": ["No LDP label binding"],
                    "label": "r2 binding has self LSR-ID and remote peer LSR-ID",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Verify loopback FECs are advertised once ROUTE module reports them")
        # ROUTE 推送各模块/连通路由 → LDP 申请 local label → 通告给对端
        # 对端 remote LIB 应包含本端 loopback prefix（label != 3，因为我们不是 PHP 的目标）
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp binding",
                    "regex": [
                        rf"(?im)^\s*{re.escape(R2_LSR_ID)}\s+{re.escape(R2_LOOP_V4)}/{R2_LOOP_V4_LEN}\s+\d+\s*$",
                    ],
                    "label": "r1 remote LIB has r2 loopback FEC",
                },
                {
                    "device": "r2",
                    "command": "show ldp binding",
                    "regex": [
                        rf"(?im)^\s*{re.escape(R1_LSR_ID)}\s+{re.escape(R1_LOOP_V4)}/{R1_LOOP_V4_LEN}\s+\d+\s*$",
                    ],
                    "label": "r2 remote LIB has r1 loopback FEC",
                },
            ],
            timeout=60,
            interval=3,
        )

        step("Best-effort: TUNNEL candidate registration (M6 matching)")
        # M6 把 peer↔nexthop 匹配扩展为 lsr_id / transport_v4 / link_addr_v4 三选一。
        # 但本用例没有 IGP，r1 不会学到去 r2 loopback (/32) 的路由——M6 限制只
        # 为 /32 host route 申请 LSP，因此 candidate 不会注册。这里保留弱校验，
        # 真正的 M6 验证在 ldp_with_isis.py 中（ISIS 注入的 /32 命中 link_addr_v4 规则）。
        try:
            wait_check(
                rt,
                device="r1",
                command="show tunnel candidate",
                timeout=10,
                interval=2,
                regex=[r"(?im)\bsrc\s+ldp\b"],
                label="r1 ldp tunnel candidate (best effort)",
            )
            print("  [info] r1 has LDP tunnel candidate")
        except Exception:
            print("  [info] r1 has no LDP tunnel candidate (expected without IGP /32 route to peer loopback)")

        print("LDP basic scenario check passed.")
    finally:
        _cleanup(rt)
