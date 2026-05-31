#!/usr/bin/env python3
"""
BGP basic dual-stack check script.

This file is a case script loaded by `scripts/ci/module_runner.py`.
Runner lifecycle:
- load case top.yaml once
- start topology runtime once per case directory
- run all scripts in this case directory
- cleanup runtime once after all scripts
"""

from __future__ import annotations

import re

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TEST_V4_PREFIX = "10.10.10.0"
TEST_V4_PREFIX_LEN = 24
TEST_V6_PREFIX = "2001:db8:1010::"
TEST_V6_PREFIX_LEN = 64


def _cleanup_case_config(rt: TopologyRuntime, r1_peer_ip: str, r1_peer_ip6: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {TEST_V4_PREFIX} {TEST_V4_PREFIX_LEN} {r1_peer_ip}",
            f"no route static ipv6 {TEST_V6_PREFIX} {TEST_V6_PREFIX_LEN} {r1_peer_ip6}",
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
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

        step("Configure BGP dual-stack neighbors")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_ip} as 65002",
                f"neighbor {r1_peer_ip6} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r1_peer_ip6} enable",
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

        session_checks = [
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv4-unicast",
                "contains": [r1_peer_ip],
                "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1->r2 ipv4-unicast",
            },
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv6-unicast",
                "contains": [r1_peer_ip6],
                "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1->r2 ipv6-unicast",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv4-unicast",
                "contains": [r2_peer_ip],
                "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2->r1 ipv4-unicast",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv6-unicast",
                "contains": [r2_peer_ip6],
                "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2->r1 ipv6-unicast",
            },
        ]

        step("Wait BGP sessions")
        wait_checks(rt, session_checks, timeout=40)

        step("Apply static routes on r1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv4 {TEST_V4_PREFIX} {TEST_V4_PREFIX_LEN} {r1_peer_ip}",
                f"route static ipv6 {TEST_V6_PREFIX} {TEST_V6_PREFIX_LEN} {r1_peer_ip6}",
                "end",
            ],
        )

        route_checks = [
            {
                "device": "r1",
                "command": "show bgp route af ipv4-unicast",
                "contains": [f"{TEST_V4_PREFIX}/{TEST_V4_PREFIX_LEN}"],
                "label": "r1 local route 10.10.10.0/24",
            },
            {
                "device": "r1",
                "command": "show bgp route af ipv6-unicast",
                "contains": [f"{TEST_V6_PREFIX}/{TEST_V6_PREFIX_LEN}"],
                "label": "r1 local route 2001:db8:1010::/64",
            },
            {
                "device": "r2",
                "command": "show bgp route af ipv4-unicast",
                "contains": [f"{TEST_V4_PREFIX}/{TEST_V4_PREFIX_LEN}"],
                "label": "r2 learned route 10.10.10.0/24",
            },
            {
                "device": "r2",
                "command": "show bgp route af ipv6-unicast",
                "contains": [f"{TEST_V6_PREFIX}/{TEST_V6_PREFIX_LEN}"],
                "label": "r2 learned route 2001:db8:1010::/64",
            },
        ]

        step("Wait BGP routes")
        wait_checks(rt, route_checks, timeout=40)

        step("Reboot r1 and wait CLI reconnect")
        # reboot 后要等会话重建，配置必须存活：先 save 落盘到 startup-config
        reboot_device(rt, "r1", timeout=90, save_config=True)

        step("Wait BGP sessions after reboot")
        wait_checks(rt, session_checks, timeout=40)

        step("Wait BGP routes after reboot")
        wait_checks(rt, route_checks, timeout=40)

        print("BGP basic dual-stack check passed.")
    finally:
        _cleanup_case_config(rt, r1_peer_ip, r1_peer_ip6)
