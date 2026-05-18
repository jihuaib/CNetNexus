#!/usr/bin/env python3
"""
LDP + ISIS route withdraw/relearn tunnel programming check.

Topology:
  r1 -- GE-1/GE-1 -- r2 -- GE-2/GE-1 -- r3

Coverage:
- ISIS advertises r3 loopback to r1 through r2.
- LDP learns the ISIS /32 route and programs an LDP tunnel on r1.
- Withdrawing ISIS from r3 loopback removes the ISIS route and withdraws the
  LDP tunnel programming.
- Re-enabling ISIS on r3 loopback relearns the route, reprograms the tunnel,
  and r1 MPLS ping to r3 loopback succeeds.
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    g_top,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


TAG = 1

R1_R2_IF = "GE-1"
R2_R1_IF = "GE-1"
R2_R3_IF = "GE-2"
R3_R2_IF = "GE-1"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"
R3_NET = "49.0001.0000.0000.0003.00"

R1_LSR_ID = "1.1.1.1"
R2_LSR_ID = "2.2.2.2"
R3_LSR_ID = "3.3.3.3"

R1_LOOP_ID = 11
R2_LOOP_ID = 22
R3_LOOP_ID = 33
R1_LOOP_V4 = R1_LSR_ID
R2_LOOP_V4 = R2_LSR_ID
R3_LOOP_V4 = R3_LSR_ID
LOOP_V4_LEN = 32

HELLO_INTERVAL_MS = 1000
HOLD_TIME_MS = 3000
KEEPALIVE_INTERVAL_MS = 3000


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup LDP/ISIS/loopback config")
    for dev, ifaces, loop in (
        ("r1", (R1_R2_IF,), R1_LOOP_ID),
        ("r2", (R2_R1_IF, R2_R3_IF), R2_LOOP_ID),
        ("r3", (R3_R2_IF,), R3_LOOP_ID),
    ):
        commands = ["end", "config"]
        for ifname in ifaces:
            commands.extend(
                [
                    f"if {ifname}",
                    "no ldp enable",
                    f"no isis enable {TAG}",
                    "exit",
                ]
            )
        commands.extend(
            [
                f"if loop {loop}",
                f"no isis enable {TAG}",
                "exit",
                "no ldp",
                f"no isis {TAG}",
                f"no if loop {loop}",
                "end",
            ]
        )
        run_cmds(rt=rt, device=dev, commands=commands, strict=False)


def _configure_router(
    rt: TopologyRuntime,
    *,
    device: str,
    net: str,
    lsr_id: str,
    loop_id: int,
    loop_v4: str,
    ifaces: tuple[str, ...],
) -> None:
    commands = [
        "config",
        f"if loop {loop_id}",
        f"ip address {loop_v4} {LOOP_V4_LEN}",
        "exit",
        f"isis {TAG}",
        f"net {net}",
        "is-type level-1-2",
        "cost-style wide",
        "af ipv4",
        "exit",
    ]
    for ifname in ifaces:
        commands.extend(
            [
                f"if {ifname}",
                f"isis enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                "exit",
            ]
        )
    commands.extend(
        [
            f"if loop {loop_id}",
            f"isis enable {TAG}",
            f"isis passive {TAG}",
            "exit",
            "ldp",
            f"lsr-id {lsr_id}",
            f"hello-interval {HELLO_INTERVAL_MS}",
            f"hold-time {HOLD_TIME_MS}",
            f"keepalive-interval {KEEPALIVE_INTERVAL_MS}",
            "exit",
        ]
    )
    for ifname in ifaces:
        commands.extend([f"if {ifname}", "ldp enable", "exit"])
    commands.append("end")
    run_cmds(rt=rt, device=device, commands=commands)


def _set_r3_loop_isis(rt: TopologyRuntime, *, enable: bool) -> None:
    commands = ["config", f"if loop {R3_LOOP_ID}"]
    if enable:
        commands.extend([f"isis enable {TAG}", f"isis passive {TAG}"])
    else:
        commands.append(f"no isis enable {TAG}")
    commands.extend(["exit", "end"])
    run_cmds(rt=rt, device="r3", commands=commands)


def _wait_baseline_links(rt: TopologyRuntime) -> None:
    checks = []
    for dev, ifname, ip, prefix in (
        ("r1", R1_R2_IF, str(g_top.r1.GE_1.ip), int(g_top.r1.GE_1.prefix)),
        ("r2", R2_R1_IF, str(g_top.r2.GE_1.ip), int(g_top.r2.GE_1.prefix)),
        ("r2", R2_R3_IF, str(g_top.r2.GE_2.ip), int(g_top.r2.GE_2.prefix)),
        ("r3", R3_R2_IF, str(g_top.r3.GE_1.ip), int(g_top.r3.GE_1.prefix)),
    ):
        checks.append(
            {
                "device": dev,
                "command": f"show if {ifname}",
                "contains": [f"Interface {ifname} Detail:", "State      : UP", f"IPv4 Addr  : {ip}/{prefix}"],
                "label": f"{dev} {ifname} up",
            }
        )
    wait_checks(rt, checks, timeout=30, interval=2)


def _wait_isis_and_ldp(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis neighbor {TAG}",
                "contains": [R1_R2_IF, "Up"],
                "label": "r1 ISIS neighbor r2 up",
            },
            {
                "device": "r2",
                "command": f"show isis neighbor {TAG}",
                "contains": [R2_R1_IF, R2_R3_IF, "Up"],
                "label": "r2 ISIS neighbors r1/r3 up",
            },
            {
                "device": "r3",
                "command": f"show isis neighbor {TAG}",
                "contains": [R3_R2_IF, "Up"],
                "label": "r3 ISIS neighbor r2 up",
            },
            {
                "device": "r1",
                "command": "show route ipv4 proto isis",
                "contains": [f"{R3_LOOP_V4}/{LOOP_V4_LEN}"],
                "label": "r1 learned r3 loopback via ISIS",
            },
            {
                "device": "r2",
                "command": "show route ipv4 proto isis",
                "contains": [f"{R3_LOOP_V4}/{LOOP_V4_LEN}"],
                "label": "r2 learned r3 loopback via ISIS",
            },
            {
                "device": "r3",
                "command": "show route ipv4 proto isis",
                "contains": [f"{R1_LOOP_V4}/{LOOP_V4_LEN}"],
                "label": "r3 learned r1 loopback via ISIS",
            },
        ],
        timeout=120,
        interval=3,
    )

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show ldp neighbor",
                "contains": [R2_LSR_ID, R1_R2_IF, "OPERATIONAL"],
                "label": "r1 sees r2 LDP operational",
            },
            {
                "device": "r2",
                "command": "show ldp neighbor",
                "contains": [R1_LSR_ID, R2_R1_IF, R3_LSR_ID, R2_R3_IF, "OPERATIONAL"],
                "label": "r2 sees r1/r3 LDP operational",
            },
            {
                "device": "r3",
                "command": "show ldp neighbor",
                "contains": [R2_LSR_ID, R3_R2_IF, "OPERATIONAL"],
                "label": "r3 sees r2 LDP operational",
            },
        ],
        timeout=90,
        interval=2,
    )


def _wait_r3_tunnel_programmed(rt: TopologyRuntime, *, label: str) -> None:
    r1_to_r2 = str(g_top.r1.GE_1.peer_ip)
    r2_to_r3 = str(g_top.r2.GE_2.peer_ip)

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show ldp binding",
                "regex": [
                    rf"(?im)^\s*{re.escape(R2_LSR_ID)}\s+{re.escape(R3_LOOP_V4)}/{LOOP_V4_LEN}\s+[1-9]\d+\s*$"
                ],
                "not_contains": ["No LDP label binding"],
                "label": f"{label}: r1 has r2 label for r3 loopback",
            },
            {
                "device": "r2",
                "command": "show ldp binding",
                "regex": [
                    rf"(?im)^\s*{re.escape(R3_LSR_ID)}\s+{re.escape(R3_LOOP_V4)}/{LOOP_V4_LEN}\s+3\s*$"
                ],
                "not_contains": ["No LDP label binding"],
                "label": f"{label}: r2 has r3 implicit-null for r3 loopback",
            },
            {
                "device": "r1",
                "command": "show tunnel candidate",
                "regex": [
                    rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+endpoint\s+{re.escape(R3_LOOP_V4)}\s+nh\s+"
                    rf"{re.escape(r1_to_r2)}\s+relay\s+{re.escape(r1_to_r2)}\s+oif\s+\d+\s+src\s+ldp\s+"
                    r"pref\s+\d+\s+labels\s+\[[1-9]\d*\]\s*$"
                ],
                "label": f"{label}: r1 LDP tunnel candidate for r3 loopback",
            },
            {
                "device": "r1",
                "command": "show tunnel ftn",
                "regex": [
                    rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+fec\s+{re.escape(R3_LOOP_V4)}/{LOOP_V4_LEN}\s+"
                    r"->\s+nhlfe\s+[1-9]\d*\s+src\s+ldp\s+state\s+up\s*$"
                ],
                "label": f"{label}: r1 FTN programmed for r3 loopback",
            },
            {
                "device": "r1",
                "command": "show tunnel nhlfe",
                "regex": [
                    rf"(?im)^\s*id\s+[1-9]\d*\s+endpoint\s+{re.escape(R3_LOOP_V4)}\s+relay\s+"
                    rf"{re.escape(r1_to_r2)}\s+oif\s+\d+\s+src\s+ldp\s+labels\s+\[[1-9]\d*\]\s*$"
                ],
                "label": f"{label}: r1 NHLFE programmed for r3 loopback",
            },
            {
                "device": "r2",
                "command": "show tunnel candidate",
                "regex": [
                    rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+endpoint\s+{re.escape(R3_LOOP_V4)}\s+nh\s+"
                    rf"{re.escape(r2_to_r3)}\s+relay\s+{re.escape(r2_to_r3)}\s+oif\s+\d+\s+src\s+ldp\s+"
                    r"pref\s+\d+\s+labels\s+\[\]\s*$"
                ],
                "label": f"{label}: r2 PHP candidate for r3 loopback",
            },
            {
                "device": "r2",
                "command": "show tunnel ilm",
                "regex": [
                    r"(?im)^\s*vrf\s+0\s+label\s+[1-9]\d+\s+->\s+nhlfe\s+[1-9]\d*\s+"
                    r"action\s+pop\(3\)\s+state\s+up\s*$"
                ],
                "label": f"{label}: r2 ILM POP programmed",
            },
        ],
        timeout=90,
        interval=3,
    )


def _wait_r3_route_and_tunnel_withdrawn(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show route ipv4 {R3_LOOP_V4} {LOOP_V4_LEN}",
                "contains": ["(no matching routes)"],
                "not_regex": [r"Path\s*\[\d+\]\s*:\s*isis"],
                "label": "r1 route RIB no longer has r3 loopback",
            },
            {
                "device": "r2",
                "command": f"show route ipv4 {R3_LOOP_V4} {LOOP_V4_LEN}",
                "contains": ["(no matching routes)"],
                "not_regex": [r"Path\s*\[\d+\]\s*:\s*isis"],
                "label": "r2 route RIB no longer has r3 loopback",
            },
            {
                "device": "r1",
                "command": "show tunnel candidate",
                "not_regex": [rf"(?im)\bendpoint\s+{re.escape(R3_LOOP_V4)}\b.*\bsrc\s+ldp\b"],
                "label": "r1 LDP candidate for r3 loopback withdrawn",
            },
            {
                "device": "r1",
                "command": "show tunnel ftn",
                "not_regex": [rf"(?im)\bfec\s+{re.escape(R3_LOOP_V4)}/{LOOP_V4_LEN}\b.*\bsrc\s+ldp\b"],
                "label": "r1 LDP FTN for r3 loopback withdrawn",
            },
            {
                "device": "r1",
                "command": "show tunnel nhlfe",
                "not_regex": [rf"(?im)\bendpoint\s+{re.escape(R3_LOOP_V4)}\b.*\bsrc\s+ldp\b"],
                "label": "r1 LDP NHLFE for r3 loopback withdrawn",
            },
            {
                "device": "r2",
                "command": "show tunnel candidate",
                "not_regex": [rf"(?im)\bendpoint\s+{re.escape(R3_LOOP_V4)}\b.*\bsrc\s+ldp\b"],
                "label": "r2 LDP candidate for r3 loopback withdrawn",
            },
            {
                "device": "r2",
                "command": "show tunnel ftn",
                "not_regex": [rf"(?im)\bfec\s+{re.escape(R3_LOOP_V4)}/{LOOP_V4_LEN}\b.*\bsrc\s+ldp\b"],
                "label": "r2 LDP FTN for r3 loopback withdrawn",
            },
        ],
        timeout=90,
        interval=3,
    )

    wait_check(
        rt,
        device="r1",
        command=f"ping mpls ipv4 {R3_LOOP_V4}/{LOOP_V4_LEN} -a {R1_LOOP_V4}",
        contains=["Error: no resolved MPLS tunnel for target FEC"],
        timeout=10,
        interval=2,
        label="r1 MPLS ping fails while r3 tunnel is withdrawn",
    )


def _assert_mpls_ping(rt: TopologyRuntime, *, label: str) -> None:
    out = rt.exec_cmd("r1", f"ping mpls ipv4 {R3_LOOP_V4}/{LOOP_V4_LEN} -a {R1_LOOP_V4}", timeout=20)
    if "0% packet loss" not in out or "100% packet loss" in out:
        raise AssertionError(f"{label}: r1 MPLS ping to r3 loopback failed:\n{out}")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    try:
        _cleanup(rt)

        step("Ensure baseline links are up")
        _wait_baseline_links(rt)

        step("Configure ISIS and LDP on r1/r2/r3")
        _configure_router(
            rt,
            device="r1",
            net=R1_NET,
            lsr_id=R1_LSR_ID,
            loop_id=R1_LOOP_ID,
            loop_v4=R1_LOOP_V4,
            ifaces=(R1_R2_IF,),
        )
        _configure_router(
            rt,
            device="r2",
            net=R2_NET,
            lsr_id=R2_LSR_ID,
            loop_id=R2_LOOP_ID,
            loop_v4=R2_LOOP_V4,
            ifaces=(R2_R1_IF, R2_R3_IF),
        )
        _configure_router(
            rt,
            device="r3",
            net=R3_NET,
            lsr_id=R3_LSR_ID,
            loop_id=R3_LOOP_ID,
            loop_v4=R3_LOOP_V4,
            ifaces=(R3_R2_IF,),
        )

        step("Wait initial ISIS/LDP convergence")
        _wait_isis_and_ldp(rt)

        step("Verify initial LDP tunnel programming for ISIS-learned r3 loopback")
        _wait_r3_tunnel_programmed(rt, label="initial")
        _assert_mpls_ping(rt, label="initial")

        step("Withdraw r3 loopback from ISIS and verify tunnel withdrawal")
        _set_r3_loop_isis(rt, enable=False)
        _wait_r3_route_and_tunnel_withdrawn(rt)

        step("Re-advertise r3 loopback into ISIS")
        _set_r3_loop_isis(rt, enable=True)
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show route ipv4 proto isis",
                    "contains": [f"{R3_LOOP_V4}/{LOOP_V4_LEN}"],
                    "label": "r1 relearned r3 loopback via ISIS",
                },
                {
                    "device": "r2",
                    "command": "show route ipv4 proto isis",
                    "contains": [f"{R3_LOOP_V4}/{LOOP_V4_LEN}"],
                    "label": "r2 relearned r3 loopback via ISIS",
                },
            ],
            timeout=120,
            interval=3,
        )

        step("Verify LDP tunnel reprogramming and MPLS ping after ISIS relearn")
        _wait_r3_tunnel_programmed(rt, label="relearn")
        _assert_mpls_ping(rt, label="relearn")

        print("LDP ISIS route withdraw/relearn tunnel check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
