#!/usr/bin/env python3
"""
BGP loopback-peer check script.

Goal:
- configure loopback interfaces on r1/r2
- install static host routes so both loopbacks are reachable
- build BGP neighbors using loopback addresses
- verify sessions reach Established
- cleanup BGP/static/loopback config
"""

from __future__ import annotations

import re

from module_api import g_top, hold_check, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


R1_LOOP_ID = 201
R2_LOOP_ID = 202
R1_LOOP_IP = "172.16.201.1"
R2_LOOP_IP = "172.16.202.1"
HOST_MASK = "255.255.255.255"


def _cleanup_case_config(rt: TopologyRuntime, *, r1_peer_ip: str, r2_peer_ip: str) -> None:
    step("Cleanup BGP/loop/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {R2_LOOP_IP} {HOST_MASK} {r1_peer_ip}",
            f"no if loop {R1_LOOP_ID}",
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
            f"no route ipv4 {R1_LOOP_IP} {HOST_MASK} {r2_peer_ip}",
            f"no if loop {R2_LOOP_ID}",
            "no bgp",
            "end",
        ],
    )


def _assert_not_established(rt: TopologyRuntime, *, device: str, peer_ip: str, timeout: int, interval: int = 2) -> None:
    hold_check(
        rt,
        device=device,
        command="show bgp neighbor af ipv4-unicast",
        duration=timeout,
        interval=interval,
        not_regex=[rf"(?im)^.*\b{re.escape(peer_ip)}\b.*\bEstablished\b.*$"],
        label=f"{device} peer {peer_ip} remains non-Established before ebgp-multihop",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    try:
        step("Cleanup stale config")
        _cleanup_case_config(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip)

        step("Configure loopback interfaces")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"if loop {R1_LOOP_ID}",
                f"ip address {R1_LOOP_IP} 32",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"if loop {R2_LOOP_ID}",
                f"ip address {R2_LOOP_IP} 32",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if loop {R1_LOOP_ID}",
                    "contains": [
                        f"Interface loop{R1_LOOP_ID} Detail:",
                        "State      : UP",
                        f"IP Address : {R1_LOOP_IP}/32",
                    ],
                    "label": "r1 loopback configured",
                },
                {
                    "device": "r2",
                    "command": f"show if loop {R2_LOOP_ID}",
                    "contains": [
                        f"Interface loop{R2_LOOP_ID} Detail:",
                        "State      : UP",
                        f"IP Address : {R2_LOOP_IP}/32",
                    ],
                    "label": "r2 loopback configured",
                },
            ],
            timeout=30,
        )

        step("Install static routes to remote loopback")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {R2_LOOP_IP} {HOST_MASK} {r1_peer_ip}",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {R1_LOOP_IP} {HOST_MASK} {r2_peer_ip}",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show route ipv4 static",
                    "contains": [f"{R2_LOOP_IP}/32", r1_peer_ip],
                    "label": "r1 static route to r2 loopback",
                },
                {
                    "device": "r2",
                    "command": "show route ipv4 static",
                    "contains": [f"{R1_LOOP_IP}/32", r2_peer_ip],
                    "label": "r2 static route to r1 loopback",
                },
            ],
            timeout=30,
        )

        step("Configure BGP base")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", "bgp 65001", "router-id 1.1.1.1", "timer connect-retry 5", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", "bgp 65002", "router-id 2.2.2.2", "timer connect-retry 5", "end"],
        )

        step("Configure BGP neighbors over loopback (without ebgp-multihop)")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {R2_LOOP_IP} as 65002",
                f"neighbor {R2_LOOP_IP} source-interface loop{R1_LOOP_ID}",
                "af ipv4-unicast",
                f"neighbor {R2_LOOP_IP} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                f"neighbor {R1_LOOP_IP} as 65001",
                f"neighbor {R1_LOOP_IP} source-interface loop{R2_LOOP_ID}",
                "af ipv4-unicast",
                f"neighbor {R1_LOOP_IP} enable",
                "exit",
                "end",
            ],
        )

        step("Verify sessions do not establish before ebgp-multihop")
        _assert_not_established(rt, device="r1", peer_ip=R2_LOOP_IP, timeout=15)
        _assert_not_established(rt, device="r2", peer_ip=R1_LOOP_IP, timeout=15)

        step("Configure ebgp-multihop")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", "bgp 65001", f"neighbor {R2_LOOP_IP} ebgp-multihop 5", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", "bgp 65002", f"neighbor {R1_LOOP_IP} ebgp-multihop 5", "end"],
        )

        step("Wait BGP sessions over loopback after ebgp-multihop")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [R2_LOOP_IP, "Established"],
                    "label": "r1->r2 loopback peer",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [R1_LOOP_IP, "Established"],
                    "label": "r2->r1 loopback peer",
                },
            ],
            timeout=45,
        )

        step("Clear BGP config")
        run_cmds(rt=rt, device="r1", strict=False, commands=["config", "no bgp", "end"])
        run_cmds(rt=rt, device="r2", strict=False, commands=["config", "no bgp", "end"])

        step("Verify BGP cleared")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": ["BGP Error: BGP not configured."],
                    "label": "r1 bgp cleared",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": ["BGP Error: BGP not configured."],
                    "label": "r2 bgp cleared",
                },
            ],
            timeout=20,
        )

        print("BGP loopback static-peer check passed.")
    finally:
        _cleanup_case_config(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip)
