#!/usr/bin/env python3
"""
BGP vs static preference switch dual-stack check.

Goal:
- r2 advertises a prefix to r1 via BGP (imported from r2 static route)
- r1 configures the same prefix as static route
- verify r1 prefers static route in both Route RIB and OS table
- remove r1 static route
- verify r1 falls back to BGP route in both Route RIB and OS table
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    g_top,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
    wait_fib_ipv4_route,
    wait_fib_ipv6_route,
)
from top_runner import TopologyRuntime  # noqa: E402


TARGET_PREFIX_ADDR_V4 = "10.40.40.0"
TARGET_MASK_V4 = "24"
TARGET_PREFIX_V4 = f"{TARGET_PREFIX_ADDR_V4}/24"

TARGET_PREFIX_ADDR_V6 = "2001:db8:4040::"
TARGET_MASK_V6 = "64"
TARGET_PREFIX_V6 = f"{TARGET_PREFIX_ADDR_V6}/{TARGET_MASK_V6}"


def _wait_route_best_static_backup_bgp_ipv4(
    rt: TopologyRuntime, *, device: str, destination: str, timeout: int
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {TARGET_MASK_V4}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for", "Total 2 path(s)"],
        count={"Path [": 2},
        regex=[
            r"(?is)Path\s*\[1\]\s*:\s*static\b.*?Preference\s*:\s*1\b",
            r"(?is)Path\s*\[2\]\s*:\s*bgp\b.*?Preference\s*:\s*200\b",
        ],
        label=f"{device} best=static backup=bgp {destination}",
    )


def _wait_route_best_bgp_only_ipv4(rt: TopologyRuntime, *, device: str, destination: str, timeout: int) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {TARGET_MASK_V4}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for", "Total 1 path(s)"],
        count={"Path [": 1},
        regex=[r"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Preference\s*:\s*200\b"],
        not_regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
        label=f"{device} best=bgp only {destination}",
    )


def _wait_os_best_proto_ipv4(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    gateway: str,
    proto: str,
    timeout: int,
) -> None:
    alt_proto = "bgp" if proto == "static" else "static"
    best_row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(proto)}\s+\d+(?:\s+\S+)?\s*$"
    )
    alt_row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(alt_proto)}\s+\d+(?:\s+\S+)?\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show fib os ipv4",
        timeout=timeout,
        interval=2,
        regex=[best_row_regex],
        not_regex=[alt_row_regex],
        label=f"{device} os-best {prefix} proto={proto}",
    )
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_fib_ipv4_route(
        rt,
        device=device,
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=True,
        nexthop=gateway,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} fib-best {prefix} proto={proto}",
    )


def _wait_route_best_static_backup_bgp_ipv6(
    rt: TopologyRuntime, *, device: str, destination: str, timeout: int
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv6 {destination} {TARGET_MASK_V6}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for", "Total 2 path(s)"],
        count={"Path [": 2},
        regex=[
            r"(?is)Path\s*\[1\]\s*:\s*static\b.*?Preference\s*:\s*1\b",
            r"(?is)Path\s*\[2\]\s*:\s*bgp\b.*?Preference\s*:\s*200\b",
        ],
        label=f"{device} best=static backup=bgp {destination}",
    )


def _wait_route_best_bgp_only_ipv6(rt: TopologyRuntime, *, device: str, destination: str, timeout: int) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv6 {destination} {TARGET_MASK_V6}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for", "Total 1 path(s)"],
        count={"Path [": 1},
        regex=[r"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Preference\s*:\s*200\b"],
        not_regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
        label=f"{device} best=bgp only {destination}",
    )


def _wait_os_best_proto_ipv6(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    gateway: str,
    proto: str,
    timeout: int,
) -> None:
    alt_proto = "bgp" if proto == "static" else "static"
    best_row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(proto)}\s+\d+(?:\s+\S+)?\s*$"
    )
    alt_row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(alt_proto)}\s+\d+(?:\s+\S+)?\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show fib os ipv6",
        timeout=timeout,
        interval=2,
        regex=[best_row_regex],
        not_regex=[alt_row_regex],
        label=f"{device} os-best {prefix} proto={proto}",
    )
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_fib_ipv6_route(
        rt,
        device=device,
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=True,
        nexthop=gateway,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} fib-best {prefix} proto={proto}",
    )


def _cleanup_case_config_ipv4(rt: TopologyRuntime, *, r1_nh: str, r2_nh: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {TARGET_PREFIX_ADDR_V4} {TARGET_MASK_V4} {r1_nh}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {TARGET_PREFIX_ADDR_V4} {TARGET_MASK_V4} {r2_nh}",
            "no bgp",
            "end",
        ],
    )


def _cleanup_case_config_ipv6(rt: TopologyRuntime, *, r1_nh6: str, r2_nh6: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv6 {TARGET_PREFIX_ADDR_V6} {TARGET_MASK_V6} {r1_nh6}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route static ipv6 {TARGET_PREFIX_ADDR_V6} {TARGET_MASK_V6} {r2_nh6}",
            "no bgp",
            "end",
        ],
    )


def _run_ipv4(rt: TopologyRuntime, *, r1_peer_ip: str, r2_peer_ip: str, r2_route_nh: str) -> None:
    step("Cleanup stale config")
    _cleanup_case_config_ipv4(rt, r1_nh=r1_peer_ip, r2_nh=r2_route_nh)

    step("Configure BGP base")
    run_cmds(rt=rt, device="r1", strict=False, commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
    run_cmds(rt=rt, device="r2", strict=False, commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

    step("Configure BGP neighbors and import-route on r2")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "bgp 65001",
            f"neighbor {r1_peer_ip} as 65002",
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
            f"neighbor {r2_peer_ip} as 65001",
            "af ipv4-unicast",
            f"neighbor {r2_peer_ip} enable",
            "import-route static",
            "exit",
            "end",
        ],
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
                "device": "r2",
                "command": "show bgp neighbor af ipv4-unicast",
                "contains": [r2_peer_ip],
                "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2->r1 ipv4-unicast",
            },
        ],
        timeout=30,
    )

    step("Inject BGP route source on r2 (static imported into BGP)")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"route static ipv4 {TARGET_PREFIX_ADDR_V4} {TARGET_MASK_V4} {r2_route_nh}",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show bgp route af ipv4-unicast",
                "contains": [TARGET_PREFIX_V4],
                "label": "r1 learned BGP route",
            }
        ],
        timeout=30,
    )

    step("Configure competing static route on r1 for same prefix")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"route static ipv4 {TARGET_PREFIX_ADDR_V4} {TARGET_MASK_V4} {r1_peer_ip}",
            "end",
        ],
    )

    step("Verify route and OS both prefer static over BGP")
    _wait_route_best_static_backup_bgp_ipv4(rt, device="r1", destination=TARGET_PREFIX_ADDR_V4, timeout=30)
    _wait_os_best_proto_ipv4(rt, device="r1", prefix=TARGET_PREFIX_V4, gateway=r1_peer_ip, proto="static", timeout=30)

    step("Delete r1 static route")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {TARGET_PREFIX_ADDR_V4} {TARGET_MASK_V4} {r1_peer_ip}",
            "end",
        ],
    )

    step("Verify route and OS switch to BGP after static removal")
    _wait_route_best_bgp_only_ipv4(rt, device="r1", destination=TARGET_PREFIX_ADDR_V4, timeout=30)
    _wait_os_best_proto_ipv4(rt, device="r1", prefix=TARGET_PREFIX_V4, gateway=r1_peer_ip, proto="bgp", timeout=30)


def _run_ipv6(rt: TopologyRuntime, *, r1_peer_ip6: str, r2_peer_ip6: str, r2_route_nh6: str) -> None:
    step("Cleanup stale config")
    _cleanup_case_config_ipv6(rt, r1_nh6=r1_peer_ip6, r2_nh6=r2_route_nh6)

    step("Configure BGP base")
    run_cmds(rt=rt, device="r1", strict=False, commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
    run_cmds(rt=rt, device="r2", strict=False, commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

    step("Configure BGP neighbors and import-route on r2")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "bgp 65001",
            f"neighbor {r1_peer_ip6} as 65002",
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
            f"neighbor {r2_peer_ip6} as 65001",
            "af ipv6-unicast",
            f"neighbor {r2_peer_ip6} enable",
            "import-route static",
            "exit",
            "end",
        ],
    )

    step("Wait BGP sessions")
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv6-unicast",
                "contains": [r1_peer_ip6],
                "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1->r2 ipv6-unicast",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv6-unicast",
                "contains": [r2_peer_ip6],
                "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2->r1 ipv6-unicast",
            },
        ],
        timeout=30,
    )

    step("Inject BGP route source on r2 (static imported into BGP)")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"route static ipv6 {TARGET_PREFIX_ADDR_V6} {TARGET_MASK_V6} {r2_route_nh6}",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show bgp route af ipv6-unicast",
                "contains": [TARGET_PREFIX_V6],
                "label": "r1 learned BGP route",
            }
        ],
        timeout=30,
    )

    step("Configure competing static route on r1 for same prefix")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"route static ipv6 {TARGET_PREFIX_ADDR_V6} {TARGET_MASK_V6} {r1_peer_ip6}",
            "end",
        ],
    )

    step("Verify route and OS both prefer static over BGP")
    _wait_route_best_static_backup_bgp_ipv6(rt, device="r1", destination=TARGET_PREFIX_ADDR_V6, timeout=30)
    _wait_os_best_proto_ipv6(rt, device="r1", prefix=TARGET_PREFIX_V6, gateway=r1_peer_ip6, proto="static", timeout=30)

    step("Delete r1 static route")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv6 {TARGET_PREFIX_ADDR_V6} {TARGET_MASK_V6} {r1_peer_ip6}",
            "end",
        ],
    )

    step("Verify route and OS switch to BGP after static removal")
    _wait_route_best_bgp_only_ipv6(rt, device="r1", destination=TARGET_PREFIX_ADDR_V6, timeout=30)
    _wait_os_best_proto_ipv6(rt, device="r1", prefix=TARGET_PREFIX_V6, gateway=r1_peer_ip6, proto="bgp", timeout=30)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_route_nh = str(g_top.r2.GE_1.peer_ip)
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)
    r2_route_nh6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        _run_ipv4(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip, r2_route_nh=r2_route_nh)
        _cleanup_case_config_ipv4(rt, r1_nh=r1_peer_ip, r2_nh=r2_route_nh)
        _run_ipv6(rt, r1_peer_ip6=r1_peer_ip6, r2_peer_ip6=r2_peer_ip6, r2_route_nh6=r2_route_nh6)
        print("BGP static-vs-bgp preference fallback dual-stack check passed.")
    finally:
        _cleanup_case_config_ipv4(rt, r1_nh=r1_peer_ip, r2_nh=r2_route_nh)
        _cleanup_case_config_ipv6(rt, r1_nh6=r1_peer_ip6, r2_nh6=r2_route_nh6)
