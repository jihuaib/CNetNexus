#!/usr/bin/env python3
"""
eBGP AS loop detection check (dual-stack: IPv4 + IPv6).

Topology: r1 (AS 100) --- r2 (AS 200) --- r3 (AS 100)

Goal:
- r1 imports static routes (IPv4 + IPv6) and advertises to r2 via eBGP
- r2 learns routes from r1 (AS_PATH: 100) and advertises to r3
- r3 (AS 100) must reject the routes because AS 100 already exists in
  the AS_PATH, triggering AS loop detection
- Verify r3 never installs the routes in its BGP RIB
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    g_top,
    hold_check,
    require_devices,
    run_cmds,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


AS_R1 = "100"
AS_R2 = "200"
AS_R3 = "100"

V4_PREFIX_ADDR = "10.99.99.0"
V4_PREFIX_LEN = "24"
V4_PREFIX = f"{V4_PREFIX_ADDR}/{V4_PREFIX_LEN}"

V6_PREFIX_ADDR = "2001:db8:9999::"
V6_PREFIX_LEN = "64"
V6_PREFIX = f"{V6_PREFIX_ADDR}/{V6_PREFIX_LEN}"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _cleanup(
    rt: TopologyRuntime,
    *,
    r1_nh4: str,
    r1_nh6: str,
) -> None:
    step("Cleanup eBGP AS loop detection config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {V4_PREFIX_ADDR} {V4_PREFIX_LEN} {r1_nh4}",
            f"no route ipv6 {V6_PREFIX_ADDR} {V6_PREFIX_LEN} {r1_nh6}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(rt=rt, device="r2", strict=False, commands=["config", "no bgp", "end"])
    run_cmds(rt=rt, device="r3", strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    # r1 <-> r2 link (GE-1 on both)
    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)      # r2's GE-1 IPv4
    r1_peer_v6 = str(g_top.r1.GE_1.peer_ip6)      # r2's GE-1 IPv6
    r2_to_r1_v4 = str(g_top.r2.GE_1.peer_ip)      # r1's GE-1 IPv4
    r2_to_r1_v6 = str(g_top.r2.GE_1.peer_ip6)     # r1's GE-1 IPv6

    # r2 <-> r3 link (r2 GE-2, r3 GE-1)
    r2_to_r3_v4 = str(g_top.r2.GE_2.peer_ip)      # r3's GE-1 IPv4
    r2_to_r3_v6 = str(g_top.r2.GE_2.peer_ip6)     # r3's GE-1 IPv6
    r3_peer_v4 = str(g_top.r3.GE_1.peer_ip)        # r2's GE-2 IPv4
    r3_peer_v6 = str(g_top.r3.GE_1.peer_ip6)       # r2's GE-2 IPv6

    try:
        step("Configure eBGP on all routers")
        run_cmds(
            rt=rt,
            device="r1",
            commands=["config", f"bgp {AS_R1}", "router-id 1.1.1.1", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=["config", f"bgp {AS_R2}", "router-id 2.2.2.2", "end"],
        )
        run_cmds(
            rt=rt,
            device="r3",
            commands=["config", f"bgp {AS_R3}", "router-id 3.3.3.3", "end"],
        )

        step("Configure eBGP neighbors and AF (IPv4 + IPv6)")
        # r1 (AS 100): neighbor r2 (AS 200), import-route static
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {AS_R1}",
                f"neighbor {r1_peer_v4} as {AS_R2}",
                f"neighbor {r1_peer_v6} as {AS_R2}",
                "af ipv4-unicast",
                f"neighbor {r1_peer_v4} enable",
                f"neighbor {r1_peer_v6} enable",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r1_peer_v4} enable",
                f"neighbor {r1_peer_v6} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )
        # r2 (AS 200): neighbor r1 (AS 100) + neighbor r3 (AS 100)
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {AS_R2}",
                f"neighbor {r2_to_r1_v4} as {AS_R1}",
                f"neighbor {r2_to_r1_v6} as {AS_R1}",
                f"neighbor {r2_to_r3_v4} as {AS_R3}",
                f"neighbor {r2_to_r3_v6} as {AS_R3}",
                "af ipv4-unicast",
                f"neighbor {r2_to_r1_v4} enable",
                f"neighbor {r2_to_r1_v6} enable",
                f"neighbor {r2_to_r3_v4} enable",
                f"neighbor {r2_to_r3_v6} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_to_r1_v4} enable",
                f"neighbor {r2_to_r1_v6} enable",
                f"neighbor {r2_to_r3_v4} enable",
                f"neighbor {r2_to_r3_v6} enable",
                "exit",
                "end",
            ],
        )
        # r3 (AS 100): neighbor r2 (AS 200)
        run_cmds(
            rt=rt,
            device="r3",
            commands=[
                "config",
                f"bgp {AS_R3}",
                f"neighbor {r3_peer_v4} as {AS_R2}",
                f"neighbor {r3_peer_v6} as {AS_R2}",
                "af ipv4-unicast",
                f"neighbor {r3_peer_v4} enable",
                f"neighbor {r3_peer_v6} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r3_peer_v4} enable",
                f"neighbor {r3_peer_v6} enable",
                "exit",
                "end",
            ],
        )

        step("Wait eBGP sessions r1<->r2 and r2<->r3 established")
        session_checks = [
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [_established_regex(r1_peer_v4)],
                "label": "r1->r2 ipv4 v4-peer",
            },
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv6-unicast",
                "regex": [_established_regex(r1_peer_v6)],
                "label": "r1->r2 ipv6 v6-peer",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [
                    _established_regex(r2_to_r1_v4),
                    _established_regex(r2_to_r3_v4),
                ],
                "label": "r2 ipv4 both peers",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv6-unicast",
                "regex": [
                    _established_regex(r2_to_r1_v6),
                    _established_regex(r2_to_r3_v6),
                ],
                "label": "r2 ipv6 both peers",
            },
            {
                "device": "r3",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [_established_regex(r3_peer_v4)],
                "label": "r3->r2 ipv4 v4-peer",
            },
            {
                "device": "r3",
                "command": "show bgp neighbor af ipv6-unicast",
                "regex": [_established_regex(r3_peer_v6)],
                "label": "r3->r2 ipv6 v6-peer",
            },
        ]
        wait_checks(rt, session_checks, timeout=40)

        step("Inject static routes on r1 (IPv4 + IPv6)")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {V4_PREFIX_ADDR} {V4_PREFIX_LEN} {r1_peer_v4}",
                f"route ipv6 {V6_PREFIX_ADDR} {V6_PREFIX_LEN} {r1_peer_v6}",
                "end",
            ],
        )

        step("Wait r2 learns routes from r1 via eBGP")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [V4_PREFIX],
                    "label": "r2 learned IPv4 route",
                },
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [V6_PREFIX],
                    "label": "r2 learned IPv6 route",
                },
            ],
            timeout=40,
        )

        step("Hold-check: r3 must NOT receive routes (AS loop detection)")
        hold_check(
            rt,
            device="r3",
            command="show bgp route af ipv4-unicast",
            duration=15,
            interval=3,
            not_contains=[V4_PREFIX],
            label="r3 IPv4 route absent (AS loop: AS 100 in path)",
        )
        hold_check(
            rt,
            device="r3",
            command="show bgp route af ipv6-unicast",
            duration=15,
            interval=3,
            not_contains=[V6_PREFIX],
            label="r3 IPv6 route absent (AS loop: AS 100 in path)",
        )

        print("eBGP AS loop detection dual-stack check passed.")
    finally:
        _cleanup(rt, r1_nh4=r1_peer_v4, r1_nh6=r1_peer_v6)
