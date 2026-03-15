#!/usr/bin/env python3
"""
BGP dual-peer reboot check script.

Topology:
- device a <-> device b has two links (GE-1 and GE-2)

Checks:
1) build two BGP peers per direction and verify both are Established
2) reboot device a
3) verify both BGP peers recover to Established
"""

from __future__ import annotations

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    """
    Entry called by module_runner.
    """
    require_devices(top, ("a", "b"))

    a_ge1_peer = str(g_top.a.GE_1.peer_ip)
    a_ge2_peer = str(g_top.a.GE_2.peer_ip)
    b_ge1_peer = str(g_top.b.GE_1.peer_ip)
    b_ge2_peer = str(g_top.b.GE_2.peer_ip)

    step("Configure BGP base")
    run_cmds(rt=rt, device="a", strict=False, commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
    run_cmds(rt=rt, device="b", strict=False, commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

    step("Configure dual-link BGP neighbors")
    run_cmds(
        rt=rt,
        device="a",
        strict=False,
        commands=[
            "config",
            "bgp 65001",
            f"neighbor {a_ge1_peer} as 65002",
            f"neighbor {a_ge2_peer} as 65002",
            "af ipv4-unicast",
            f"neighbor {a_ge1_peer} enable",
            f"neighbor {a_ge2_peer} enable",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="b",
        strict=False,
        commands=[
            "config",
            "bgp 65002",
            f"neighbor {b_ge1_peer} as 65001",
            f"neighbor {b_ge2_peer} as 65001",
            "af ipv4-unicast",
            f"neighbor {b_ge1_peer} enable",
            f"neighbor {b_ge2_peer} enable",
            "exit",
            "end",
        ],
    )

    session_checks = [
        {
            "device": "a",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [a_ge1_peer, "Established"],
            "label": "a(GE-1)->b ipv4-unicast",
        },
        {
            "device": "a",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [a_ge2_peer, "Established"],
            "label": "a(GE-2)->b ipv4-unicast",
        },
        {
            "device": "b",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [b_ge1_peer, "Established"],
            "label": "b(GE-1)->a ipv4-unicast",
        },
        {
            "device": "b",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [b_ge2_peer, "Established"],
            "label": "b(GE-2)->a ipv4-unicast",
        },
    ]

    step("Wait dual-link BGP sessions")
    wait_checks(rt, session_checks, timeout=30)

    step("Reboot a and wait CLI reconnect")
    reboot_device(rt, "a", timeout=120)

    step("Wait dual-link BGP sessions after reboot")
    wait_checks(rt, session_checks, timeout=30)

    print("BGP dual-peer reboot check passed.")
