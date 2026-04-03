#!/usr/bin/env python3
"""
IF operation check script.

Operations on r1 GE-1:
1) configure ip address
2) shutdown (with configured ip)
3) no shutdown
4) reboot
5) delete ip address

After each operation, verify interface + connected routes match expectation.
"""

from __future__ import annotations

import ipaddress

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


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
        label=f"{device} route {route_key} {state}",
    )


def _cleanup(rt: TopologyRuntime, if_name: str, cfg_ip: str, cfg_prefix: int) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"if {if_name}",
            f"ip address {cfg_ip} {cfg_prefix}",
            "no shutdown",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    if_name = "GE-1"
    cfg_ip = str(g_top.r1.GE_1.ip)
    cfg_prefix = int(g_top.r1.GE_1.prefix)
    net = str(ipaddress.ip_interface(f"{cfg_ip}/{cfg_prefix}").network.network_address)
    net_show = f"show route ipv4 {net}"
    host_show = f"show route ipv4 {cfg_ip}"
    show_cmd = f"show if {if_name}"

    try:
        _run_inner(rt, if_name, cfg_ip, cfg_prefix, net, net_show, host_show, show_cmd)
    finally:
        step("Restore baseline interface config on r1 GE-1")
        _cleanup(rt, if_name, cfg_ip, cfg_prefix)

    print("IF operation check passed.")


def _run_inner(
    rt: TopologyRuntime,
    if_name: str,
    cfg_ip: str,
    cfg_prefix: int,
    net: str,
    net_show: str,
    host_show: str,
    show_cmd: str,
) -> None:
    step("Configure IP address on r1 GE-1")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {if_name}",
            f"ip address {cfg_ip} {cfg_prefix}",
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
                    f"IP Address : {cfg_ip}/{cfg_prefix}",
                ],
                "label": "r1 GE-1 configured ip",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_ip,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )

    step("Shutdown r1 GE-1 (with IP configured)")
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
                    f"IP Address : {cfg_ip}/{cfg_prefix}",
                ],
                "label": "r1 GE-1 shutdown",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net,
        expect_present=False,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_ip,
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
                    f"IP Address : {cfg_ip}/{cfg_prefix}",
                ],
                "label": "r1 GE-1 no shutdown",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_ip,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )

    step("Reboot r1 and verify IF/route restore")
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
                    f"IP Address : {cfg_ip}/{cfg_prefix}",
                ],
                "label": "r1 GE-1 restored after reboot",
            },
        ],
        timeout=30,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_ip,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )

    step("Delete IP address on r1 GE-1")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {if_name}",
            f"no ip address {cfg_ip} {cfg_prefix}",
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
                    "IP Address : -",
                ],
                "label": "r1 GE-1 ip removed",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net,
        expect_present=False,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_ip,
        expect_present=False,
        timeout=10,
    )

    step("Verify baseline interface config on r1 GE-1")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"if {if_name}",
            f"ip address {cfg_ip} {cfg_prefix}",
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
                    f"IP Address : {cfg_ip}/{cfg_prefix}",
                ],
                "label": "r1 GE-1 baseline restored",
            },
        ],
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=net_show,
        route_key=net,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )
    _wait_route_state(
        rt,
        device="r1",
        command=host_show,
        route_key=cfg_ip,
        expect_present=True,
        expect_if=if_name,
        timeout=10,
    )
