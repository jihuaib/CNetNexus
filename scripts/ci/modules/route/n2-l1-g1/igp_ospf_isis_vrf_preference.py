#!/usr/bin/env python3
"""Verify OSPF-to-IS-IS route fallback and recovery inside a VRF.

Both protocols advertise the same remote loopback prefix. OSPF is expected to
win with preference 110 while IS-IS remains as preference-115 backup. Removing
OSPF from the participating interfaces must promote IS-IS without losing
traffic; re-enabling OSPF must restore the original best path.
"""

from __future__ import annotations

import re

from module_api import require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF = "blue"
GE_IF = "GE-1"
OSPF_PROCESS = 440
OSPF_AREA = 0
ISIS_TAG = 450

R1_GE, R2_GE = "10.44.0.1", "10.44.0.2"
R1_LOOP_ID, R2_LOOP_ID = 171, 172
R1_LOOP, R2_LOOP = "10.244.1.1", "10.244.2.2"
R1_NET = "49.0001.0000.0000.0451.00"
R2_NET = "49.0001.0000.0000.0452.00"

PING_SUCCESS_RE = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
ERROR_RE = r"(?im)(?:unknown command|invalid input|module timed out|failed to start module|error:\s)"


def _cleanup(rt: TopologyRuntime) -> None:
    for dev, loop_id, public_ge4, public_ge6 in (
        ("r1", R1_LOOP_ID, "10.12.0.1", "2001:db8:12::1"),
        ("r2", R2_LOOP_ID, "10.12.0.2", "2001:db8:12::2"),
    ):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"no ospf {OSPF_PROCESS}",
                f"no isis {ISIS_TAG}",
                f"no if loop {loop_id}",
                f"if {GE_IF}",
                "no vrf forwarding",
                f"ip address {public_ge4} 30",
                f"ipv6 address {public_ge6} 64",
                "no shutdown",
                "exit",
                f"no vrf {VRF}",
                "end",
            ],
        )


def _configure_device(
    rt: TopologyRuntime,
    *,
    dev: str,
    ge_addr: str,
    loop_id: int,
    loop_addr: str,
    net: str,
) -> None:
    run_cmds(
        rt=rt,
        device=dev,
        commands=[
            "config",
            f"vrf {VRF}",
            "exit",
            f"if {GE_IF}",
            f"vrf forwarding {VRF}",
            f"ip address {ge_addr} 30",
            "no shutdown",
            "exit",
            f"if loop {loop_id}",
            f"vrf forwarding {VRF}",
            f"ip address {loop_addr} 32",
            "exit",
            f"ospf {OSPF_PROCESS} vrf {VRF}",
            f"router-id {loop_addr}",
            f"area {OSPF_AREA}",
            "exit",
            f"isis {ISIS_TAG} vrf {VRF}",
            f"net {net}",
            "cost-style wide",
            "af ipv4",
            "exit",
            f"if {GE_IF}",
            f"ospf enable {OSPF_PROCESS} area {OSPF_AREA}",
            f"ospf network-type {OSPF_PROCESS} point-to-point",
            f"ospf hello-interval {OSPF_PROCESS} 2",
            f"ospf dead-interval {OSPF_PROCESS} 8",
            f"isis enable {ISIS_TAG}",
            f"isis hello-interval {ISIS_TAG} 2",
            "exit",
            f"if loop {loop_id}",
            f"ospf enable {OSPF_PROCESS} area {OSPF_AREA}",
            f"ospf passive {OSPF_PROCESS}",
            f"isis enable {ISIS_TAG}",
            f"isis passive {ISIS_TAG}",
            "exit",
            "end",
        ],
    )


def _wait_adjacencies(rt: TopologyRuntime, timeout: int = 100) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show ospf neighbor {OSPF_PROCESS}",
                "contains": [R2_LOOP, GE_IF, "Full"],
                "not_regex": [ERROR_RE],
                "label": "r1 OSPF neighbor Full",
            },
            {
                "device": "r2",
                "command": f"show ospf neighbor {OSPF_PROCESS}",
                "contains": [R1_LOOP, GE_IF, "Full"],
                "not_regex": [ERROR_RE],
                "label": "r2 OSPF neighbor Full",
            },
            {
                "device": "r1",
                "command": f"show isis neighbor {ISIS_TAG}",
                "contains": [GE_IF, R2_GE, "Up"],
                "not_regex": [ERROR_RE],
                "label": "r1 IS-IS neighbor Up",
            },
            {
                "device": "r2",
                "command": f"show isis neighbor {ISIS_TAG}",
                "contains": [GE_IF, R1_GE, "Up"],
                "not_regex": [ERROR_RE],
                "label": "r2 IS-IS neighbor Up",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_protocol_candidates(rt: TopologyRuntime, timeout: int = 100) -> None:
    for dev, peer in (("r1", R2_LOOP), ("r2", R1_LOOP)):
        wait_check(
            rt,
            device=dev,
            command=f"show ospf route {OSPF_PROCESS}",
            contains=[f"{peer}/32"],
            not_regex=[ERROR_RE],
            timeout=timeout,
            interval=2,
            label=f"{dev} OSPF candidate for {peer}/32",
        )
        wait_check(
            rt,
            device=dev,
            command=f"show isis route ipv4 {ISIS_TAG}",
            contains=[f"{peer}/32"],
            not_regex=[ERROR_RE],
            timeout=timeout,
            interval=2,
            label=f"{dev} IS-IS candidate for {peer}/32",
        )


def _wait_best_path(
    rt: TopologyRuntime,
    *,
    protocol: str,
    preference: int,
    backup: bool,
    timeout: int = 100,
) -> None:
    other = "isis" if protocol == "ospf" else "ospf"
    other_preference = 115 if protocol == "ospf" else 110

    for dev, peer, nexthop in (
        ("r1", R2_LOOP, R2_GE),
        ("r2", R1_LOOP, R1_GE),
    ):
        if backup:
            route_regex = [
                rf"(?is)Path\s*\[1\]\s*:\s*{re.escape(protocol)}\b.*?"
                rf"Preference\s*:\s*{preference}\b.*?"
                rf"Path\s*\[2\]\s*:\s*{re.escape(other)}\b.*?"
                rf"Preference\s*:\s*{other_preference}\b"
            ]
            route_count = {"Path [": 2}
            route_contains = [f"Routing entry for {peer}/32", "Total 2 path(s)"]
            route_not_regex: list[str] = []
        else:
            route_regex = [
                rf"(?is)Path\s*\[1\]\s*:\s*{re.escape(protocol)}\b.*?"
                rf"Preference\s*:\s*{preference}\b"
            ]
            route_count = {"Path [": 1}
            route_contains = [f"Routing entry for {peer}/32", "Total 1 path(s)"]
            route_not_regex = [rf"(?im)^\s*Path\s*\[\d+\]\s*:\s*{re.escape(other)}\s*$"]

        wait_check(
            rt,
            device=dev,
            command=f"show route ipv4 vrf {VRF} {peer} 32",
            contains=route_contains,
            count=route_count,
            regex=route_regex,
            not_regex=route_not_regex,
            timeout=timeout,
            interval=2,
            label=f"{dev} best={protocol} backup={'yes' if backup else 'no'}",
        )
        wait_check(
            rt,
            device=dev,
            command=f"show fib ipv4 vrf {VRF} {peer} 32",
            contains=[f"Routing entry for {peer}/32", f"Path [1]: {protocol}", f"Nexthop   : {nexthop}"],
            not_contains=[f"Path [1]: {other}"],
            regex=[
                rf"(?im)^\s*Preference\s*:\s*{preference}\s*$",
                r"(?im)^\s*Installed\s*:\s*yes\s*$",
            ],
            timeout=timeout,
            interval=2,
            label=f"{dev} FIB best={protocol}",
        )
        wait_check(
            rt,
            device=dev,
            command=f"show fib os ipv4 vrf {VRF}",
            contains=[f"{peer}/32"],
            regex=[
                rf"(?im)^\s*\S+\s+unicast\s+{re.escape(peer)}/32\s+{re.escape(nexthop)}\s+"
                rf"\S+\s+{re.escape(protocol)}\s+\d+(?:\s+\S+)?\s*$"
            ],
            timeout=timeout,
            interval=2,
            label=f"{dev} OS FIB best={protocol}",
        )


def _wait_public_isolation(rt: TopologyRuntime) -> None:
    for dev, peer in (("r1", R2_LOOP), ("r2", R1_LOOP)):
        wait_check(
            rt,
            device=dev,
            command=f"show route ipv4 {peer} 32",
            contains=["(no matching routes)"],
            timeout=30,
            interval=2,
            label=f"{dev} public RIB excludes {peer}/32",
        )


def _wait_traffic(rt: TopologyRuntime, *, stage: str) -> None:
    for dev, src, dst in (
        ("r1", R1_LOOP, R2_LOOP),
        ("r2", R2_LOOP, R1_LOOP),
    ):
        wait_check(
            rt,
            device=dev,
            command=f"ping {dst} -a {src} vrf {VRF}",
            regex=[PING_SUCCESS_RE],
            timeout=60,
            interval=2,
            normalize_whitespace=False,
            label=f"{dev} {stage} VRF traffic",
        )


def _disable_ospf(rt: TopologyRuntime) -> None:
    for dev, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=dev,
            commands=[
                "config",
                f"if {GE_IF}",
                f"no ospf enable {OSPF_PROCESS}",
                "exit",
                f"if loop {loop_id}",
                f"no ospf enable {OSPF_PROCESS}",
                "exit",
                "end",
            ],
        )


def _enable_ospf(rt: TopologyRuntime) -> None:
    for dev, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=dev,
            commands=[
                "config",
                f"if {GE_IF}",
                f"ospf enable {OSPF_PROCESS} area {OSPF_AREA}",
                f"ospf network-type {OSPF_PROCESS} point-to-point",
                f"ospf hello-interval {OSPF_PROCESS} 2",
                f"ospf dead-interval {OSPF_PROCESS} 8",
                "exit",
                f"if loop {loop_id}",
                f"ospf enable {OSPF_PROCESS} area {OSPF_AREA}",
                f"ospf passive {OSPF_PROCESS}",
                "exit",
                "end",
            ],
        )


def _wait_ospf_withdrawn_isis_healthy(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show ospf neighbor {OSPF_PROCESS}",
                "contains": ["(no OSPF neighbor)"],
                "label": "r1 OSPF adjacency withdrawn",
            },
            {
                "device": "r2",
                "command": f"show ospf neighbor {OSPF_PROCESS}",
                "contains": ["(no OSPF neighbor)"],
                "label": "r2 OSPF adjacency withdrawn",
            },
            {
                "device": "r1",
                "command": f"show isis neighbor {ISIS_TAG}",
                "contains": [GE_IF, R2_GE, "Up"],
                "label": "r1 IS-IS remains Up",
            },
            {
                "device": "r2",
                "command": f"show isis neighbor {ISIS_TAG}",
                "contains": [GE_IF, R1_GE, "Up"],
                "label": "r2 IS-IS remains Up",
            },
        ],
        timeout=45,
        interval=2,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    try:
        step("Configure OSPF and IS-IS for the same prefixes in VRF blue")
        _cleanup(rt)
        _configure_device(
            rt,
            dev="r1",
            ge_addr=R1_GE,
            loop_id=R1_LOOP_ID,
            loop_addr=R1_LOOP,
            net=R1_NET,
        )
        _configure_device(
            rt,
            dev="r2",
            ge_addr=R2_GE,
            loop_id=R2_LOOP_ID,
            loop_addr=R2_LOOP,
            net=R2_NET,
        )
        _wait_adjacencies(rt)
        _wait_protocol_candidates(rt)

        step("Verify OSPF preference 110 wins and IS-IS preference 115 remains backup")
        _wait_best_path(rt, protocol="ospf", preference=110, backup=True)
        _wait_public_isolation(rt)
        _wait_traffic(rt, stage="OSPF-best")

        step("Withdraw OSPF and verify convergence to IS-IS with forwarding intact")
        _disable_ospf(rt)
        _wait_ospf_withdrawn_isis_healthy(rt)
        _wait_best_path(rt, protocol="isis", preference=115, backup=False)
        _wait_traffic(rt, stage="IS-IS-fallback")

        step("Re-enable OSPF and verify best-path and forwarding recovery")
        _enable_ospf(rt)
        _wait_adjacencies(rt)
        _wait_protocol_candidates(rt)
        _wait_best_path(rt, protocol="ospf", preference=110, backup=True)
        _wait_traffic(rt, stage="OSPF-restored")
    finally:
        _cleanup(rt)

