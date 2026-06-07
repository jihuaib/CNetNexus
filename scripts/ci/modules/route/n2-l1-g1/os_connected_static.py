#!/usr/bin/env python3
"""
Route OS downlink check (connected + static, dual-stack).

Goals:
- verify connected IPv4/IPv6 routes are installed to OS route table
- verify connected routes are withdrawn/restored on interface shutdown/no shutdown
- verify static IPv4/IPv6 route is installed/withdrawn in OS route table
"""

from __future__ import annotations

import ipaddress
import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_ipv4_route, wait_fib_ipv6_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


STATIC_PREFIX_ADDR = "198.18.66.0"
STATIC_MASK = "24"
STATIC_PREFIX = "198.18.66.0/24"
LOOP_ID = 101
LOOP_IF = f"loop{LOOP_ID}"
LOOP_IP = "198.18.101.1"
LOOP_PREFIX_LEN = 32
LOOP_PREFIX = f"{LOOP_IP}/32"

GE_IF = "GE-1"
V6_STATIC_PREFIX_ADDR = "2001:db8:198:18:66::"
V6_STATIC_PREFIX_LEN = 64
V6_STATIC_PREFIX = (
    f"{ipaddress.ip_interface(f'{V6_STATIC_PREFIX_ADDR}/{V6_STATIC_PREFIX_LEN}').network.network_address}/"
    f"{V6_STATIC_PREFIX_LEN}"
)
V6_LOOP_IP = "2001:db8:101::1"
V6_LOOP_PREFIX_LEN = 128
V6_LOOP_PREFIX = f"{V6_LOOP_IP}/{V6_LOOP_PREFIX_LEN}"


def _wait_os_route(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    table: str,
    route_type: str,
    proto: str,
    gateway: str,
    expect_present: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    row_regex = (
        rf"(?im)^\s*{re.escape(table)}\s+{re.escape(route_type)}\s+"
        rf"{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(proto)}\s+\d+(?:\s+\S+)?\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show fib os ipv4",
        timeout=timeout,
        interval=interval,
        regex=[row_regex] if expect_present else (),
        not_regex=[row_regex] if not expect_present else (),
        label=(
            f"{device} os route {prefix} "
            f"{'present' if expect_present else 'absent'} "
            f"(table={table} type={route_type} proto={proto} gw={gateway})"
        ),
    )
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_fib_ipv4_route(
        rt,
        device=device,
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=expect_present,
        nexthop=gateway if gateway != "-" else None,
        installed=True if proto == "static" else None,
        skip_os=False if proto == "static" else True,
        timeout=timeout,
        interval=interval,
        label=(
            f"{device} fib route {prefix} "
            f"{'present' if expect_present else 'absent'} "
            f"(proto={proto} gw={gateway})"
        ),
    )


def _wait_os_route_ipv6(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    table: str,
    route_type: str,
    proto: str,
    gateway: str,
    expect_present: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    row_regex = (
        rf"(?im)^\s*{re.escape(table)}\s+{re.escape(route_type)}\s+"
        rf"{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(proto)}\s+\d+(?:\s+\S+)?\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show fib os ipv6",
        timeout=timeout,
        interval=interval,
        regex=[row_regex] if expect_present else (),
        not_regex=[row_regex] if not expect_present else (),
        label=(
            f"{device} os route {prefix} "
            f"{'present' if expect_present else 'absent'} "
            f"(table={table} type={route_type} proto={proto} gw={gateway})"
        ),
    )
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_fib_ipv6_route(
        rt,
        device=device,
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=expect_present,
        nexthop=gateway if gateway != "-" else None,
        installed=True if proto == "static" else None,
        skip_os=False if proto == "static" else True,
        timeout=timeout,
        interval=interval,
        label=(
            f"{device} fib route {prefix} "
            f"{'present' if expect_present else 'absent'} "
            f"(proto={proto} gw={gateway})"
        ),
    )


def _cleanup_ipv6(rt: TopologyRuntime, *, static_nh: str) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            f"no route static ipv6 {V6_STATIC_PREFIX_ADDR} {V6_STATIC_PREFIX_LEN} {static_nh}",
            f"no if loop {LOOP_ID}",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    _run_ipv4(rt, top)
    _run_ipv6(rt, top)
    print("Route OS connected/static downlink dual-stack check passed.")


def _run_ipv4(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    if_name = "GE-1"
    if_ip = str(g_top.r1.GE_1.ip)
    if_prefix = int(g_top.r1.GE_1.prefix)
    net_addr = str(ipaddress.ip_interface(f"{if_ip}/{if_prefix}").network.network_address)
    connected_net_prefix = f"{net_addr}/{if_prefix}"
    connected_host_prefix = f"{if_ip}/32"
    static_nh = str(g_top.r1.GE_1.peer_ip)

    try:
        step("Cleanup stale static/loop config and force interface up")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"if {if_name}",
                "no shutdown",
                "exit",
                f"no route static ipv4 {STATIC_PREFIX_ADDR} {STATIC_MASK} {static_nh}",
                f"no if loop {LOOP_ID}",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {if_name}",
                    "contains": [f"Interface {if_name} Detail:", "State      : UP"],
                    "label": "r1 GE-1 up before OS check",
                }
            ],
            timeout=20,
            interval=2,
        )

        step("Verify connected routes installed in OS")
        _wait_os_route(
            rt,
            device="r1",
            prefix=connected_net_prefix,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=connected_host_prefix,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )

        step("Shutdown interface and verify connected routes withdrawn from OS")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {if_name}",
                "shutdown",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {if_name}",
                    "contains": [f"Interface {if_name} Detail:", "State      : DOWN"],
                    "label": "r1 GE-1 shutdown",
                }
            ],
            timeout=20,
            interval=2,
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=connected_net_prefix,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=False,
            timeout=30,
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=connected_host_prefix,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=False,
            timeout=30,
        )

        step("No shutdown interface and verify connected routes restored to OS")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {if_name}",
                "no shutdown",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {if_name}",
                    "contains": [f"Interface {if_name} Detail:", "State      : UP"],
                    "label": "r1 GE-1 no shutdown",
                }
            ],
            timeout=20,
            interval=2,
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=connected_net_prefix,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=connected_host_prefix,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )

        step("Create loop interface and verify loop connected route in OS")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"ip address {LOOP_IP} {LOOP_PREFIX_LEN}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if loop {LOOP_ID}",
                    "contains": [
                        f"Interface {LOOP_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr : {LOOP_PREFIX}",
                    ],
                    "label": f"r1 {LOOP_IF} configured",
                }
            ],
            timeout=20,
            interval=2,
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=LOOP_PREFIX,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )

        step("Delete loop IP and verify loop connected route withdrawn from OS")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"no ip address {LOOP_IP} {LOOP_PREFIX_LEN}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if loop {LOOP_ID}",
                    "contains": [
                        f"Interface {LOOP_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr : -",
                    ],
                    "label": f"r1 {LOOP_IF} ip removed",
                }
            ],
            timeout=20,
            interval=2,
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=LOOP_PREFIX,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=False,
            timeout=30,
        )

        step("Delete loop interface")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"no if loop {LOOP_ID}",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if loop {LOOP_ID}",
                    "contains": [f"Error: Interface {LOOP_IF} not found"],
                    "label": f"r1 {LOOP_IF} deleted",
                }
            ],
            timeout=20,
            interval=2,
        )

        step("Add static route and verify OS install")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv4 {STATIC_PREFIX_ADDR} {STATIC_MASK} {static_nh}",
                "end",
            ],
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=STATIC_PREFIX,
            table="main",
            route_type="unicast",
            proto="static",
            gateway=static_nh,
            expect_present=True,
            timeout=30,
        )

        step("Delete static route and verify OS withdrawal")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"no route static ipv4 {STATIC_PREFIX_ADDR} {STATIC_MASK} {static_nh}",
                "end",
            ],
        )
        _wait_os_route(
            rt,
            device="r1",
            prefix=STATIC_PREFIX,
            table="main",
            route_type="unicast",
            proto="static",
            gateway=static_nh,
            expect_present=False,
            timeout=30,
        )

    finally:
        step("Cleanup route OS downlink case config")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"if {if_name}",
                "no shutdown",
                "exit",
                f"no route static ipv4 {STATIC_PREFIX_ADDR} {STATIC_MASK} {static_nh}",
                f"no if loop {LOOP_ID}",
                "end",
            ],
        )


def _run_ipv6(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    if_v6 = str(ipaddress.ip_address(str(g_top.r1.GE_1.ip6)))
    if_prefix = int(g_top.r1.GE_1.prefix6)
    if_show_prefix = f"{if_v6}/{if_prefix}"
    net_addr = str(ipaddress.ip_interface(f"{if_v6}/{if_prefix}").network.network_address)
    connected_net_prefix = f"{net_addr}/{if_prefix}"
    connected_host_prefix = f"{if_v6}/128"
    static_nh = str(g_top.r1.GE_1.peer_ip6)

    try:
        step("Cleanup stale static/loop/ipv6 config")
        _cleanup_ipv6(rt, static_nh=static_nh)

        step("Verify GE-1 IPv6 underlay from top")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv6 Addr  : {if_show_prefix}",
                    ],
                    "label": "r1 GE-1 ipv6 up before OS check",
                }
            ],
            timeout=20,
            interval=2,
        )

        step("Verify connected IPv6 routes installed in OS")
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=connected_net_prefix,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=connected_host_prefix,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )

        step("Shutdown interface and verify connected IPv6 routes withdrawn from OS")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {GE_IF}",
                "shutdown",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [f"Interface {GE_IF} Detail:", "State      : DOWN"],
                    "label": "r1 GE-1 shutdown",
                }
            ],
            timeout=20,
            interval=2,
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=connected_net_prefix,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=False,
            timeout=30,
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=connected_host_prefix,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=False,
            timeout=30,
        )

        step("No shutdown interface and verify connected IPv6 routes restored to OS")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {GE_IF}",
                "no shutdown",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [f"Interface {GE_IF} Detail:", "State      : UP"],
                    "label": "r1 GE-1 no shutdown",
                }
            ],
            timeout=20,
            interval=2,
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=connected_net_prefix,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=connected_host_prefix,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )

        step("Create loop interface and verify loop connected IPv6 route in OS")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"ipv6 address {V6_LOOP_IP} {V6_LOOP_PREFIX_LEN}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if loop {LOOP_ID}",
                    "contains": [
                        f"Interface {LOOP_IF} Detail:",
                        "State      : UP",
                        f"IPv6 Addr  : {V6_LOOP_PREFIX}",
                    ],
                    "label": f"r1 {LOOP_IF} configured",
                }
            ],
            timeout=20,
            interval=2,
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=V6_LOOP_PREFIX,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=30,
        )

        step("Delete loop IPv6 and verify loop connected route withdrawn from OS")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"no ipv6 address {V6_LOOP_IP} {V6_LOOP_PREFIX_LEN}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if loop {LOOP_ID}",
                    "contains": [
                        f"Interface {LOOP_IF} Detail:",
                        "State      : UP",
                        "IPv6 Addr  : -",
                    ],
                    "label": f"r1 {LOOP_IF} ipv6 removed",
                }
            ],
            timeout=20,
            interval=2,
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=V6_LOOP_PREFIX,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=False,
            timeout=30,
        )

        step("Delete loop interface")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"no if loop {LOOP_ID}",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if loop {LOOP_ID}",
                    "contains": [f"Error: Interface {LOOP_IF} not found"],
                    "label": f"r1 {LOOP_IF} deleted",
                }
            ],
            timeout=20,
            interval=2,
        )

        step("Add IPv6 static route and verify OS install")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv6 {V6_STATIC_PREFIX_ADDR} {V6_STATIC_PREFIX_LEN} {static_nh}",
                "end",
            ],
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=V6_STATIC_PREFIX,
            table="main",
            route_type="unicast",
            proto="static",
            gateway=static_nh,
            expect_present=True,
            timeout=30,
        )

        step("Delete IPv6 static route and verify OS withdrawal")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"no route static ipv6 {V6_STATIC_PREFIX_ADDR} {V6_STATIC_PREFIX_LEN} {static_nh}",
                "end",
            ],
        )
        _wait_os_route_ipv6(
            rt,
            device="r1",
            prefix=V6_STATIC_PREFIX,
            table="main",
            route_type="unicast",
            proto="static",
            gateway=static_nh,
            expect_present=False,
            timeout=30,
        )
    finally:
        step("Cleanup route OS downlink case config")
        _cleanup_ipv6(rt, static_nh=static_nh)
