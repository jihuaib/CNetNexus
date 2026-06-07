#!/usr/bin/env python3
"""
BGP import-route dual-stack check script.

Goal:
- enable `import-route static` for IPv4/IPv6 on r2
- inject IPv4/IPv6 static routes on r2
- verify r2 imports the static routes into local BGP RIB
- verify r1 receives the routes from r2
- disable `import-route static`
- verify the imported BGP routes are withdrawn while the original static routes remain
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TEST_V4_PREFIX_ADDR = "10.20.20.0"
TEST_V4_PREFIX_LEN = "24"
TEST_V4_PREFIX = f"{TEST_V4_PREFIX_ADDR}/{TEST_V4_PREFIX_LEN}"

TEST_V6_PREFIX_ADDR = "2001:db8:2020::"
TEST_V6_PREFIX_LEN = "64"
TEST_V6_PREFIX = f"{TEST_V6_PREFIX_ADDR}/{TEST_V6_PREFIX_LEN}"


def _cleanup_case_config(rt: TopologyRuntime, r2_route_nh: str, r2_route_nh6: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN} {r2_route_nh}",
            f"no route static ipv6 {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN} {r2_route_nh6}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    """
    Entry called by module_runner.
    """
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)
    r2_route_nh = str(g_top.r2.GE_1.peer_ip)
    r2_route_nh6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        step("Ensure BGP base config")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"],
        )

        step("Ensure BGP dual-stack neighbors + import-route static")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_ip} as 65002",
                f"neighbor {r1_peer_ip6} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "exit",
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
                f"neighbor {r2_peer_ip} as 65001",
                f"neighbor {r2_peer_ip6} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_peer_ip6} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Check ROUTE subscriptions are installed")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show route subscribe ipv4",
                    "contains": ["Route Subscribers", "bgp", "static", "ipv4"],
                    "regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv4\s*$"],
                    "label": "r2 route ipv4 static subscription installed",
                },
                {
                    "device": "r2",
                    "command": "show route subscribe ipv6",
                    "contains": ["Route Subscribers", "bgp", "static", "ipv6"],
                    "regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv6\s*$"],
                    "label": "r2 route ipv6 static subscription installed",
                },
            ],
            timeout=10,
        )

        step("Wait BGP sessions")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [r1_peer_ip],
                    "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 ipv4-unicast",
                },
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "contains": [r1_peer_ip6],
                    "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 ipv6-unicast",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [r2_peer_ip],
                    "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2->r1 ipv4-unicast",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "contains": [r2_peer_ip6],
                    "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2->r1 ipv6-unicast",
                },
            ],
            timeout=40,
        )

        step("Inject static routes on r2 for import-route")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route static ipv4 {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN} {r2_route_nh}",
                f"route static ipv6 {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN} {r2_route_nh6}",
                "end",
            ],
        )

        step("Check imported routes on r2 local and r1 peer BGP RIB")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [TEST_V4_PREFIX],
                    "label": "r2 local imported IPv4 static route",
                },
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [TEST_V6_PREFIX],
                    "label": "r2 local imported IPv6 static route",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [TEST_V4_PREFIX],
                    "label": "r1 learned IPv4 route from r2",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [TEST_V6_PREFIX],
                    "label": "r1 learned IPv6 route from r2",
                },
            ],
            timeout=40,
        )

        step("Disable import-route static on r2")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "af ipv4-unicast",
                "no import-route static",
                "exit",
                "af ipv6-unicast",
                "no import-route static",
                "exit",
                "end",
            ],
        )

        step("Check ROUTE subscriptions are removed")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show route subscribe ipv4",
                    "contains": ["(no subscribers)"],
                    "not_regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv4\s*$"],
                    "label": "r2 route ipv4 static subscription removed",
                },
                {
                    "device": "r2",
                    "command": "show route subscribe ipv6",
                    "contains": ["(no subscribers)"],
                    "not_regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv6\s*$"],
                    "label": "r2 route ipv6 static subscription removed",
                },
            ],
            timeout=10,
        )

        step("Check no import-route withdraws BGP copies but keeps static route")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [TEST_V4_PREFIX],
                    "label": "r2 imported IPv4 route removed from BGP RIB",
                },
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "not_contains": [TEST_V6_PREFIX],
                    "label": "r2 imported IPv6 route removed from BGP RIB",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [TEST_V4_PREFIX],
                    "label": "r1 withdrawn IPv4 route removed from BGP RIB",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "not_contains": [TEST_V6_PREFIX],
                    "label": "r1 withdrawn IPv6 route removed from BGP RIB",
                },
                {
                    "device": "r2",
                    "command": "show route static ipv4",
                    "contains": [TEST_V4_PREFIX, r2_route_nh],
                    "label": "r2 original IPv4 static route remains",
                },
                {
                    "device": "r2",
                    "command": "show route static ipv6",
                    "contains": [TEST_V6_PREFIX, r2_route_nh6],
                    "label": "r2 original IPv6 static route remains",
                },
            ],
            timeout=40,
        )

        print("BGP import-route dual-stack check passed.")
    finally:
        _cleanup_case_config(rt, r2_route_nh, r2_route_nh6)
