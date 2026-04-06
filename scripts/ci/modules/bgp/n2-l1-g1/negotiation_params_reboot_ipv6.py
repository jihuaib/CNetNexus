#!/usr/bin/env python3
"""
BGP negotiation-parameter IPv6 check script.

Goal:
- verify router-id change triggers re-negotiation
- verify hold timer change triggers re-negotiation
- verify open AF change (ipv4-unicast) triggers re-negotiation
- verify negotiated parameters persist after reboot
"""

from __future__ import annotations

import re

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def _session_checks(r1_peer_ip6: str, r2_peer_ip6: str) -> list[dict[str, object]]:
    return [
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [r1_peer_ip6],
                "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r1->r2 ipv6-unicast established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [r2_peer_ip6],
                "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r2->r1 ipv6-unicast established",
        },
    ]


def _cleanup(rt: TopologyRuntime) -> None:
    for dev in ("r1", "r2"):
        run_cmds(rt=rt, device=dev, strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        _run_inner(rt, r1_peer_ip6, r2_peer_ip6)
    finally:
        step("Cleanup BGP config")
        _cleanup(rt)

    print("BGP negotiation parameter IPv6 check passed.")


def _run_inner(rt: TopologyRuntime, r1_peer_ip6: str, r2_peer_ip6: str) -> None:
    step("Build baseline BGP session (IPv6 only)")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "bgp 65001",
            "router-id 1.1.1.1",
            "timer keepalive 30 hold 90",
            "timer connect-retry 5",
            f"neighbor {r1_peer_ip6} as 65002",
            "no af ipv4-unicast",
            "af ipv6-unicast",
            f"neighbor {r1_peer_ip6} enable",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            "bgp 65002",
            "router-id 2.2.2.2",
            "timer keepalive 30 hold 90",
            "timer connect-retry 5",
            f"neighbor {r2_peer_ip6} as 65001",
            "no af ipv4-unicast",
            "af ipv6-unicast",
            f"neighbor {r2_peer_ip6} enable",
            "exit",
            "end",
        ],
    )
    wait_checks(rt, _session_checks(r1_peer_ip6, r2_peer_ip6), timeout=40)

    step("Change r1 router-id and verify re-negotiation")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config", "bgp 65001", "router-id 1.1.1.11", "end"],
    )
    wait_checks(rt, _session_checks(r1_peer_ip6, r2_peer_ip6), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast {r2_peer_ip6}",
                "contains": [r2_peer_ip6, "Remote Router-ID", "1.1.1.11", "Established"],
                "label": "r2 sees updated r1 router-id",
            },
        ],
        timeout=40,
    )

    step("Change hold timers and verify negotiated hold")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config", "bgp 65001", "timer keepalive 15 hold 45", "end"],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=["config", "bgp 65002", "timer keepalive 15 hold 45", "end"],
    )
    wait_checks(rt, _session_checks(r1_peer_ip6, r2_peer_ip6), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv6-unicast {r1_peer_ip6}",
                "contains": [r1_peer_ip6, "Negotiated", "45 s"],
                "label": "r1 negotiated hold=45",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast {r2_peer_ip6}",
                "contains": [r2_peer_ip6, "Negotiated", "45 s"],
                "label": "r2 negotiated hold=45",
            },
        ],
        timeout=40,
    )

    step("Enable IPv4 AF and verify negotiated AF list")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "bgp 65001",
            "af ipv4-unicast",
            f"neighbor {r1_peer_ip6} enable",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            "bgp 65002",
            "af ipv4-unicast",
            f"neighbor {r2_peer_ip6} enable",
            "exit",
            "end",
        ],
    )
    wait_checks(rt, _session_checks(r1_peer_ip6, r2_peer_ip6), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv6-unicast {r1_peer_ip6}",
                "contains": [r1_peer_ip6, "afi=1 safi=1", "afi=2 safi=1"],
                "label": "r1 negotiated ipv4+ipv6 AF",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast {r2_peer_ip6}",
                "contains": [r2_peer_ip6, "afi=1 safi=1", "afi=2 safi=1"],
                "label": "r2 negotiated ipv4+ipv6 AF",
            },
        ],
        timeout=40,
    )

    step("Toggle OPEN capability route-refresh and verify re-negotiation")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config", "bgp 65001", f"no neighbor {r1_peer_ip6} open-capability route-refresh", "end"],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=["config", "bgp 65002", f"no neighbor {r2_peer_ip6} open-capability route-refresh", "end"],
    )
    wait_checks(rt, _session_checks(r1_peer_ip6, r2_peer_ip6), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv6-unicast {r1_peer_ip6}",
                "contains": [r1_peer_ip6, "Route-Refresh", "No", "Established"],
                "label": "r1 route-refresh disabled and renegotiated",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast {r2_peer_ip6}",
                "contains": [r2_peer_ip6, "Route-Refresh", "No", "Established"],
                "label": "r2 route-refresh disabled and renegotiated",
            },
        ],
        timeout=40,
    )

    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config", "bgp 65001", f"neighbor {r1_peer_ip6} open-capability route-refresh", "end"],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=["config", "bgp 65002", f"neighbor {r2_peer_ip6} open-capability route-refresh", "end"],
    )
    wait_checks(rt, _session_checks(r1_peer_ip6, r2_peer_ip6), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv6-unicast {r1_peer_ip6}",
                "contains": [r1_peer_ip6, "Route-Refresh", "Yes", "Established"],
                "label": "r1 route-refresh enabled and renegotiated",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast {r2_peer_ip6}",
                "contains": [r2_peer_ip6, "Route-Refresh", "Yes", "Established"],
                "label": "r2 route-refresh enabled and renegotiated",
            },
        ],
        timeout=40,
    )

    step("Reboot r1 and verify negotiated params after restore")
    reboot_device(rt, "r1", timeout=120)
    wait_checks(rt, _session_checks(r1_peer_ip6, r2_peer_ip6), timeout=60)
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast {r2_peer_ip6}",
                "contains": [
                    r2_peer_ip6,
                    "Remote Router-ID",
                    "1.1.1.11",
                    "Negotiated",
                    "45 s",
                    "afi=2 safi=1",
                    "Route-Refresh",
                    "Yes",
                ],
                "label": "r2 keeps router-id/hold/af negotiation after r1 reboot",
            },
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv6-unicast {r1_peer_ip6}",
                "contains": [r1_peer_ip6, "Negotiated", "45 s", "afi=2 safi=1", "Route-Refresh", "Yes"],
                "label": "r1 keeps hold/af negotiation after reboot",
            },
        ],
        timeout=60,
    )
