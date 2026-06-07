#!/usr/bin/env python3
"""
BGP loopback-peer dual-stack check script.

Goal:
- configure dual-stack loopback interfaces on r1/r2
- install static host routes so both loopbacks are reachable over IPv4/IPv6
- build BGP neighbors using loopback addresses for IPv4/IPv6 AFs
- verify sessions reach Established only after ebgp-multihop
- cleanup BGP/static/loopback config
"""

from __future__ import annotations

import re

from module_api import g_top, hold_check, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


R1_LOOP_ID = 201
R2_LOOP_ID = 202
R1_LOOP_V4 = "172.16.201.1"
R2_LOOP_V4 = "172.16.202.1"
HOST_MASK_V4 = "32"
R1_LOOP_V6 = "2001:db8:201::1"
R2_LOOP_V6 = "2001:db8:202::1"
HOST_MASK_V6 = "128"


def _cleanup_case_config(
    rt: TopologyRuntime,
    *,
    r1_peer_ip: str,
    r1_peer_ip6: str,
    r2_peer_ip: str,
    r2_peer_ip6: str,
) -> None:
    step("Cleanup BGP/loop/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {R2_LOOP_V4} {HOST_MASK_V4} {r1_peer_ip}",
            f"no route static ipv6 {R2_LOOP_V6} {HOST_MASK_V6} {r1_peer_ip6}",
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
            f"no route static ipv4 {R1_LOOP_V4} {HOST_MASK_V4} {r2_peer_ip}",
            f"no route static ipv6 {R1_LOOP_V6} {HOST_MASK_V6} {r2_peer_ip6}",
            f"no if loop {R2_LOOP_ID}",
            "no bgp",
            "end",
        ],
    )


def _assert_not_established(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    peer_ip: str,
    timeout: int,
    interval: int = 2,
) -> None:
    hold_check(
        rt,
        device=device,
        command=f"show bgp neighbor af {afi}",
        duration=timeout,
        interval=interval,
        not_regex=[rf"(?im)^.*\b{re.escape(peer_ip)}\b.*\bEstablished\b.*$"],
        label=f"{device} peer {peer_ip} remains non-Established before ebgp-multihop",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        step("Cleanup stale config")
        _cleanup_case_config(
            rt,
            r1_peer_ip=r1_peer_ip,
            r1_peer_ip6=r1_peer_ip6,
            r2_peer_ip=r2_peer_ip,
            r2_peer_ip6=r2_peer_ip6,
        )

        step("Configure dual-stack loopback interfaces")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"if loop {R1_LOOP_ID}",
                f"ip address {R1_LOOP_V4} {HOST_MASK_V4}",
                f"ipv6 address {R1_LOOP_V6} {HOST_MASK_V6}",
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
                f"ip address {R2_LOOP_V4} {HOST_MASK_V4}",
                f"ipv6 address {R2_LOOP_V6} {HOST_MASK_V6}",
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
                        f"IPv4 Addr  : {R1_LOOP_V4}/{HOST_MASK_V4}",
                        f"IPv6 Addr  : {R1_LOOP_V6}/{HOST_MASK_V6}",
                    ],
                    "label": "r1 dual-stack loopback configured",
                },
                {
                    "device": "r2",
                    "command": f"show if loop {R2_LOOP_ID}",
                    "contains": [
                        f"Interface loop{R2_LOOP_ID} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {R2_LOOP_V4}/{HOST_MASK_V4}",
                        f"IPv6 Addr  : {R2_LOOP_V6}/{HOST_MASK_V6}",
                    ],
                    "label": "r2 dual-stack loopback configured",
                },
            ],
            timeout=30,
        )

        step("Install static routes to remote loopbacks")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"route static ipv4 {R2_LOOP_V4} {HOST_MASK_V4} {r1_peer_ip}",
                f"route static ipv6 {R2_LOOP_V6} {HOST_MASK_V6} {r1_peer_ip6}",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route static ipv4 {R1_LOOP_V4} {HOST_MASK_V4} {r2_peer_ip}",
                f"route static ipv6 {R1_LOOP_V6} {HOST_MASK_V6} {r2_peer_ip6}",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show route static ipv4",
                    "contains": [f"{R2_LOOP_V4}/{HOST_MASK_V4}", r1_peer_ip],
                    "label": "r1 static IPv4 route to r2 loopback",
                },
                {
                    "device": "r1",
                    "command": "show route static ipv6",
                    "contains": [f"{R2_LOOP_V6}/{HOST_MASK_V6}", r1_peer_ip6],
                    "label": "r1 static IPv6 route to r2 loopback",
                },
                {
                    "device": "r2",
                    "command": "show route static ipv4",
                    "contains": [f"{R1_LOOP_V4}/{HOST_MASK_V4}", r2_peer_ip],
                    "label": "r2 static IPv4 route to r1 loopback",
                },
                {
                    "device": "r2",
                    "command": "show route static ipv6",
                    "contains": [f"{R1_LOOP_V6}/{HOST_MASK_V6}", r2_peer_ip6],
                    "label": "r2 static IPv6 route to r1 loopback",
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
                f"neighbor {R2_LOOP_V4} as 65002",
                f"neighbor {R2_LOOP_V4} source-interface loop{R1_LOOP_ID}",
                f"neighbor {R2_LOOP_V6} as 65002",
                f"neighbor {R2_LOOP_V6} source-interface loop{R1_LOOP_ID}",
                "af ipv4-unicast",
                f"neighbor {R2_LOOP_V4} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {R2_LOOP_V6} enable",
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
                f"neighbor {R1_LOOP_V4} as 65001",
                f"neighbor {R1_LOOP_V4} source-interface loop{R2_LOOP_ID}",
                f"neighbor {R1_LOOP_V6} as 65001",
                f"neighbor {R1_LOOP_V6} source-interface loop{R2_LOOP_ID}",
                "af ipv4-unicast",
                f"neighbor {R1_LOOP_V4} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {R1_LOOP_V6} enable",
                "exit",
                "end",
            ],
        )

        step("Verify sessions do not establish before ebgp-multihop")
        _assert_not_established(rt, device="r1", afi="ipv4-unicast", peer_ip=R2_LOOP_V4, timeout=15)
        _assert_not_established(rt, device="r2", afi="ipv4-unicast", peer_ip=R1_LOOP_V4, timeout=15)
        _assert_not_established(rt, device="r1", afi="ipv6-unicast", peer_ip=R2_LOOP_V6, timeout=15)
        _assert_not_established(rt, device="r2", afi="ipv6-unicast", peer_ip=R1_LOOP_V6, timeout=15)

        step("Configure ebgp-multihop")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {R2_LOOP_V4} ebgp-multihop 5",
                f"neighbor {R2_LOOP_V6} ebgp-multihop 5",
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
                f"neighbor {R1_LOOP_V4} ebgp-multihop 5",
                f"neighbor {R1_LOOP_V6} ebgp-multihop 5",
                "end",
            ],
        )

        step("Wait BGP dual-stack sessions over loopback after ebgp-multihop")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [R2_LOOP_V4],
                    "regex": [rf"(?im)^\s*{re.escape(R2_LOOP_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 IPv4 loopback peer",
                },
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "contains": [R2_LOOP_V6],
                    "regex": [rf"(?im)^\s*{re.escape(R2_LOOP_V6)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 IPv6 loopback peer",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [R1_LOOP_V4],
                    "regex": [rf"(?im)^\s*{re.escape(R1_LOOP_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2->r1 IPv4 loopback peer",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "contains": [R1_LOOP_V6],
                    "regex": [rf"(?im)^\s*{re.escape(R1_LOOP_V6)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2->r1 IPv6 loopback peer",
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
                    "contains": ["target module is not running"],
                    "label": "r1 bgp IPv4 cleared",
                },
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "contains": ["target module is not running"],
                    "label": "r1 bgp IPv6 cleared",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": ["target module is not running"],
                    "label": "r2 bgp IPv4 cleared",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "contains": ["target module is not running"],
                    "label": "r2 bgp IPv6 cleared",
                },
            ],
            timeout=20,
        )

        print("BGP loopback static-peer dual-stack check passed.")
    finally:
        _cleanup_case_config(
            rt,
            r1_peer_ip=r1_peer_ip,
            r1_peer_ip6=r1_peer_ip6,
            r2_peer_ip=r2_peer_ip,
            r2_peer_ip6=r2_peer_ip6,
        )
