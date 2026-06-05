#!/usr/bin/env python3
"""
BGP route nexthop-id detail check (dual-stack).

Coverage:
- BGP route detail prints NH-ID for learned IPv4/IPv6 routes
- ROUTE route detail prints the same NH-ID
- show route nexthop and show fib nexthop expose the same object id and gateway
- FIB route detail is installed through that nexthop
"""

from __future__ import annotations

import re

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


V4_PREFIX_ADDR = "10.77.10.0"
V4_PREFIX_LEN = 24
V4_PREFIX = f"{V4_PREFIX_ADDR}/{V4_PREFIX_LEN}"

V6_PREFIX_ADDR = "2001:db8:7710::"
V6_PREFIX_LEN = 64
V6_PREFIX = f"{V6_PREFIX_ADDR}/{V6_PREFIX_LEN}"


def _extract_nh_id(output: str, *, command: str) -> str:
    match = re.search(r"(?im)^\s*NH-ID\s*:\s*([1-9][0-9]*)\s*$", output)
    if not match:
        raise RuntimeError(f"failed to extract NH-ID from {command} output:\n{output}")
    return match.group(1)


def _cleanup(rt: TopologyRuntime, *, r1_nh4: str, r1_nh6: str) -> None:
    step("Cleanup BGP/static nexthop-id detail case")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no route static ipv4 {V4_PREFIX_ADDR} {V4_PREFIX_LEN} {r1_nh4}",
            f"no route static ipv6 {V6_PREFIX_ADDR} {V6_PREFIX_LEN} {r1_nh6}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=["end", "config", "no bgp", "end"],
    )


def _wait_bgp_detail_and_extract_id(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix_addr: str,
    prefix_len: int,
    prefix: str,
    nexthop: str,
) -> str:
    command = f"show bgp route af {afi}-unicast {prefix_addr} {prefix_len}"
    wait_check(
        rt,
        device="r2",
        command=command,
        timeout=60,
        interval=2,
        contains=[f"BGP Route Detail: {prefix}", "Valid    : Yes"],
        regex=[
            rf"(?im)^\s*NextHop\s*:\s*{re.escape(nexthop)}\s*$",
            r"(?im)^\s*NH-ID\s*:\s*[1-9][0-9]*\s*$",
        ],
        label=f"r2 BGP {afi} route detail NH-ID for {prefix}",
    )
    return _extract_nh_id(cmd(rt, "r2", command), command=command)


def _assert_route_and_fib_nexthop_id(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix_addr: str,
    prefix_len: int,
    prefix: str,
    nexthop: str,
    nh_id: str,
) -> None:
    wait_check(
        rt,
        device="r2",
        command=f"show route {afi} {prefix_addr} {prefix_len}",
        timeout=40,
        interval=2,
        contains=[f"Routing entry for {prefix}"],
        regex=[
            rf"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?"
            rf"Nexthop\s*:\s*{re.escape(nexthop)}\s*.*?"
            rf"NH-ID\s*:\s*{re.escape(nh_id)}\s*"
        ],
        label=f"r2 ROUTE {afi} detail same BGP NH-ID {nh_id}",
    )
    wait_check(
        rt,
        device="r2",
        command=f"show route nexthop {afi} id {nh_id}",
        timeout=40,
        interval=2,
        regex=[
            rf"(?im)^\s*{re.escape(nh_id)}\s+\d+\s+{re.escape(afi)}\s+bgp\s+"
            rf"{re.escape(nexthop)}\s+{re.escape(nexthop)}\s+\d+\s+\d+\s+\d+\s*$"
        ],
        contains=["Total 1 nexthop(s)"],
        label=f"r2 ROUTE nexthop object id={nh_id} proto=bgp",
    )
    wait_check(
        rt,
        device="r2",
        command=f"show fib nexthop {afi} id {nh_id}",
        timeout=40,
        interval=2,
        regex=[
            rf"(?im)^\s*{re.escape(nh_id)}\s+\d+\s+{re.escape(afi)}\s+ip\s+up\s+"
            rf"{re.escape(nexthop)}\s+\d+\s*$"
        ],
        contains=["Total 1 nexthop(s)"],
        label=f"r2 FIB nexthop object id={nh_id}",
    )
    wait_fib_route(
        rt,
        device="r2",
        afi=afi,
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=True,
        nexthop=nexthop,
        nh_type="ip",
        installed=True,
        skip_os=False,
        timeout=40,
        interval=2,
        label=f"r2 FIB {afi} route {prefix} via nexthop id {nh_id}",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        _cleanup(rt, r1_nh4=r1_peer_ip, r1_nh6=r1_peer_ip6)

        step("Configure BGP dual-stack import-route static")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                f"neighbor {r1_peer_ip} as 65002",
                f"neighbor {r1_peer_ip6} as 65002",
                "af ipv4-unicast",
                "import-route static",
                f"neighbor {r1_peer_ip} enable",
                "exit",
                "af ipv6-unicast",
                "import-route static",
                f"neighbor {r1_peer_ip6} enable",
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
                f"neighbor {r2_peer_ip} as 65001",
                f"neighbor {r2_peer_ip6} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_peer_ip6} enable",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2 IPv4 BGP session established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2 IPv6 BGP session established",
                },
            ],
            timeout=60,
            interval=2,
        )

        step("Inject static IPv4/IPv6 prefixes on r1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv4 {V4_PREFIX_ADDR} {V4_PREFIX_LEN} {r1_peer_ip}",
                f"route static ipv6 {V6_PREFIX_ADDR} {V6_PREFIX_LEN} {r1_peer_ip6}",
                "end",
            ],
        )

        step("Verify BGP/ROUTE/FIB nexthop id consistency for IPv4")
        nh4_id = _wait_bgp_detail_and_extract_id(
            rt,
            afi="ipv4",
            prefix_addr=V4_PREFIX_ADDR,
            prefix_len=V4_PREFIX_LEN,
            prefix=V4_PREFIX,
            nexthop=r2_peer_ip,
        )
        _assert_route_and_fib_nexthop_id(
            rt,
            afi="ipv4",
            prefix_addr=V4_PREFIX_ADDR,
            prefix_len=V4_PREFIX_LEN,
            prefix=V4_PREFIX,
            nexthop=r2_peer_ip,
            nh_id=nh4_id,
        )

        step("Verify BGP/ROUTE/FIB nexthop id consistency for IPv6")
        nh6_id = _wait_bgp_detail_and_extract_id(
            rt,
            afi="ipv6",
            prefix_addr=V6_PREFIX_ADDR,
            prefix_len=V6_PREFIX_LEN,
            prefix=V6_PREFIX,
            nexthop=r2_peer_ip6,
        )
        _assert_route_and_fib_nexthop_id(
            rt,
            afi="ipv6",
            prefix_addr=V6_PREFIX_ADDR,
            prefix_len=V6_PREFIX_LEN,
            prefix=V6_PREFIX,
            nexthop=r2_peer_ip6,
            nh_id=nh6_id,
        )

        print("BGP route nexthop-id detail check passed.")
    finally:
        _cleanup(rt, r1_nh4=r1_peer_ip, r1_nh6=r1_peer_ip6)

