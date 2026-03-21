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

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    if_name = "GE-1"
    cfg_ip = str(g_top.r1.GE_1.ip)
    cfg_prefix = int(g_top.r1.GE_1.prefix)
    net = str(ipaddress.ip_interface(f"{cfg_ip}/{cfg_prefix}").network.network_address)
    net_show = f"show route ipv4 {net}"
    host_show = f"show route ipv4 {cfg_ip}"
    net_pfx = f"{net}/{cfg_prefix}"
    host_pfx = f"{cfg_ip}/32"
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
            {
                "device": "r1",
                "command": net_show,
                "contains": [net_pfx, "Total 1 path(s)"],
                "label": "r1 connected network route",
            },
            {
                "device": "r1",
                "command": host_show,
                "contains": [host_pfx, "Total 1 path(s)"],
                "label": "r1 connected host route",
            },
        ],
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
            {
                "device": "r1",
                "command": net_show,
                "contains": ["(no routes)", "Total 0 path(s)"],
                "label": "r1 network route withdrawn on shutdown",
            },
            {
                "device": "r1",
                "command": host_show,
                "contains": ["(no routes)", "Total 0 path(s)"],
                "label": "r1 host route withdrawn on shutdown",
            },
        ],
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
            {
                "device": "r1",
                "command": net_show,
                "contains": [net_pfx, "Total 1 path(s)"],
                "label": "r1 network route restored on no shutdown",
            },
            {
                "device": "r1",
                "command": host_show,
                "contains": [host_pfx, "Total 1 path(s)"],
                "label": "r1 host route restored on no shutdown",
            },
        ],
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
            {
                "device": "r1",
                "command": net_show,
                "contains": [net_pfx, "Total 1 path(s)"],
                "label": "r1 network route restored after reboot",
            },
            {
                "device": "r1",
                "command": host_show,
                "contains": [host_pfx, "Total 1 path(s)"],
                "label": "r1 host route restored after reboot",
            },
        ],
        timeout=30,
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
            {
                "device": "r1",
                "command": net_show,
                "contains": ["(no routes)", "Total 0 path(s)"],
                "label": "r1 network route withdrawn on no ip",
            },
            {
                "device": "r1",
                "command": host_show,
                "contains": ["(no routes)", "Total 0 path(s)"],
                "label": "r1 host route withdrawn on no ip",
            },
        ],
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
            {
                "device": "r1",
                "command": net_show,
                "contains": [net_pfx, "Total 1 path(s)"],
                "label": "r1 network route restored",
            },
            {
                "device": "r1",
                "command": host_show,
                "contains": [host_pfx, "Total 1 path(s)"],
                "label": "r1 host route restored",
            },
        ],
        timeout=10,
    )

    print("IF operation check passed.")
