#!/usr/bin/env python3
"""
ISIS route nexthop-id detail check (dual-stack).

Coverage:
- ISIS route detail prints NH-ID for learned IPv4/IPv6 routes
- ROUTE route detail prints the same NH-ID
- show route nexthop and show fib nexthop expose the same object id and gateway
- FIB route detail is installed through that nexthop
"""

from __future__ import annotations

import ipaddress
import re

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TAG = 130
GE_IF = "GE-1"

R1_NET = "49.0001.0000.0000.0031.00"
R2_NET = "49.0001.0000.0000.0032.00"

R1_LOOP_ID = 131
R2_LOOP_ID = 132

R1_LOOP_V4 = "10.255.131.1"
R1_LOOP_V4_LEN = 32
R2_LOOP_V4 = "10.255.132.2"
R2_LOOP_V4_LEN = 32

R1_LOOP_V6 = "2001:db8:255:131::1"
R1_LOOP_V6_LEN = 128
R2_LOOP_V6 = "2001:db8:255:132::2"
R2_LOOP_V6_LEN = 128

R2_LOOP_V4_PREFIX = f"{R2_LOOP_V4}/{R2_LOOP_V4_LEN}"
R2_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R2_LOOP_V6}/{R2_LOOP_V6_LEN}", strict=False))


def _extract_field(output: str, *, command: str, field: str) -> str:
    match = re.search(rf"(?im)^\s*{re.escape(field)}\s*:\s*(\S+)\s*$", output)
    if not match:
        raise RuntimeError(f"failed to extract {field} from {command} output:\n{output}")
    return match.group(1)


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup ISIS nexthop-id detail case")
    for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=[
                "end",
                "config",
                f"no isis {TAG}",
                f"no if loop {loop_id}",
                f"if {GE_IF}",
                "no shutdown",
                "exit",
                "end",
            ],
        )


def _configure_isis(rt: TopologyRuntime) -> None:
    step("Configure loopbacks and ISIS dual-stack")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if loop {R1_LOOP_ID}",
            f"ip address {R1_LOOP_V4} {R1_LOOP_V4_LEN}",
            f"ipv6 address {R1_LOOP_V6} {R1_LOOP_V6_LEN}",
            "exit",
            f"isis {TAG}",
            f"net {R1_NET}",
            "is-type level-1-2",
            "cost-style wide",
            "af ipv4",
            "af ipv6",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"if loop {R2_LOOP_ID}",
            f"ip address {R2_LOOP_V4} {R2_LOOP_V4_LEN}",
            f"ipv6 address {R2_LOOP_V6} {R2_LOOP_V6_LEN}",
            "exit",
            f"isis {TAG}",
            f"net {R2_NET}",
            "is-type level-1-2",
            "cost-style wide",
            "af ipv4",
            "af ipv6",
            "end",
        ],
    )
    for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=device,
            commands=[
                "config",
                f"if {GE_IF}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                f"if loop {loop_id}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis passive {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )


def _wait_adjacency(rt: TopologyRuntime) -> None:
    r1_peer_ip4 = str(g_top.r1.GE_1.peer_ip)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis neighbor {TAG}",
                "contains": ["ISIS Neighbors", GE_IF, r1_peer_ip4],
                "regex": [
                    rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\s+yes\s+yes\s+\d+\s+\d+\s+"
                    rf"{re.escape(r1_peer_ip4)}\s+fe80:[0-9a-f:]+\s*$"
                ],
                "label": "r1 ISIS neighbor up",
            }
        ],
        timeout=90,
        interval=2,
    )


def _wait_isis_detail_and_extract(
    rt: TopologyRuntime,
    *,
    afi: str,
    destination: str,
    prefix_len: int,
    prefix: str,
    expected_nexthop: str | None = None,
) -> tuple[str, str]:
    command = f"show isis route {afi} {TAG} {destination} {prefix_len}"
    nexthop_regex = (
        rf"(?im)^\s*Nexthop\s*:\s*{re.escape(expected_nexthop)}\s*$"
        if expected_nexthop
        else r"(?im)^\s*Nexthop\s*:\s*\S+\s*$"
    )
    wait_check(
        rt,
        device="r1",
        command=command,
        timeout=90,
        interval=2,
        contains=[f"Prefix       : {prefix}"],
        regex=[nexthop_regex, r"(?im)^\s*NH-ID\s*:\s*[1-9][0-9]*\s*$"],
        label=f"r1 ISIS {afi} route detail NH-ID for {prefix}",
    )
    output = cmd(rt, "r1", command)
    return (
        _extract_field(output, command=command, field="Nexthop"),
        _extract_field(output, command=command, field="NH-ID"),
    )


def _assert_route_and_fib_nexthop_id(
    rt: TopologyRuntime,
    *,
    afi: str,
    destination: str,
    prefix_len: int,
    prefix: str,
    nexthop: str,
    nh_id: str,
) -> None:
    wait_check(
        rt,
        device="r1",
        command=f"show route {afi} {destination} {prefix_len}",
        timeout=60,
        interval=2,
        contains=[f"Routing entry for {prefix}"],
        regex=[
            rf"(?is)Path\s*\[1\]\s*:\s*isis\b.*?"
            rf"Nexthop\s*:\s*{re.escape(nexthop)}\s*.*?"
            rf"NH-ID\s*:\s*{re.escape(nh_id)}\s*"
        ],
        label=f"r1 ROUTE {afi} detail same ISIS NH-ID {nh_id}",
    )
    wait_check(
        rt,
        device="r1",
        command=f"show route nexthop {afi} id {nh_id}",
        timeout=60,
        interval=2,
        regex=[
            rf"(?im)^\s*{re.escape(nh_id)}\s+\d+\s+{re.escape(afi)}\s+isis\s+"
            rf"{re.escape(nexthop)}\s+{re.escape(nexthop)}\s+\d+\s+\d+\s+\d+\s*$"
        ],
        contains=["Total 1 nexthop(s)"],
        label=f"r1 ROUTE nexthop object id={nh_id} proto=isis",
    )
    wait_check(
        rt,
        device="r1",
        command=f"show fib nexthop {afi} id {nh_id}",
        timeout=60,
        interval=2,
        regex=[
            rf"(?im)^\s*{re.escape(nh_id)}\s+\d+\s+{re.escape(afi)}\s+ip\s+up\s+"
            rf"{re.escape(nexthop)}\s+\d+\s*$"
        ],
        contains=["Total 1 nexthop(s)"],
        label=f"r1 FIB nexthop object id={nh_id}",
    )
    wait_fib_route(
        rt,
        device="r1",
        afi=afi,
        prefix_addr=destination,
        prefix_len=prefix_len,
        expect_present=True,
        nexthop=nexthop,
        nh_type="ip",
        installed=True,
        skip_os=False,
        timeout=60,
        interval=2,
        label=f"r1 FIB {afi} route {prefix} via nexthop id {nh_id}",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip4 = str(g_top.r1.GE_1.peer_ip)

    try:
        _cleanup(rt)
        _configure_isis(rt)
        _wait_adjacency(rt)

        step("Verify ISIS/ROUTE/FIB nexthop id consistency for IPv4")
        nh4, nh4_id = _wait_isis_detail_and_extract(
            rt,
            afi="ipv4",
            destination=R2_LOOP_V4,
            prefix_len=R2_LOOP_V4_LEN,
            prefix=R2_LOOP_V4_PREFIX,
            expected_nexthop=r1_peer_ip4,
        )
        _assert_route_and_fib_nexthop_id(
            rt,
            afi="ipv4",
            destination=R2_LOOP_V4,
            prefix_len=R2_LOOP_V4_LEN,
            prefix=R2_LOOP_V4_PREFIX,
            nexthop=nh4,
            nh_id=nh4_id,
        )

        step("Verify ISIS/ROUTE/FIB nexthop id consistency for IPv6")
        nh6, nh6_id = _wait_isis_detail_and_extract(
            rt,
            afi="ipv6",
            destination=R2_LOOP_V6,
            prefix_len=R2_LOOP_V6_LEN,
            prefix=R2_LOOP_V6_PREFIX,
        )
        _assert_route_and_fib_nexthop_id(
            rt,
            afi="ipv6",
            destination=R2_LOOP_V6,
            prefix_len=R2_LOOP_V6_LEN,
            prefix=R2_LOOP_V6_PREFIX,
            nexthop=nh6,
            nh_id=nh6_id,
        )

        print("ISIS route nexthop-id detail check passed.")
    finally:
        _cleanup(rt)

