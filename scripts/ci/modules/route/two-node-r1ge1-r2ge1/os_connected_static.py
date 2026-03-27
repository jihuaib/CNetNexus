#!/usr/bin/env python3
"""
Route OS downlink check (connected + static, IPv4).

Goals:
- verify connected routes are installed to OS route table
- verify connected routes are withdrawn/restored on interface shutdown/no shutdown
- verify static route is installed/withdrawn in OS route table
"""

from __future__ import annotations

import ipaddress
import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


STATIC_PREFIX_ADDR = "198.18.66.0"
STATIC_MASK = "255.255.255.0"
STATIC_PREFIX = "198.18.66.0/24"
LOOP_ID = 101
LOOP_IF = f"loop{LOOP_ID}"
LOOP_IP = "198.18.101.1"
LOOP_PREFIX_LEN = 32
LOOP_PREFIX = f"{LOOP_IP}/32"


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
        rf"{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(proto)}\s+\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show route ipv4 os",
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


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
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
                f"no route ipv4 {STATIC_PREFIX_ADDR} {STATIC_MASK} {static_nh}",
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
                        f"IP Address : {LOOP_PREFIX}",
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
                        "IP Address : -",
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
                f"route ipv4 {STATIC_PREFIX_ADDR} {STATIC_MASK} {static_nh}",
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
                f"no route ipv4 {STATIC_PREFIX_ADDR} {STATIC_MASK} {static_nh}",
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

        print("Route OS connected/static downlink check passed.")
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
                f"no route ipv4 {STATIC_PREFIX_ADDR} {STATIC_MASK} {static_nh}",
                f"no if loop {LOOP_ID}",
                "end",
            ],
        )
