#!/usr/bin/env python3
"""
BGP basic check script.

This file is a case script loaded by `scripts/ci/module_runner.py`.
Runner lifecycle:
- load case top.yaml once
- start topology runtime once per case directory
- run all scripts in this case directory
- cleanup runtime once after all scripts
"""

from __future__ import annotations

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def _cleanup_case_config(rt: TopologyRuntime, r1_local_ip: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 10.10.10.0 24 {r1_local_ip}",
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
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    """
    Entry called by module_runner.
    """
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r1_local_ip = str(g_top.r1.GE_1.ip)

    try:
        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

        step("Configure BGP neighbors")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_ip} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "import-route static",
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
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "exit",
                "end",
            ],
        )

        session_checks = [
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv4-unicast",
                "contains": [r1_peer_ip, "Established"],
                "label": "r1->r2 ipv4-unicast",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv4-unicast",
                "contains": [r2_peer_ip, "Established"],
                "label": "r2->r1 ipv4-unicast",
            },
        ]

        step("Wait BGP sessions")
        wait_checks(rt, session_checks, timeout=30)

        step("Apply static route on r1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=["config", f"route ipv4 10.10.10.0 24 {r1_local_ip}", "end"],
        )

        route_checks = [
            {
                "device": "r1",
                "command": "show bgp route af ipv4-unicast",
                "contains": ["10.10.10.0/24"],
                "label": "r1 local route 10.10.10.0/24",
            },
            {
                "device": "r2",
                "command": "show bgp route af ipv4-unicast",
                "contains": ["10.10.10.0/24"],
                "label": "r2 learned route 10.10.10.0/24",
            },
        ]

        step("Wait BGP routes")
        wait_checks(rt, route_checks, timeout=30)

        step("Reboot r1 and wait CLI reconnect")
        reboot_device(rt, "r1", timeout=90)

        step("Wait BGP sessions after reboot")
        wait_checks(rt, session_checks, timeout=30)

        step("Wait BGP routes after reboot")
        wait_checks(rt, route_checks, timeout=30)

        print("BGP basic check passed.")
    finally:
        _cleanup_case_config(rt, r1_local_ip)
