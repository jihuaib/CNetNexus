#!/usr/bin/env python3
"""
LLDP basic discovery and CLI coverage.

Topology: r1 --- GE-1 --- r2

Coverage:
- global LLDP enable with timer/hold-multiplier config
- interface enable, admin-status, and port-description config
- interface runtime display
- mutual LLDP neighbor discovery
- neighbor detail display
- runtime statistics counters
- show current-configuration contribution
"""

from __future__ import annotations

import re

from module_api import require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"
TX_INTERVAL_SEC = 5
HOLD_MULTIPLIER = 2
PORT_DESC_R1 = "r1-to-r2"
PORT_DESC_R2 = "r2-to-r1"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup LLDP config")
    for dev in ("r1", "r2"):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"if {GE_IF}",
                "no lldp port-description",
                "no lldp admin-status",
                "lldp enable",
                "exit",
                "no lldp hold-multiplier",
                "no lldp timer",
                "no lldp",
                "end",
            ],
        )


def _configure(rt: TopologyRuntime, *, device: str, port_desc: str) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            "lldp",
            f"lldp timer {TX_INTERVAL_SEC}",
            f"lldp hold-multiplier {HOLD_MULTIPLIER}",
            f"if {GE_IF}",
            "lldp enable",
            "lldp admin-status txrx",
            f"lldp port-description {port_desc}",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    try:
        _cleanup(rt)

        step("Configure LLDP on both routers")
        _configure(rt, device="r1", port_desc=PORT_DESC_R1)
        _configure(rt, device="r2", port_desc=PORT_DESC_R2)

        step("Verify LLDP protocol and interface state")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show lldp",
                    "contains": [
                        "LLDP Protocol",
                        "Admin : up",
                        f"Tx interval : {TX_INTERVAL_SEC} sec",
                        f"Hold multiplier: {HOLD_MULTIPLIER}",
                    ],
                    "label": "r1 lldp summary",
                },
                {
                    "device": "r2",
                    "command": "show lldp",
                    "contains": [
                        "LLDP Protocol",
                        "Admin : up",
                        f"Tx interval : {TX_INTERVAL_SEC} sec",
                        f"Hold multiplier: {HOLD_MULTIPLIER}",
                    ],
                    "label": "r2 lldp summary",
                },
                {
                    "device": "r1",
                    "command": "show lldp interface",
                    "contains": [GE_IF, "up", "txrx", "yes", PORT_DESC_R1],
                    "regex": [rf"(?im)^{re.escape(GE_IF)}\s+\d+\s+up\s+txrx\s+yes\s+{re.escape(PORT_DESC_R1)}\s*$"],
                    "label": "r1 lldp interface",
                },
                {
                    "device": "r2",
                    "command": "show lldp interface",
                    "contains": [GE_IF, "up", "txrx", "yes", PORT_DESC_R2],
                    "regex": [rf"(?im)^{re.escape(GE_IF)}\s+\d+\s+up\s+txrx\s+yes\s+{re.escape(PORT_DESC_R2)}\s*$"],
                    "label": "r2 lldp interface",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Wait for mutual LLDP neighbor discovery")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show lldp neighbors",
                    "contains": [GE_IF],
                    "not_contains": ["No LLDP neighbor"],
                    "regex": [rf"(?im)^{re.escape(GE_IF)}\s+\S+"],
                    "label": "r1 sees lldp neighbor",
                },
                {
                    "device": "r2",
                    "command": "show lldp neighbors",
                    "contains": [GE_IF],
                    "not_contains": ["No LLDP neighbor"],
                    "regex": [rf"(?im)^{re.escape(GE_IF)}\s+\S+"],
                    "label": "r2 sees lldp neighbor",
                },
            ],
            timeout=45,
            interval=3,
        )

        step("Verify LLDP neighbor detail and statistics")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show lldp neighbors detail",
                    "contains": ["Interface: GE-1", f"Port description : {PORT_DESC_R2}", "TTL"],
                    "label": "r1 neighbor detail",
                },
                {
                    "device": "r2",
                    "command": "show lldp neighbors detail",
                    "contains": ["Interface: GE-1", f"Port description : {PORT_DESC_R1}", "TTL"],
                    "label": "r2 neighbor detail",
                },
                {
                    "device": "r1",
                    "command": "show lldp statistics",
                    "contains": ["LLDP Statistics", "TX frames", "RX frames", "Neighbor updates"],
                    "regex": [r"(?im)TX frames\s*:\s*[1-9]\d*", r"(?im)RX frames\s*:\s*[1-9]\d*"],
                    "label": "r1 lldp statistics",
                },
                {
                    "device": "r2",
                    "command": "show lldp statistics",
                    "contains": ["LLDP Statistics", "TX frames", "RX frames", "Neighbor updates"],
                    "regex": [r"(?im)TX frames\s*:\s*[1-9]\d*", r"(?im)RX frames\s*:\s*[1-9]\d*"],
                    "label": "r2 lldp statistics",
                },
            ],
            timeout=20,
            interval=2,
        )

        step("Verify LLDP current-configuration output")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "contains": [
                        "lldp",
                        f"lldp timer {TX_INTERVAL_SEC}",
                        f"lldp hold-multiplier {HOLD_MULTIPLIER}",
                        "lldp enable",
                        f"lldp port-description {PORT_DESC_R1}",
                    ],
                    "label": "r1 lldp config rendering",
                },
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "contains": [
                        "lldp",
                        f"lldp timer {TX_INTERVAL_SEC}",
                        f"lldp hold-multiplier {HOLD_MULTIPLIER}",
                        "lldp enable",
                        f"lldp port-description {PORT_DESC_R2}",
                    ],
                    "label": "r2 lldp config rendering",
                },
            ],
            timeout=20,
            interval=2,
        )

    finally:
        _cleanup(rt)

    print("LLDP basic discovery check passed.")
