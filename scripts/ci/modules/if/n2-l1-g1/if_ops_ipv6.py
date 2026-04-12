#!/usr/bin/env python3
"""
IF IPv6 operation check script.

Operations on r1 GE-1:
1) configure ipv6 address
2) shutdown (with configured ipv6)
3) no shutdown
4) reboot
5) delete ipv6 address

After each operation, verify interface + connected IPv6 routes match expectation.
"""

from __future__ import annotations

import ipaddress

from module_api import reboot_device, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


CFG_V6 = "2001:db8:12::1"
CFG_V6_PREFIX = 64


def _wait_route_state(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    route_key: str,
    expect_present: bool,
    expect_if: str | None = None,
    timeout: int = 10,
    interval: int = 2,
) -> None:
    contains: list[str] = []
    regex: list[str] = []
    not_contains: list[str] = []

    if expect_present:
        contains.extend([f"Routing entry for {route_key}", "Total 1 path(s)"])
        regex.append(r"(?im)^\s*Path\s*\[1\]\s*:\s*(?:connected|local)\b")
        not_contains.extend(["(no routes)", "(no matching routes)"])
        if expect_if is not None:
            contains.append(f"Interface : {expect_if}")
    else:
        not_contains.extend(["Path [1]: connected", "Path [1]: local"])
        regex.append(r"(?im)\((?:no routes|no matching routes)\)")

    state = "present" if expect_present else "absent"
    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        contains=contains,
        not_contains=not_contains,
        regex=regex,
        label=f"{device} ipv6-route {route_key} {state}",
    )


def _cleanup(rt: TopologyRuntime, if_name: str, cfg_v6: str, cfg_v6_prefix: int) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"if {if_name}",
            f"no ipv6 address {cfg_v6} {cfg_v6_prefix}",
            "no shutdown",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    if_name = "GE-1"
    cfg_v6 = str(ipaddress.ip_address(CFG_V6))
    cfg_v6_prefix = CFG_V6_PREFIX
    net_v6 = str(ipaddress.ip_interface(f"{cfg_v6}/{cfg_v6_prefix}").network.network_address)

    net_show = f"show route ipv6 {net_v6} {cfg_v6_prefix}"
    host_show = f"show route ipv6 {cfg_v6} 128"
    show_cmd = f"show if {if_name}"

    try:
        _run_inner(rt, if_name, cfg_v6, cfg_v6_prefix, net_v6, net_show, host_show, show_cmd)
    finally:
        step("Restore baseline interface config on r1 GE-1 (remove test IPv6)")
        _cleanup(rt, if_name, cfg_v6, cfg_v6_prefix)

    print("IF IPv6 operation check passed.")


def _run_inner(
    rt: TopologyRuntime,
    if_name: str,
    cfg_v6: str,
    cfg_v6_prefix: int,
    net_v6: str,
    net_show: str,
    host_show: str,
    show_cmd: str,
) -> None:
    cfg_v6_show = f"{cfg_v6}/{cfg_v6_prefix}"

    step("Configure IPv6 address on r1 GE-1")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {if_name}",
            f"ipv6 address {cfg_v6} {cfg_v6_prefix}",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": show_cmd,
                "contains": [
                    f"Interface {if_name} Detail:",
                    "State      : UP",
                    f"IPv6 Addr  : {cfg_v6_show}",
                ],
                "label": "r1 GE-1 configured ipv6",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net_v6,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_v6,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )

    step("Shutdown r1 GE-1 (with IPv6 configured)")
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
                "command": show_cmd,
                "contains": [
                    f"Interface {if_name} Detail:",
                    "State      : DOWN",
                    f"IPv6 Addr  : {cfg_v6_show}",
                ],
                "label": "r1 GE-1 shutdown with ipv6",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net_v6,
        expect_present=False,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_v6,
        expect_present=False,
        timeout=10,
    )

    step("No shutdown r1 GE-1")
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
                "command": show_cmd,
                "contains": [
                    f"Interface {if_name} Detail:",
                    "State      : UP",
                    f"IPv6 Addr  : {cfg_v6_show}",
                ],
                "label": "r1 GE-1 no shutdown with ipv6",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net_v6,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_v6,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )

    step("Reboot r1 and verify IF/IPv6 route restore")
    reboot_device(rt, "r1", timeout=120)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": show_cmd,
                "contains": [
                    f"Interface {if_name} Detail:",
                    "State      : UP",
                    f"IPv6 Addr  : {cfg_v6_show}",
                ],
                "label": "r1 GE-1 ipv6 restored after reboot",
            },
        ],
        timeout=30,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net_v6,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_v6,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )

    step("Delete IPv6 address on r1 GE-1")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {if_name}",
            f"no ipv6 address {cfg_v6} {cfg_v6_prefix}",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": show_cmd,
                "contains": [
                    f"Interface {if_name} Detail:",
                    "State      : UP",
                    "IPv6 Addr  : -",
                ],
                "label": "r1 GE-1 ipv6 removed",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net_v6,
        expect_present=False,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_v6,
        expect_present=False,
        timeout=10,
    )
