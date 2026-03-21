#!/usr/bin/env python3
"""
BGP negotiation-parameter check script.

Goal:
- verify router-id change triggers re-negotiation
- verify hold timer change triggers re-negotiation
- verify open AF change (ipv6-unicast) triggers re-negotiation
- verify negotiated parameters persist after reboot
"""

from __future__ import annotations

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def _session_checks(r1_peer_ip: str, r2_peer_ip: str) -> list[dict[str, object]]:
    return [
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r1_peer_ip, "Established"],
            "label": "r1->r2 ipv4-unicast established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r2_peer_ip, "Established"],
            "label": "r2->r1 ipv4-unicast established",
        },
    ]


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    step("Build baseline BGP session (IPv4 only)")
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
            f"neighbor {r1_peer_ip} as 65002",
            "no af ipv6-unicast",
            "af ipv4-unicast",
            f"neighbor {r1_peer_ip} enable",
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
            f"neighbor {r2_peer_ip} as 65001",
            "no af ipv6-unicast",
            "af ipv4-unicast",
            f"neighbor {r2_peer_ip} enable",
            "exit",
            "end",
        ],
    )
    wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip), timeout=40)

    step("Change r1 router-id and verify re-negotiation")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config", "bgp 65001", "router-id 1.1.1.11", "end"],
    )
    wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast {r2_peer_ip}",
                "contains": [r2_peer_ip, "Remote Router-ID", "1.1.1.11", "Established"],
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
    wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv4-unicast {r1_peer_ip}",
                "contains": [r1_peer_ip, "Negotiated", "45 s"],
                "label": "r1 negotiated hold=45",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast {r2_peer_ip}",
                "contains": [r2_peer_ip, "Negotiated", "45 s"],
                "label": "r2 negotiated hold=45",
            },
        ],
        timeout=40,
    )

    step("Enable IPv6 AF and verify negotiated AF list")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "bgp 65001",
            "af ipv6-unicast",
            f"neighbor {r1_peer_ip} enable",
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
            "af ipv6-unicast",
            f"neighbor {r2_peer_ip} enable",
            "exit",
            "end",
        ],
    )
    wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv4-unicast {r1_peer_ip}",
                "contains": [r1_peer_ip, "afi=1 safi=1", "afi=2 safi=1"],
                "label": "r1 negotiated ipv4+ipv6 AF",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast {r2_peer_ip}",
                "contains": [r2_peer_ip, "afi=1 safi=1", "afi=2 safi=1"],
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
        commands=["config", "bgp 65001", f"no neighbor {r1_peer_ip} open-capability route-refresh", "end"],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=["config", "bgp 65002", f"no neighbor {r2_peer_ip} open-capability route-refresh", "end"],
    )
    wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv4-unicast {r1_peer_ip}",
                "contains": [r1_peer_ip, "Route-Refresh", "No", "Established"],
                "label": "r1 route-refresh disabled and renegotiated",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast {r2_peer_ip}",
                "contains": [r2_peer_ip, "Route-Refresh", "No", "Established"],
                "label": "r2 route-refresh disabled and renegotiated",
            },
        ],
        timeout=40,
    )

    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config", "bgp 65001", f"neighbor {r1_peer_ip} open-capability route-refresh", "end"],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=["config", "bgp 65002", f"neighbor {r2_peer_ip} open-capability route-refresh", "end"],
    )
    wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip), timeout=40)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv4-unicast {r1_peer_ip}",
                "contains": [r1_peer_ip, "Route-Refresh", "Yes", "Established"],
                "label": "r1 route-refresh enabled and renegotiated",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast {r2_peer_ip}",
                "contains": [r2_peer_ip, "Route-Refresh", "Yes", "Established"],
                "label": "r2 route-refresh enabled and renegotiated",
            },
        ],
        timeout=40,
    )

    step("Reboot r1 and verify negotiated params after restore")
    reboot_device(rt, "r1", timeout=120)
    wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip), timeout=60)
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast {r2_peer_ip}",
                "contains": [
                    r2_peer_ip,
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
                "command": f"show bgp neighbor af ipv4-unicast {r1_peer_ip}",
                "contains": [r1_peer_ip, "Negotiated", "45 s", "afi=2 safi=1", "Route-Refresh", "Yes"],
                "label": "r1 keeps hold/af negotiation after reboot",
            },
        ],
        timeout=60,
    )

    step("Cleanup BGP config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config", "no bgp", "end"],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=["config", "no bgp", "end"],
    )

    print("BGP negotiation parameter check passed.")
