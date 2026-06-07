#!/usr/bin/env python3
"""
Validate IF/VRF subscription relationship show commands.

The checks are intentionally read-only:
  - show if subscribe
  - show vrf subscribe

They verify command registration, output shape, and baseline runtime subscribers
created by the always-on modules during startup.
"""

from __future__ import annotations

from module_api import require_devices, step, wait_check  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


IF_SUBSCRIBER_PATTERNS = [
    r"(?im)^\s*route\s+0x00000007\s+all\s+all\s+ready\s*$",
]

VRF_SUBSCRIBER_PATTERNS = [
    r"(?im)^\s*if\s+0x00000005\s+all\s+vrf-add\|vrf-del\|vrf-state\s+ready\s*$",
    r"(?im)^\s*route\s+0x00000007\s+all\s+vrf-add\|vrf-del\|vrf-state\s+ready\s*$",
    r"(?im)^\s*fib\s+0x0000000B\s+all\s+vrf-add\|vrf-del\|vrf-state\s+ready\s*$",
]

ERROR_TOKENS = [
    "Unknown command",
    "IF show Error",
    "VRF Error",
    "(no subscribers)",
]


def _wait_if_subscriptions(rt: TopologyRuntime, command: str) -> None:
    wait_check(
        rt,
        device="r1",
        command=command,
        timeout=15,
        interval=1,
        contains=[
            "IF Subscribers:",
            "Module",
            "Module-ID",
            "IF-Type",
            "Events",
            "Replay",
            "Total",
        ],
        not_contains=ERROR_TOKENS,
        regex=IF_SUBSCRIBER_PATTERNS,
        label=f"r1 {command}",
    )


def _wait_vrf_subscriptions(rt: TopologyRuntime, command: str) -> None:
    wait_check(
        rt,
        device="r1",
        command=command,
        timeout=15,
        interval=1,
        contains=[
            "VRF Subscribers:",
            "Module",
            "Module-ID",
            "AF",
            "Events",
            "Replay",
            "Total",
        ],
        not_contains=ERROR_TOKENS,
        regex=VRF_SUBSCRIBER_PATTERNS,
        label=f"r1 {command}",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))

    step("Show IF subscription relationships")
    _wait_if_subscriptions(rt, "show if subscribe")

    step("Show VRF subscription relationships")
    _wait_vrf_subscriptions(rt, "show vrf subscribe")

    print("IF/VRF subscription show command check passed.")
