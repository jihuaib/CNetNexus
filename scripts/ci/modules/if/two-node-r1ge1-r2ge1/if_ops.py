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
import time

from module_api import cmd, g_top, reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
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
    deadline = time.time() + timeout
    last_out = ""
    while time.time() < deadline:
        out = cmd(rt, device, command, strict=False)
        last_out = out

        has_entry = f"Routing entry for {route_key}" in out
        has_connected = "Path [1]: connected" in out
        is_absent = "(no routes)" in out or "(no matching routes)" in out

        if expect_present:
            has_if = expect_if is None or f"Interface : {expect_if}" in out
            if has_entry and has_connected and has_if and "Total 1 path(s)" in out and not is_absent:
                return
        else:
            if has_entry and is_absent and not has_connected:
                return

        time.sleep(interval)

    state = "present" if expect_present else "absent"
    raise RuntimeError(
        f"{device} route state mismatch for '{route_key}' after {timeout}s (expect {state})\n"
        f"command: {command}\n"
        f"last output:\n{last_out}"
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
        expect_present=False,
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
        expect_present=False,
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
        expect_present=False,
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

    step("Restore baseline interface config on r1 GE-1")
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
        expect_present=False,
        timeout=10,
    )

    print("IF operation check passed.")
