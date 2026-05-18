#!/usr/bin/env python3
"""
NetNexus <-> FRR LDP IPv4 interop smoke test.

Topology: r1(NetNexus) --- GE-1/eth1 --- f1(FRR)

Coverage:
- NetNexus and FRR LDP basic configuration on a directly connected link
- LDP discovery and TCP session establishment with FRR ldpd
- Local/remote label binding exchange for LSR-ID and loopback host FECs
- FRR LDP shutdown removes the NetNexus adjacency/session
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    frr_config,
    hold_check,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"
FRR_LINUX_IF = "eth1"

NN_LSR_ID = "1.1.1.1"
FRR_LSR_ID = "2.2.2.2"
NN_LINK_IP = "10.12.0.1"
FRR_LINK_IP = "10.12.0.2"

NN_LOOP_ID = 11
NN_LOOP_V4 = NN_LSR_ID
FRR_LOOP_V4 = FRR_LSR_ID
LOOP_V4_LEN = 32

HELLO_INTERVAL_MS = 1000
HOLD_TIME_MS = 3000
KEEPALIVE_INTERVAL_MS = 3000


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup NetNexus/FRR LDP interop config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"if {GE_IF}",
            "no ldp enable",
            "exit",
            f"no if loop {NN_LOOP_ID}",
            "no ldp",
            "end",
        ],
    )
    frr_config(
        rt,
        "f1",
        [
            "no mpls ldp",
        ],
        strict=False,
    )
    rt.exec_cmd("f1", f"ip addr del {FRR_LOOP_V4}/{LOOP_V4_LEN} dev lo", strict=False)


def _configure_netnexus(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if loop {NN_LOOP_ID}",
            f"ip address {NN_LOOP_V4} {LOOP_V4_LEN}",
            "exit",
            "ldp",
            f"lsr-id {NN_LSR_ID}",
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


def _configure_frr(rt: TopologyRuntime) -> None:
    rt.exec_cmd("f1", "ip link set dev lo up")
    rt.exec_cmd("f1", f"ip addr replace {FRR_LOOP_V4}/{LOOP_V4_LEN} dev lo")
    frr_config(
        rt,
        "f1",
        [
            "mpls ldp",
            f"router-id {FRR_LSR_ID}",
            "address-family ipv4",
            f"discovery transport-address {FRR_LINK_IP}",
            "discovery hello interval 1",
            "discovery hello holdtime 3",
            "session holdtime 15",
            "label local allocate host-routes",
            f"interface {FRR_LINUX_IF}",
            "exit",
            "exit-address-family",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "f1"))

    try:
        _cleanup(rt)

        step("Ensure GE-1/eth1 baseline connectivity")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {NN_LINK_IP}/30",
                    ],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "f1",
                    "command": f"ip -4 addr show dev {FRR_LINUX_IF}",
                    "contains": [f"{FRR_LINK_IP}/30"],
                    "label": "f1 eth1 has IPv4 address",
                },
                {
                    "device": "f1",
                    "command": f"ping -c 1 -W 2 {NN_LINK_IP}",
                    "contains": ["1 received"],
                    "label": "f1 can ping r1 link IP",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure NetNexus and FRR LDP")
        _configure_netnexus(rt)
        _configure_frr(rt)

        step("Verify LDP interface/discovery is enabled")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp interface",
                    "contains": [GE_IF, NN_LINK_IP, "up"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(GE_IF)}\s+\d+\s+{re.escape(NN_LINK_IP)}\s+up\s+"
                        rf"{HELLO_INTERVAL_MS}\s+{HOLD_TIME_MS}\s*$"
                    ],
                    "label": "r1 LDP interface up",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show mpls ldp discovery'",
                    "contains": ["ipv4", NN_LSR_ID, "Link", FRR_LINUX_IF],
                    "label": "FRR discovered NetNexus link hello",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Wait LDP session OPERATIONAL/OPERATIONAL")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp neighbor",
                    "contains": [FRR_LSR_ID, GE_IF, FRR_LINK_IP, "OPERATIONAL"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(FRR_LSR_ID)}\s+0\s+{re.escape(GE_IF)}\s+"
                        rf"{re.escape(FRR_LINK_IP)}\s+OPERATIONAL\b"
                    ],
                    "not_contains": ["No LDP adjacency"],
                    "label": "NetNexus sees FRR OPERATIONAL",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show mpls ldp neighbor'",
                    "contains": ["ipv4", NN_LSR_ID, "OPERATIONAL", NN_LINK_IP],
                    "label": "FRR sees NetNexus OPERATIONAL",
                },
            ],
            timeout=80,
            interval=2,
        )

        step("Hold LDP session stable")
        hold_check(
            rt,
            device="r1",
            command="show ldp neighbor",
            duration=12,
            interval=2,
            contains=[FRR_LSR_ID, GE_IF, FRR_LINK_IP, "OPERATIONAL"],
            regex=[
                rf"(?im)^\s*{re.escape(FRR_LSR_ID)}\s+0\s+{re.escape(GE_IF)}\s+"
                rf"{re.escape(FRR_LINK_IP)}\s+OPERATIONAL\b"
            ],
            not_contains=["No LDP adjacency"],
            label="NetNexus keeps FRR LDP session stable",
        )
        hold_check(
            rt,
            device="f1",
            command="vtysh -c 'show mpls ldp neighbor'",
            duration=12,
            interval=2,
            contains=["ipv4", NN_LSR_ID, "OPERATIONAL", NN_LINK_IP],
            label="FRR keeps NetNexus LDP session stable",
        )

        step("Verify label bindings exchanged in both directions")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp binding",
                    "contains": ["Local Label Information Base", "Remote Label Information Base"],
                    "regex": [
                        rf"(?im)^\s*{re.escape(NN_LSR_ID)}/{LOOP_V4_LEN}\s+3\s*$",
                        rf"(?im)^\s*{re.escape(FRR_LSR_ID)}\s+{re.escape(FRR_LOOP_V4)}/{LOOP_V4_LEN}\s+\S+\s*$",
                    ],
                    "not_contains": ["No LDP label binding"],
                    "label": "NetNexus has local and FRR remote LIB entries",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show mpls ldp binding'",
                    "contains": [f"{NN_LOOP_V4}/{LOOP_V4_LEN}", f"{FRR_LOOP_V4}/{LOOP_V4_LEN}"],
                    "regex": [
                        rf"(?im)^ipv4\s+{re.escape(NN_LOOP_V4)}/{LOOP_V4_LEN}\s+.*\s+imp-null\s+",
                        rf"(?im)^ipv4\s+{re.escape(FRR_LOOP_V4)}/{LOOP_V4_LEN}\s+.*\s+imp-null\s+",
                    ],
                    "label": "FRR has local and NetNexus remote LIB entries",
                },
            ],
            timeout=80,
            interval=3,
        )

        step("Disable FRR LDP and verify NetNexus removes adjacency")
        frr_config(rt, "f1", ["no mpls ldp"])
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp neighbor",
                    "contains": ["No LDP adjacency"],
                    "not_contains": [FRR_LSR_ID, "OPERATIONAL"],
                    "label": "NetNexus LDP adjacency removed after FRR shutdown",
                },
            ],
            timeout=15,
            interval=2,
        )

        print("NetNexus <-> FRR IPv4 LDP interop check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
