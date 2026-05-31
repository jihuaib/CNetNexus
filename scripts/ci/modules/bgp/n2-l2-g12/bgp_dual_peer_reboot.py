#!/usr/bin/env python3
"""
BGP dual-peer reboot dual-stack check script.

Topology:
- device a <-> device b has two links (GE-1 and GE-2)

Checks:
1) build two BGP peers per direction and verify both are Established
2) reboot device a
3) verify both BGP peers recover to Established
"""

from __future__ import annotations

import re

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def _cleanup(rt: TopologyRuntime) -> None:
    for dev in ("a", "b"):
        run_cmds(rt=rt, device=dev, strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    """
    Entry called by module_runner.
    """
    require_devices(top, ("a", "b"))

    a_ge1_peer = str(g_top.a.GE_1.peer_ip)
    a_ge2_peer = str(g_top.a.GE_2.peer_ip)
    b_ge1_peer = str(g_top.b.GE_1.peer_ip)
    b_ge2_peer = str(g_top.b.GE_2.peer_ip)
    a_ge1_peer6 = str(g_top.a.GE_1.peer_ip6)
    a_ge2_peer6 = str(g_top.a.GE_2.peer_ip6)
    b_ge1_peer6 = str(g_top.b.GE_1.peer_ip6)
    b_ge2_peer6 = str(g_top.b.GE_2.peer_ip6)

    try:
        _run_inner_ipv4(rt, a_ge1_peer, a_ge2_peer, b_ge1_peer, b_ge2_peer)
        step("Cleanup BGP config before IPv6 scenario")
        _cleanup(rt)
        _run_inner_ipv6(rt, a_ge1_peer6, a_ge2_peer6, b_ge1_peer6, b_ge2_peer6)
    finally:
        step("Cleanup BGP config")
        _cleanup(rt)

    print("BGP dual-peer reboot dual-stack check passed.")


def _run_inner_ipv4(
    rt: TopologyRuntime,
    a_ge1_peer: str,
    a_ge2_peer: str,
    b_ge1_peer: str,
    b_ge2_peer: str,
) -> None:
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
            "contains": [a_ge1_peer],
                "regex": [rf"(?im)^\s*{re.escape(a_ge1_peer)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "a(GE-1)->b ipv4-unicast",
        },
        {
            "device": "a",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [a_ge2_peer],
                "regex": [rf"(?im)^\s*{re.escape(a_ge2_peer)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "a(GE-2)->b ipv4-unicast",
        },
        {
            "device": "b",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [b_ge1_peer],
                "regex": [rf"(?im)^\s*{re.escape(b_ge1_peer)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "b(GE-1)->a ipv4-unicast",
        },
        {
            "device": "b",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [b_ge2_peer],
                "regex": [rf"(?im)^\s*{re.escape(b_ge2_peer)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "b(GE-2)->a ipv4-unicast",
        },
    ]

    step("Wait dual-link BGP sessions")
    wait_checks(rt, session_checks, timeout=30)

    step("Reboot a and wait CLI reconnect")
    # reboot 后要等会话重建，配置必须存活：先 save 落盘到 startup-config
    reboot_device(rt, "a", timeout=120, save_config=True)

    step("Wait dual-link BGP sessions after reboot")
    wait_checks(rt, session_checks, timeout=30)


def _run_inner_ipv6(
    rt: TopologyRuntime,
    a_ge1_peer6: str,
    a_ge2_peer6: str,
    b_ge1_peer6: str,
    b_ge2_peer6: str,
) -> None:
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
            f"neighbor {a_ge1_peer6} as 65002",
            f"neighbor {a_ge2_peer6} as 65002",
            "af ipv6-unicast",
            f"neighbor {a_ge1_peer6} enable",
            f"neighbor {a_ge2_peer6} enable",
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
            f"neighbor {b_ge1_peer6} as 65001",
            f"neighbor {b_ge2_peer6} as 65001",
            "af ipv6-unicast",
            f"neighbor {b_ge1_peer6} enable",
            f"neighbor {b_ge2_peer6} enable",
            "exit",
            "end",
        ],
    )

    session_checks = [
        {
            "device": "a",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [a_ge1_peer6],
                "regex": [rf"(?im)^\s*{re.escape(a_ge1_peer6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "a(GE-1)->b ipv6-unicast",
        },
        {
            "device": "a",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [a_ge2_peer6],
                "regex": [rf"(?im)^\s*{re.escape(a_ge2_peer6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "a(GE-2)->b ipv6-unicast",
        },
        {
            "device": "b",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [b_ge1_peer6],
                "regex": [rf"(?im)^\s*{re.escape(b_ge1_peer6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "b(GE-1)->a ipv6-unicast",
        },
        {
            "device": "b",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [b_ge2_peer6],
                "regex": [rf"(?im)^\s*{re.escape(b_ge2_peer6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "b(GE-2)->a ipv6-unicast",
        },
    ]

    step("Wait dual-link BGP sessions")
    wait_checks(rt, session_checks, timeout=30)

    step("Reboot a and wait CLI reconnect")
    # reboot 后要等会话重建，配置必须存活：先 save 落盘到 startup-config
    reboot_device(rt, "a", timeout=120, save_config=True)

    step("Wait dual-link BGP sessions after reboot")
    wait_checks(rt, session_checks, timeout=30)
