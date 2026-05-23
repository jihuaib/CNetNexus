#!/usr/bin/env python3
"""
BGP VRF import-route connected validation for IPv4 and IPv6.

Covers:
- create VRF "red" on r1/r2 with IPv4/IPv6 AF RD
- move GE-1 into the VRF and establish dual-stack BGP VRF neighbors
- create a loop interface inside r2 VRF
- enable import-route connected under r2 BGP VRF AFs
- verify r2 imports the loop connected routes and r1 learns them
- verify r1 route/FIB/OS FIB are programmed and ping vrf red reaches the r2 loop
- disable import-route connected and verify learned routes are withdrawn.
"""

from __future__ import annotations

import ipaddress
import re
import time

from module_api import (  # noqa: E402
    g_top,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
GE_IF = "GE-1"

R1_V4 = "10.98.0.1"
R2_V4 = "10.98.0.2"
V4_LEN = 30

R1_V6 = "2001:db8:98::1"
R2_V6 = "2001:db8:98::2"
V6_LEN = 64

TEST_V4_PREFIX_ADDR = "10.248.40.0"
TEST_V4_PREFIX_LEN = 24
TEST_V4_PREFIX = f"{TEST_V4_PREFIX_ADDR}/{TEST_V4_PREFIX_LEN}"

TEST_V6_PREFIX_ADDR = "2001:db8:248:40::"
TEST_V6_PREFIX_LEN = 64
TEST_V6_PREFIX = f"{TEST_V6_PREFIX_ADDR}/{TEST_V6_PREFIX_LEN}"


def _network(addr: str, prefix_len: int) -> str:
    net = ipaddress.ip_network(f"{addr}/{prefix_len}", strict=False)
    return f"{net.network_address}/{prefix_len}"


def _cleanup(rt: TopologyRuntime, base: dict[str, dict[str, str | int]]) -> None:
    for dev in ("r1", "r2"):
        b = base[dev]
        local_v4 = R1_V4 if dev == "r1" else R2_V4
        local_v6 = R1_V6 if dev == "r1" else R2_V6
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"no route static ipv4 {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN} {R1_V4} vrf {VRF_NAME}",
                f"no route static ipv6 {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN} {R1_V6} vrf {VRF_NAME}",
                "no bgp",
                f"if {GE_IF}",
                "no shutdown",
                f"no ip address {local_v4} {V4_LEN}",
                f"no ipv6 address {local_v6} {V6_LEN}",
                "no vrf forwarding",
                f"ip address {b['v4']} {b['v4_len']}",
                f"ipv6 address {b['v6']} {b['v6_len']}",
                "exit",
                f"no vrf {VRF_NAME}",
                "end",
            ],
        )


def _setup_vrf_and_interface(
    rt: TopologyRuntime,
    *,
    device: str,
    local_v4: str,
    local_v6: str,
    rd_v4: str,
    rd_v6: str,
) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"route-distinguisher {rd_v4}",
            "exit",
            "af ipv6-unicast",
            f"route-distinguisher {rd_v6}",
            "exit",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device=device,
        command=f"show vrf name {VRF_NAME}",
        timeout=10,
        interval=1,
        contains=["VRF Detail:", f"Name           : {VRF_NAME}"],
        label=f"{device} vrf {VRF_NAME} ready",
    )
    # Let the VRF cache replay reach IF/ROUTE/BGP before entering VRF views.
    time.sleep(2)

    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {local_v4} {V4_LEN}",
            f"ipv6 address {local_v6} {V6_LEN}",
            "exit",
            "end",
        ],
    )


def _wait_vrf_connected_routes(rt: TopologyRuntime) -> None:
    v4_net = _network(R1_V4, V4_LEN)
    v6_net = _network(R1_V6, V6_LEN)
    checks = []
    for dev in ("r1", "r2"):
        checks.extend(
            [
                {
                    "device": dev,
                    "command": f"show route ipv4 {v4_net.split('/')[0]} {V4_LEN} vrf {VRF_NAME}",
                    "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                    "label": f"{dev} vrf ipv4 connected route ready",
                },
                {
                    "device": dev,
                    "command": f"show route ipv6 {v6_net.split('/')[0]} {V6_LEN} vrf {VRF_NAME}",
                    "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                    "label": f"{dev} vrf ipv6 connected route ready",
                },
            ]
        )
    wait_checks(rt, checks, timeout=20, interval=2)


def _configure_bgp_vrf(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            "bgp 65001",
            "router-id 1.1.1.1",
            f"vrf {VRF_NAME}",
            "router-id 1.1.1.1",
            f"neighbor {R2_V4} as 65002",
            f"neighbor {R2_V6} as 65002",
            "af ipv4-unicast",
            f"neighbor {R2_V4} enable",
            "exit",
            "af ipv6-unicast",
            f"neighbor {R2_V6} enable",
            "exit",
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
            f"vrf {VRF_NAME}",
            "router-id 2.2.2.2",
            f"neighbor {R1_V4} as 65001",
            f"neighbor {R1_V6} as 65001",
            "af ipv4-unicast",
            f"neighbor {R1_V4} enable",
            "import-route static",
            "exit",
            "af ipv6-unicast",
            f"neighbor {R1_V6} enable",
            "import-route static",
            "exit",
            "exit",
            "end",
        ],
    )


def _wait_dual_stack_sessions(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                "contains": [R2_V4, "AF: ipv4-unicast"],
                "regex": [rf"(?im)^\s*{re.escape(R2_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1 vrf ipv4 session up",
            },
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv6-unicast vrf {VRF_NAME}",
                "contains": [R2_V6, "AF: ipv6-unicast"],
                "regex": [rf"(?im)^\s*{re.escape(R2_V6)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1 vrf ipv6 session up",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                "contains": [R1_V4, "AF: ipv4-unicast"],
                "regex": [rf"(?im)^\s*{re.escape(R1_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2 vrf ipv4 session up",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast vrf {VRF_NAME}",
                "contains": [R1_V6, "AF: ipv6-unicast"],
                "regex": [rf"(?im)^\s*{re.escape(R1_V6)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2 vrf ipv6 session up",
            },
        ],
        timeout=60,
        interval=2,
    )


def _wait_import_subscriptions(rt: TopologyRuntime, *, present: bool) -> None:
    if present:
        checks = [
            {
                "device": "r2",
                "command": f"show route ipv4 subscribe vrf {VRF_NAME}",
                "contains": ["Route Subscribers", f"VRF filter: {VRF_NAME}"],
                "regex": [r"(?im)^\s*bgp\s+static\s+[1-9]\d*\s+ipv4\s*$"],
                "label": "r2 vrf ipv4 static subscription installed",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 subscribe vrf {VRF_NAME}",
                "contains": ["Route Subscribers", f"VRF filter: {VRF_NAME}"],
                "regex": [r"(?im)^\s*bgp\s+static\s+[1-9]\d*\s+ipv6\s*$"],
                "label": "r2 vrf ipv6 static subscription installed",
            },
        ]
    else:
        checks = [
            {
                "device": "r2",
                "command": f"show route ipv4 subscribe vrf {VRF_NAME}",
                "contains": ["(no subscribers)"],
                "not_regex": [r"(?im)^\s*bgp\s+static\s+\d+\s+ipv4\s*$"],
                "label": "r2 vrf ipv4 static subscription removed",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 subscribe vrf {VRF_NAME}",
                "contains": ["(no subscribers)"],
                "not_regex": [r"(?im)^\s*bgp\s+static\s+\d+\s+ipv6\s*$"],
                "label": "r2 vrf ipv6 static subscription removed",
            },
        ]
    wait_checks(rt, checks, timeout=15, interval=2)


def _wait_vrf_static_routes(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show route ipv4 {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN} vrf {VRF_NAME}",
                "contains": [TEST_V4_PREFIX, R1_V4],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
                "label": "r2 vrf ipv4 static route installed",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN} vrf {VRF_NAME}",
                "contains": [TEST_V6_PREFIX, R1_V6],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
                "label": "r2 vrf ipv6 static route installed",
            },
        ],
        timeout=20,
        interval=2,
    )


def _wait_bgp_imported_routes(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN}",
                "contains": [f"BGP Route Detail: {TEST_V4_PREFIX}", "Imported", f"NextHop  : {R1_V4}"],
                "label": "r2 imported vrf ipv4 static route detail",
            },
            {
                "device": "r2",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME} {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN}",
                "contains": [f"BGP Route Detail: {TEST_V6_PREFIX}", "Imported", f"NextHop  : {R1_V6}"],
                "label": "r2 imported vrf ipv6 static route detail",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                "contains": [TEST_V4_PREFIX],
                "label": "r1 learned vrf ipv4 route",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME}",
                "contains": [TEST_V6_PREFIX],
                "label": "r1 learned vrf ipv6 route",
            },
            {
                "device": "r1",
                "command": "show bgp route af ipv4-unicast",
                "not_contains": [TEST_V4_PREFIX],
                "label": "r1 public ipv4 rib isolated from vrf route",
            },
            {
                "device": "r1",
                "command": "show bgp route af ipv6-unicast",
                "not_contains": [TEST_V6_PREFIX],
                "label": "r1 public ipv6 rib isolated from vrf route",
            },
        ],
        timeout=40,
        interval=2,
    )


def _disable_import_route(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            "bgp 65002",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "no import-route static",
            "exit",
            "af ipv6-unicast",
            "no import-route static",
            "exit",
            "exit",
            "end",
        ],
    )


def _wait_bgp_routes_withdrawn(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                "not_contains": [TEST_V4_PREFIX],
                "label": "r2 imported vrf ipv4 route withdrawn",
            },
            {
                "device": "r2",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME}",
                "not_contains": [TEST_V6_PREFIX],
                "label": "r2 imported vrf ipv6 route withdrawn",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                "not_contains": [TEST_V4_PREFIX],
                "label": "r1 learned vrf ipv4 route withdrawn",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME}",
                "not_contains": [TEST_V6_PREFIX],
                "label": "r1 learned vrf ipv6 route withdrawn",
            },
            {
                "device": "r2",
                "command": f"show route ipv4 {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN} vrf {VRF_NAME}",
                "contains": [TEST_V4_PREFIX, R1_V4],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
                "label": "r2 original vrf ipv4 static route remains",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN} vrf {VRF_NAME}",
                "contains": [TEST_V6_PREFIX, R1_V6],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
                "label": "r2 original vrf ipv6 static route remains",
            },
        ],
        timeout=40,
        interval=2,
    )


def _legacy_static_run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    base = {
        "r1": {
            "v4": str(g_top.r1.GE_1.ip),
            "v4_len": int(g_top.r1.GE_1.prefix),
            "v6": str(g_top.r1.GE_1.ip6),
            "v6_len": int(g_top.r1.GE_1.prefix6),
        },
        "r2": {
            "v4": str(g_top.r2.GE_1.ip),
            "v4_len": int(g_top.r2.GE_1.prefix),
            "v6": str(g_top.r2.GE_1.ip6),
            "v6_len": int(g_top.r2.GE_1.prefix6),
        },
    }

    try:
        _cleanup(rt, base)

        step("Create dual-stack VRF on r1/r2 and bind GE-1")
        _setup_vrf_and_interface(rt, device="r1", local_v4=R1_V4, local_v6=R1_V6, rd_v4="65001:98", rd_v6="65001:99")
        _setup_vrf_and_interface(rt, device="r2", local_v4=R2_V4, local_v6=R2_V6, rd_v4="65002:98", rd_v6="65002:99")
        _wait_vrf_connected_routes(rt)

        step("Configure BGP VRF neighbors and r2 import-route static")
        _configure_bgp_vrf(rt)
        _wait_import_subscriptions(rt, present=True)

        step("Wait dual-stack BGP VRF sessions")
        _wait_dual_stack_sessions(rt)

        step("Inject IPv4/IPv6 static routes in r2 VRF")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"route static ipv4 {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN} {R1_V4} vrf {VRF_NAME}",
                f"route static ipv6 {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN} {R1_V6} vrf {VRF_NAME}",
                "end",
            ],
        )
        _wait_vrf_static_routes(rt)

        step("Verify r2 imports VRF static routes and r1 learns them")
        _wait_bgp_imported_routes(rt)

        step("Disable r2 BGP VRF import-route static")
        _disable_import_route(rt)
        _wait_import_subscriptions(rt, present=False)

        step("Verify imported BGP copies are withdrawn but VRF static routes remain")
        _wait_bgp_routes_withdrawn(rt)

        print("BGP VRF import-route static dual-stack check passed.")
    finally:
        _cleanup(rt, base)


# ---------------------------------------------------------------------------
# Connected-route VRF import scenario.
#
# Keep the legacy static helpers above unused for now; this final run()
# definition is the script entry used by module_runner.
# ---------------------------------------------------------------------------

LOOP_ID = 98
LOOP_V4 = "10.248.40.1"
LOOP_V4_LEN = 32
LOOP_V4_PREFIX = f"{LOOP_V4}/{LOOP_V4_LEN}"
LOOP_V6 = "2001:db8:248:40::1"
LOOP_V6_LEN = 128
LOOP_V6_PREFIX = f"{LOOP_V6}/{LOOP_V6_LEN}"
PING_SUCCESS_RE = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
PING_FAIL_RE = r"(?im)(?:\b100(?:\.0)?%\s+packet loss\b|network is unreachable|no route to host|unreachable)"


def _cleanup_connected(rt: TopologyRuntime, base: dict[str, dict[str, str | int]]) -> None:
    for dev in ("r1", "r2"):
        b = base[dev]
        local_v4 = R1_V4 if dev == "r1" else R2_V4
        local_v6 = R1_V6 if dev == "r1" else R2_V6
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                "no bgp",
                f"no if loop {LOOP_ID}",
                f"if {GE_IF}",
                "no shutdown",
                f"no ip address {local_v4} {V4_LEN}",
                f"no ipv6 address {local_v6} {V6_LEN}",
                "no vrf forwarding",
                f"ip address {b['v4']} {b['v4_len']}",
                f"ipv6 address {b['v6']} {b['v6_len']}",
                "exit",
                f"no vrf {VRF_NAME}",
                "end",
            ],
        )


def _configure_r2_vrf_loop(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"if loop {LOOP_ID}",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {LOOP_V4} {LOOP_V4_LEN}",
            f"ipv6 address {LOOP_V6} {LOOP_V6_LEN}",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show route ipv4 {LOOP_V4} {LOOP_V4_LEN} vrf {VRF_NAME}",
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                "label": "r2 vrf loop ipv4 connected route",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 {LOOP_V6} {LOOP_V6_LEN} vrf {VRF_NAME}",
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                "label": "r2 vrf loop ipv6 connected route",
            },
        ],
        timeout=20,
        interval=2,
    )


def _configure_bgp_vrf_connected(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            "bgp 65001",
            "router-id 1.1.1.1",
            f"vrf {VRF_NAME}",
            "router-id 1.1.1.1",
            f"neighbor {R2_V4} as 65002",
            f"neighbor {R2_V6} as 65002",
            "af ipv4-unicast",
            f"neighbor {R2_V4} enable",
            "exit",
            "af ipv6-unicast",
            f"neighbor {R2_V6} enable",
            "exit",
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
            f"vrf {VRF_NAME}",
            "router-id 2.2.2.2",
            f"neighbor {R1_V4} as 65001",
            f"neighbor {R1_V6} as 65001",
            "af ipv4-unicast",
            f"neighbor {R1_V4} enable",
            "import-route connected",
            "exit",
            "af ipv6-unicast",
            f"neighbor {R1_V6} enable",
            "import-route connected",
            "exit",
            "exit",
            "end",
        ],
    )


def _wait_connected_subscriptions(rt: TopologyRuntime, *, present: bool) -> None:
    if present:
        checks = [
            {
                "device": "r2",
                "command": f"show route ipv4 subscribe vrf {VRF_NAME}",
                "contains": ["Route Subscribers", f"VRF filter: {VRF_NAME}"],
                "regex": [r"(?im)^\s*bgp\s+connected\s+[1-9]\d*\s+ipv4\s*$"],
                "label": "r2 vrf ipv4 connected subscription installed",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 subscribe vrf {VRF_NAME}",
                "contains": ["Route Subscribers", f"VRF filter: {VRF_NAME}"],
                "regex": [r"(?im)^\s*bgp\s+connected\s+[1-9]\d*\s+ipv6\s*$"],
                "label": "r2 vrf ipv6 connected subscription installed",
            },
        ]
    else:
        checks = [
            {
                "device": "r2",
                "command": f"show route ipv4 subscribe vrf {VRF_NAME}",
                "contains": ["(no subscribers)"],
                "not_regex": [r"(?im)^\s*bgp\s+connected\s+\d+\s+ipv4\s*$"],
                "label": "r2 vrf ipv4 connected subscription removed",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 subscribe vrf {VRF_NAME}",
                "contains": ["(no subscribers)"],
                "not_regex": [r"(?im)^\s*bgp\s+connected\s+\d+\s+ipv6\s*$"],
                "label": "r2 vrf ipv6 connected subscription removed",
            },
        ]
    wait_checks(rt, checks, timeout=15, interval=2)


def _wait_bgp_connected_imported_and_learned(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} {LOOP_V4} {LOOP_V4_LEN}",
                "contains": [f"BGP Route Detail: {LOOP_V4_PREFIX}", "Imported"],
                "label": "r2 imported vrf loop ipv4 route",
            },
            {
                "device": "r2",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME} {LOOP_V6} {LOOP_V6_LEN}",
                "contains": [f"BGP Route Detail: {LOOP_V6_PREFIX}", "Imported"],
                "label": "r2 imported vrf loop ipv6 route",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                "contains": [LOOP_V4_PREFIX],
                "label": "r1 learned vrf loop ipv4 route",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME}",
                "contains": [LOOP_V6_PREFIX],
                "label": "r1 learned vrf loop ipv6 route",
            },
            {
                "device": "r1",
                "command": "show bgp route af ipv4-unicast",
                "not_contains": [LOOP_V4_PREFIX],
                "label": "r1 public ipv4 rib isolated from vrf route",
            },
            {
                "device": "r1",
                "command": "show bgp route af ipv6-unicast",
                "not_contains": [LOOP_V6_PREFIX],
                "label": "r1 public ipv6 rib isolated from vrf route",
            },
        ],
        timeout=40,
        interval=2,
    )


def _wait_r1_vrf_data_plane(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show route ipv4 {LOOP_V4} {LOOP_V4_LEN} vrf {VRF_NAME}",
                "contains": [LOOP_V4_PREFIX, R2_V4],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*bgp\b"],
                "label": "r1 vrf route ipv4 has bgp path",
            },
            {
                "device": "r1",
                "command": f"show route ipv6 {LOOP_V6} {LOOP_V6_LEN} vrf {VRF_NAME}",
                "contains": [LOOP_V6_PREFIX, R2_V6],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*bgp\b"],
                "label": "r1 vrf route ipv6 has bgp path",
            },
            {
                "device": "r1",
                "command": f"show fib ipv4 {LOOP_V4} {LOOP_V4_LEN} vrf {VRF_NAME}",
                "contains": [f"FIB Route Detail: {LOOP_V4_PREFIX}", f"Nexthop   : {R2_V4}"],
                "regex": [
                    r"(?im)^\s*AFI\s*:\s*ipv4\s*$",
                    r"(?im)^\s*NH-Type\s*:\s*ip\s*$",
                    r"(?im)^\s*Installed\s*:\s*yes\s*$",
                    r"(?im)^\s*Skip OS\s*:\s*no\s*$",
                ],
                "label": "r1 vrf fib ipv4 installed",
            },
            {
                "device": "r1",
                "command": f"show fib ipv6 {LOOP_V6} {LOOP_V6_LEN} vrf {VRF_NAME}",
                "contains": [f"FIB Route Detail: {LOOP_V6_PREFIX}", f"Nexthop   : {R2_V6}"],
                "regex": [
                    r"(?im)^\s*AFI\s*:\s*ipv6\s*$",
                    r"(?im)^\s*NH-Type\s*:\s*ip\s*$",
                    r"(?im)^\s*Installed\s*:\s*yes\s*$",
                    r"(?im)^\s*Skip OS\s*:\s*no\s*$",
                ],
                "label": "r1 vrf fib ipv6 installed",
            },
            {
                "device": "r1",
                "command": f"show fib ipv4 os vrf {VRF_NAME}",
                "contains": [LOOP_V4_PREFIX],
                "label": "r1 vrf os fib ipv4 installed",
            },
            {
                "device": "r1",
                "command": f"show fib ipv6 os vrf {VRF_NAME}",
                "contains": [LOOP_V6_PREFIX],
                "label": "r1 vrf os fib ipv6 installed",
            },
        ],
        timeout=40,
        interval=2,
    )


def _wait_vrf_ping(rt: TopologyRuntime, *, command: str, expect_success: bool, label: str) -> None:
    wait_check(
        rt,
        device="r1",
        command=command,
        timeout=30,
        interval=2,
        regex=[PING_SUCCESS_RE] if expect_success else [PING_FAIL_RE],
        not_regex=[PING_FAIL_RE] if expect_success else [PING_SUCCESS_RE],
        normalize_whitespace=False,
        label=label,
    )


def _disable_connected_import_route(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            "bgp 65002",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "no import-route connected",
            "exit",
            "af ipv6-unicast",
            "no import-route connected",
            "exit",
            "exit",
            "end",
        ],
    )


def _wait_connected_routes_withdrawn(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                "not_contains": [LOOP_V4_PREFIX],
                "label": "r2 imported vrf loop ipv4 withdrawn",
            },
            {
                "device": "r2",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME}",
                "not_contains": [LOOP_V6_PREFIX],
                "label": "r2 imported vrf loop ipv6 withdrawn",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                "not_contains": [LOOP_V4_PREFIX],
                "label": "r1 vrf loop ipv4 withdrawn",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME}",
                "not_contains": [LOOP_V6_PREFIX],
                "label": "r1 vrf loop ipv6 withdrawn",
            },
            {
                "device": "r1",
                "command": f"show fib ipv4 {LOOP_V4} {LOOP_V4_LEN} vrf {VRF_NAME}",
                "not_contains": [f"FIB Route Detail: {LOOP_V4_PREFIX}"],
                "label": "r1 vrf fib ipv4 withdrawn",
            },
            {
                "device": "r1",
                "command": f"show fib ipv6 {LOOP_V6} {LOOP_V6_LEN} vrf {VRF_NAME}",
                "not_contains": [f"FIB Route Detail: {LOOP_V6_PREFIX}"],
                "label": "r1 vrf fib ipv6 withdrawn",
            },
            {
                "device": "r2",
                "command": f"show route ipv4 {LOOP_V4} {LOOP_V4_LEN} vrf {VRF_NAME}",
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                "label": "r2 original vrf loop ipv4 connected route remains",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 {LOOP_V6} {LOOP_V6_LEN} vrf {VRF_NAME}",
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                "label": "r2 original vrf loop ipv6 connected route remains",
            },
        ],
        timeout=40,
        interval=2,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    base = {
        "r1": {
            "v4": str(g_top.r1.GE_1.ip),
            "v4_len": int(g_top.r1.GE_1.prefix),
            "v6": str(g_top.r1.GE_1.ip6),
            "v6_len": int(g_top.r1.GE_1.prefix6),
        },
        "r2": {
            "v4": str(g_top.r2.GE_1.ip),
            "v4_len": int(g_top.r2.GE_1.prefix),
            "v6": str(g_top.r2.GE_1.ip6),
            "v6_len": int(g_top.r2.GE_1.prefix6),
        },
    }

    try:
        _cleanup_connected(rt, base)

        step("Create dual-stack VRF on r1/r2 and bind GE-1")
        _setup_vrf_and_interface(rt, device="r1", local_v4=R1_V4, local_v6=R1_V6, rd_v4="65001:98", rd_v6="65001:99")
        _setup_vrf_and_interface(rt, device="r2", local_v4=R2_V4, local_v6=R2_V6, rd_v4="65002:98", rd_v6="65002:99")
        _wait_vrf_connected_routes(rt)

        step("Create r2 loop in VRF as connected import source")
        _configure_r2_vrf_loop(rt)

        step("Configure BGP VRF neighbors and r2 import-route connected")
        _configure_bgp_vrf_connected(rt)
        _wait_connected_subscriptions(rt, present=True)

        step("Wait dual-stack BGP VRF sessions")
        _wait_dual_stack_sessions(rt)

        step("Verify BGP imports and advertises r2 VRF loop connected routes")
        _wait_bgp_connected_imported_and_learned(rt)

        step("Verify r1 route/FIB/OS FIB and ping vrf data plane")
        _wait_r1_vrf_data_plane(rt)
        _wait_vrf_ping(rt, command=f"ping {LOOP_V4} vrf {VRF_NAME}", expect_success=True, label="r1 ping vrf red r2 loop ipv4")
        _wait_vrf_ping(
            rt,
            command=f"ping ipv6 {LOOP_V6} vrf {VRF_NAME}",
            expect_success=True,
            label="r1 ping vrf red r2 loop ipv6",
        )

        step("Disable r2 BGP VRF import-route connected")
        _disable_connected_import_route(rt)
        _wait_connected_subscriptions(rt, present=False)

        step("Verify learned routes/FIB are withdrawn and r2 connected loop remains")
        _wait_connected_routes_withdrawn(rt)
        _wait_vrf_ping(
            rt,
            command=f"ping {LOOP_V4} vrf {VRF_NAME}",
            expect_success=False,
            label="r1 ping vrf red r2 loop ipv4 fails after withdraw",
        )

        print("BGP VRF import-route connected dual-stack check passed.")
    finally:
        _cleanup_connected(rt, base)
