#!/usr/bin/env python3
"""Verify OSPFv2 adjacency, routes, persistence, and isolation in a VRF."""

from __future__ import annotations

from module_api import process_reboot, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF = "blue"
PROCESS = 410
AREA = 0
GE_IF = "GE-1"
R1_GE = "10.41.0.1"
R2_GE = "10.41.0.2"
R1_LOOP_ID = 141
R2_LOOP_ID = 142
R1_LOOP = "10.241.1.1"
R2_LOOP = "10.241.2.2"
ERROR_RE = r"(?im)(?:unknown command|invalid input|module timed out|failed to start module|error:\s)"


def _cleanup(rt: TopologyRuntime) -> None:
    for dev, loop_id, public_ge in (
        ("r1", R1_LOOP_ID, "10.12.0.1"),
        ("r2", R2_LOOP_ID, "10.12.0.2"),
    ):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"no ospf {PROCESS}",
                f"no if loop {loop_id}",
                f"if {GE_IF}",
                "no vrf forwarding",
                f"ip address {public_ge} 30",
                "no shutdown",
                "exit",
                f"no vrf {VRF}",
                "end",
            ],
        )


def _configure(rt: TopologyRuntime, dev: str, ge: str, router_id: str, loop_id: int, loop: str) -> None:
    run_cmds(
        rt=rt,
        device=dev,
        commands=[
            "config",
            f"vrf {VRF}",
            "exit",
            f"if {GE_IF}",
            f"vrf forwarding {VRF}",
            f"ip address {ge} 30",
            "no shutdown",
            "exit",
            f"if loop {loop_id}",
            f"vrf forwarding {VRF}",
            f"ip address {loop} 32",
            "exit",
            f"ospf {PROCESS} vrf {VRF}",
            f"router-id {router_id}",
            f"area {AREA}",
            "exit",
            f"if {GE_IF}",
            f"ospf enable {PROCESS} area {AREA}",
            f"ospf network-type {PROCESS} point-to-point",
            f"ospf hello-interval {PROCESS} 2",
            f"ospf dead-interval {PROCESS} 8",
            "exit",
            f"if loop {loop_id}",
            f"ospf enable {PROCESS} area {AREA}",
            f"ospf passive {PROCESS}",
            "exit",
            "end",
        ],
    )


def _wait_operational(rt: TopologyRuntime, timeout: int = 90) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show ospf neighbor {PROCESS}",
                "contains": [R2_LOOP, GE_IF, "Full"],
                "not_regex": [ERROR_RE],
                "label": "r1 blue OSPF neighbor Full",
            },
            {
                "device": "r2",
                "command": f"show ospf neighbor {PROCESS}",
                "contains": [R1_LOOP, GE_IF, "Full"],
                "not_regex": [ERROR_RE],
                "label": "r2 blue OSPF neighbor Full",
            },
        ],
        timeout=timeout,
        interval=2,
    )
    for dev, peer in (("r1", R2_LOOP), ("r2", R1_LOOP)):
        wait_check(
            rt,
            device=dev,
            command=f"show route ipv4 vrf {VRF} {peer} 32",
            contains=[f"Routing entry for {peer}/32"],
            regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*ospf\s*$"],
            not_regex=[ERROR_RE],
            timeout=timeout,
            interval=2,
            label=f"{dev} blue OSPF RIB route",
        )
        wait_check(
            rt,
            device=dev,
            command=f"show route ipv4 {peer} 32",
            contains=["(no matching routes)"],
            timeout=30,
            interval=2,
            label=f"{dev} public RIB isolation",
        )
        wait_check(
            rt,
            device=dev,
            command=f"show fib ipv4 vrf {VRF} {peer} 32",
            contains=[f"{peer}/32", "ospf"],
            timeout=timeout,
            interval=2,
            label=f"{dev} blue OSPF FIB route",
        )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    try:
        step("Configure OSPFv2 process after process-id with vrf keyword")
        _cleanup(rt)
        _configure(rt, "r1", R1_GE, R1_LOOP, R1_LOOP_ID, R1_LOOP)
        _configure(rt, "r2", R2_GE, R2_LOOP, R2_LOOP_ID, R2_LOOP)
        _wait_operational(rt)

        step("Verify VRF CLI rendering, forwarding, and process restore")
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            contains=[f"ospf {PROCESS} vrf {VRF}", f"vrf forwarding {VRF}"],
            not_regex=[ERROR_RE],
            timeout=30,
            label="OSPF VRF running configuration",
        )
        wait_check(
            rt,
            device="r1",
            command=f"show ospf summary {PROCESS}",
            contains=[VRF, R1_LOOP],
            not_regex=[ERROR_RE],
            timeout=30,
            label="OSPF summary exposes VRF",
        )
        wait_check(
            rt,
            device="r1",
            command=f"ping {R2_LOOP} -a {R1_LOOP} vrf {VRF}",
            regex=[r"(?im)\b0(?:\.0)?%\s+packet loss\b"],
            timeout=60,
            interval=2,
            normalize_whitespace=False,
            label="OSPF VRF forwarding",
        )
        process_reboot(rt, "r1", "ospf", ready_timeout=90)
        _wait_operational(rt)
    finally:
        _cleanup(rt)
