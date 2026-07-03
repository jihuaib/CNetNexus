#!/usr/bin/env python3
"""
BGP EVPN neighbor negotiation check.

This case verifies that the EVPN AF can be configured on a BGP neighbor and
that both sides negotiate MP capability AFI=25/SAFI=70.
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def _cleanup(rt: TopologyRuntime) -> None:
    for dev in ("r1", "r2"):
        run_cmds(rt=rt, device=dev, strict=False, commands=["config", "no bgp", "end"])


def _evpn_session_checks(r1_peer_ip: str, r2_peer_ip: str) -> list[dict[str, object]]:
    return [
        {
            "device": "r1",
            "command": "show bgp neighbor af evpn",
            "contains": [r1_peer_ip, "AF: evpn"],
            "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r1->r2 EVPN established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af evpn",
            "contains": [r2_peer_ip, "AF: evpn"],
            "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r2->r1 EVPN established",
        },
    ]


def _evpn_detail_checks(r1_peer_ip: str, r2_peer_ip: str) -> list[dict[str, object]]:
    return [
        {
            "device": "r1",
            "command": f"show bgp neighbor af evpn {r1_peer_ip}",
            "contains": [
                r1_peer_ip,
                "Session State",
                "Established",
                "Local Address Families",
                "Remote Address Families",
                "Negotiated Address Families",
                "afi=25 safi=70",
                "evpn",
            ],
            "regex": [
                r"(?ims)^\s*Received Messages:\s*$.*?^\s*OPEN\s*:\s*[1-9]\d*\s*$",
                r"(?ims)^\s*Sent Messages:\s*$.*?^\s*OPEN\s*:\s*[1-9]\d*\s*$",
            ],
            "label": "r1 EVPN MP capability negotiated",
        },
        {
            "device": "r2",
            "command": f"show bgp neighbor af evpn {r2_peer_ip}",
            "contains": [
                r2_peer_ip,
                "Session State",
                "Established",
                "Local Address Families",
                "Remote Address Families",
                "Negotiated Address Families",
                "afi=25 safi=70",
                "evpn",
            ],
            "regex": [
                r"(?ims)^\s*Received Messages:\s*$.*?^\s*OPEN\s*:\s*[1-9]\d*\s*$",
                r"(?ims)^\s*Sent Messages:\s*$.*?^\s*OPEN\s*:\s*[1-9]\d*\s*$",
            ],
            "label": "r2 EVPN MP capability negotiated",
        },
    ]


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    try:
        step("Cleanup stale BGP config")
        _cleanup(rt)

        step("Configure EVPN AF on direct eBGP neighbors")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                "timer connect-retry 5",
                f"neighbor {r1_peer_ip} as 65002",
                "af evpn",
                f"neighbor {r1_peer_ip} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                "bgp 65002",
                "router-id 2.2.2.2",
                "timer connect-retry 5",
                f"neighbor {r2_peer_ip} as 65001",
                "af evpn",
                f"neighbor {r2_peer_ip} enable",
                "exit",
                "end",
            ],
        )

        step("Wait EVPN neighbor sessions Established")
        wait_checks(rt, _evpn_session_checks(r1_peer_ip, r2_peer_ip), timeout=40)

        step("Verify EVPN MP capability negotiation")
        wait_checks(rt, _evpn_detail_checks(r1_peer_ip, r2_peer_ip), timeout=40)
    finally:
        step("Cleanup BGP config")
        _cleanup(rt)

    print("BGP EVPN neighbor negotiation check passed.")
