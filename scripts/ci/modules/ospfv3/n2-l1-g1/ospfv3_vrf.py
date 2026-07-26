#!/usr/bin/env python3
"""Verify OSPFv3 adjacency, routes, persistence, and isolation in a VRF."""

from __future__ import annotations

from module_api import process_reboot, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF = "blue"
PROCESS = 420
GE_IF = "GE-1"
R1_GE4, R2_GE4 = "10.42.0.1", "10.42.0.2"
R1_GE6, R2_GE6 = "2001:db8:42::1", "2001:db8:42::2"
R1_LOOP_ID, R2_LOOP_ID = 151, 152
R1_LOOP, R2_LOOP = "2001:db8:241::1", "2001:db8:242::2"
R1_RID, R2_RID = "10.242.1.1", "10.242.2.2"
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
                f"no ospfv3 {PROCESS}",
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


def _configure(rt: TopologyRuntime, dev: str, ge4: str, ge6: str, rid: str, loop_id: int, loop: str) -> None:
    run_cmds(
        rt=rt,
        device=dev,
        commands=[
            "config",
            f"vrf {VRF}",
            "exit",
            f"if {GE_IF}",
            f"vrf forwarding {VRF}",
            f"ip address {ge4} 30",
            f"ipv6 address {ge6} 64",
            "no shutdown",
            "exit",
            f"if loop {loop_id}",
            f"vrf forwarding {VRF}",
            f"ipv6 address {loop} 128",
            "exit",
            f"ospfv3 {PROCESS} vrf {VRF}",
            f"router-id {rid}",
            "area 0",
            "exit",
            f"if {GE_IF}",
            f"ospfv3 enable {PROCESS} area 0",
            f"ospfv3 network-type {PROCESS} point-to-point",
            f"ospfv3 hello-interval {PROCESS} 2",
            f"ospfv3 dead-interval {PROCESS} 8",
            "exit",
            f"if loop {loop_id}",
            f"ospfv3 enable {PROCESS} area 0",
            f"ospfv3 passive {PROCESS}",
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
                "command": f"show ospfv3 neighbor {PROCESS}",
                "contains": [R2_RID, GE_IF, "Full"],
                "not_regex": [ERROR_RE],
                "label": "r1 blue OSPFv3 neighbor Full",
            },
            {
                "device": "r2",
                "command": f"show ospfv3 neighbor {PROCESS}",
                "contains": [R1_RID, GE_IF, "Full"],
                "not_regex": [ERROR_RE],
                "label": "r2 blue OSPFv3 neighbor Full",
            },
        ],
        timeout=timeout,
        interval=2,
    )
    for dev, peer in (("r1", R2_LOOP), ("r2", R1_LOOP)):
        wait_check(
            rt,
            device=dev,
            command=f"show route ipv6 vrf {VRF} {peer} 128",
            contains=[f"Routing entry for {peer}/128"],
            regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*ospfv3\s*$"],
            timeout=timeout,
            interval=2,
            label=f"{dev} blue OSPFv3 RIB route",
        )
        wait_check(
            rt,
            device=dev,
            command=f"show route ipv6 {peer} 128",
            contains=["(no matching routes)"],
            timeout=30,
            interval=2,
            label=f"{dev} public IPv6 RIB isolation",
        )
        wait_check(
            rt,
            device=dev,
            command=f"show fib ipv6 vrf {VRF} {peer} 128",
            contains=[f"{peer}/128", "ospfv3"],
            timeout=timeout,
            interval=2,
            label=f"{dev} blue OSPFv3 FIB route",
        )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    try:
        step("Configure OSPFv3 process after process-id with vrf keyword")
        _cleanup(rt)
        _configure(rt, "r1", R1_GE4, R1_GE6, R1_RID, R1_LOOP_ID, R1_LOOP)
        _configure(rt, "r2", R2_GE4, R2_GE6, R2_RID, R2_LOOP_ID, R2_LOOP)
        _wait_operational(rt)

        step("Verify OSPFv3 VRF rendering, forwarding, and process restore")
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            contains=[f"ospfv3 {PROCESS} vrf {VRF}", f"vrf forwarding {VRF}"],
            not_regex=[ERROR_RE],
            timeout=30,
            label="OSPFv3 VRF running configuration",
        )
        wait_check(
            rt,
            device="r1",
            command=f"show ospfv3 summary {PROCESS}",
            contains=[VRF, R1_RID],
            not_regex=[ERROR_RE],
            timeout=30,
            label="OSPFv3 summary exposes VRF",
        )
        wait_check(
            rt,
            device="r1",
            command=f"ping ipv6 {R2_LOOP} -a {R1_LOOP} vrf {VRF}",
            regex=[r"(?im)\b0(?:\.0)?%\s+packet loss\b"],
            timeout=60,
            interval=2,
            normalize_whitespace=False,
            label="OSPFv3 VRF forwarding",
        )
        process_reboot(rt, "r1", "ospfv3", ready_timeout=90)
        _wait_operational(rt)
    finally:
        _cleanup(rt)
