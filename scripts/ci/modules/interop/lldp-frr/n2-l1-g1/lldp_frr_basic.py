#!/usr/bin/env python3
"""
NetNexus <-> FRR-container LLDP interop smoke test.

FRR itself does not provide an LLDP daemon; the FRR CI image starts lldpd
alongside FRR daemons. This case verifies NetNexus LLDP packets interoperate
with that standards-based peer on the same FRR interop container.
"""

from __future__ import annotations

import re

from module_api import hold_check, require_devices, run_cmds, should_skip_cleanup, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"
FRR_LINUX_IF = "eth1"
NN_LINK_IP = "10.12.0.1"
FRR_LINK_IP = "10.12.0.2"

TX_INTERVAL_SEC = 5
HOLD_MULTIPLIER = 2
PORT_DESC_R1 = "r1-to-f1"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup NetNexus/FRR LLDP interop config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"if {GE_IF}",
            "no lldp port-description",
            "no lldp admin-status",
            "no lldp enable",
            "exit",
            "no lldp hold-multiplier",
            "no lldp timer",
            "no lldp",
            "end",
        ],
    )


def _configure_netnexus(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            "lldp",
            f"lldp timer {TX_INTERVAL_SEC}",
            f"lldp hold-multiplier {HOLD_MULTIPLIER}",
            f"if {GE_IF}",
            "lldp enable",
            "lldp admin-status txrx",
            f"lldp port-description {PORT_DESC_R1}",
            "exit",
            "end",
        ],
    )


def _configure_lldpd(rt: TopologyRuntime) -> None:
    rt.exec_cmd("f1", "pgrep -x lldpd")
    rt.exec_cmd("f1", f"ip link set dev {FRR_LINUX_IF} up")
    rt.exec_cmd("f1", "lldpcli configure system hostname f1", strict=False)
    rt.exec_cmd("f1", "lldpcli configure system description 'CNetNexus FRR CI peer'", strict=False)
    rt.exec_cmd("f1", f"lldpcli configure lldp tx-interval {TX_INTERVAL_SEC}", strict=False)
    rt.exec_cmd("f1", f"lldpcli configure lldp tx-hold {HOLD_MULTIPLIER}", strict=False)


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

        step("Configure NetNexus LLDP and FRR-container lldpd")
        _configure_netnexus(rt)
        _configure_lldpd(rt)

        step("Verify NetNexus discovers the FRR LLDP peer")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show lldp neighbors",
                    "contains": [GE_IF],
                    "not_contains": ["No LLDP neighbor"],
                    "regex": [rf"(?im)^{re.escape(GE_IF)}\s+f1\s+\S+"],
                    "label": "NetNexus sees FRR LLDP peer",
                },
                {
                    "device": "r1",
                    "command": "show lldp neighbors detail",
                    "contains": ["Interface: GE-1", "System name       : f1", "TTL"],
                    "regex": [r"(?im)System description:\s+CNetNexus FRR CI peer"],
                    "label": "NetNexus neighbor detail includes lldpd system data",
                },
                {
                    "device": "r1",
                    "command": "show lldp statistics",
                    "contains": ["LLDP Statistics", "TX frames", "RX frames", "Neighbor updates"],
                    "regex": [r"(?im)TX frames\s*:\s*[1-9]\d*", r"(?im)RX frames\s*:\s*[1-9]\d*"],
                    "label": "NetNexus LLDP statistics increment",
                },
            ],
            timeout=60,
            interval=3,
        )

        step("Verify lldpd discovers the NetNexus peer")
        wait_checks(
            rt,
            [
                {
                    "device": "f1",
                    "command": f"lldpcli show neighbors ports {FRR_LINUX_IF}",
                    "contains": ["LLDP neighbors", "r1", FRR_LINUX_IF],
                    "label": "lldpd sees NetNexus peer",
                },
                {
                    "device": "f1",
                    "command": f"lldpcli show neighbors ports {FRR_LINUX_IF} details",
                    "contains": ["r1", PORT_DESC_R1, "CNetNexus LLDP"],
                    "label": "lldpd neighbor detail includes NetNexus TLVs",
                },
            ],
            timeout=60,
            interval=3,
        )

        step("Hold LLDP discovery stable across multiple transmit intervals")
        hold_check(
            rt,
            device="r1",
            command="show lldp neighbors",
            duration=12,
            interval=3,
            contains=[GE_IF, "f1"],
            not_contains=["No LLDP neighbor"],
            label="NetNexus keeps FRR LLDP neighbor stable",
        )

        step("Stop lldpd and verify NetNexus expires the FRR neighbor")
        rt.exec_cmd("f1", "pkill -TERM lldpd", strict=False)
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show lldp neighbors",
                    "contains": ["No LLDP neighbor"],
                    "not_contains": ["f1"],
                    "label": "NetNexus LLDP neighbor expired after lldpd stop",
                },
            ],
            timeout=(TX_INTERVAL_SEC * HOLD_MULTIPLIER) + 20,
            interval=2,
        )

        print("NetNexus <-> FRR-container LLDP interop check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
