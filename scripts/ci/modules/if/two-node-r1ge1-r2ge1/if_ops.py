#!/usr/bin/env python3
"""
IF operation check script.

Operations on r1 GE-1:
1) configure ip address
2) delete ip address
3) shutdown
4) no shutdown

After each operation, verify interface output matches expectation.
"""

from __future__ import annotations

from module_api import g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    if_name = "GE-1"
    cfg_ip = str(g_top.r1.GE_1.ip)
    cfg_prefix = int(g_top.r1.GE_1.prefix)
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
            }
        ],
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
            }
        ],
        timeout=10,
    )

    step("Shutdown r1 GE-1")
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
                    "IP Address : -",
                ],
                "label": "r1 GE-1 shutdown",
            }
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
                    "IP Address : -",
                ],
                "label": "r1 GE-1 no shutdown",
            }
        ],
        timeout=10,
    )

    print("IF operation check passed.")
