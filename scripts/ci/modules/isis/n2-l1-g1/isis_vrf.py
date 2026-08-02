#!/usr/bin/env python3
"""Verify dual-stack IS-IS adjacency, routes, persistence, and isolation in a VRF."""

from __future__ import annotations

from module_api import process_reboot, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF = "blue"
TAG = 430
GE_IF = "GE-1"
R1_GE4, R2_GE4 = "10.43.0.1", "10.43.0.2"
R1_GE6, R2_GE6 = "2001:db8:43::1", "2001:db8:43::2"
R1_LOOP_ID, R2_LOOP_ID = 161, 162
R1_LOOP4, R2_LOOP4 = "10.243.1.1", "10.243.2.2"
R1_LOOP6, R2_LOOP6 = "2001:db8:243::1", "2001:db8:244::2"
R1_NET = "49.0001.0000.0000.0431.00"
R2_NET = "49.0001.0000.0000.0432.00"
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
                f"no isis {TAG}",
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


def _configure(
    rt: TopologyRuntime,
    dev: str,
    ge4: str,
    ge6: str,
    loop_id: int,
    loop4: str,
    loop6: str,
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
            f"ip address {ge4} 30",
            f"ipv6 address {ge6} 64",
            "no shutdown",
            "exit",
            f"if loop {loop_id}",
            f"vrf forwarding {VRF}",
            f"ip address {loop4} 32",
            f"ipv6 address {loop6} 128",
            "exit",
            f"isis {TAG} vrf {VRF}",
            f"net {net}",
            "cost-style wide",
            "af ipv4",
            "af ipv6",
            "exit",
            "exit",
            f"if {GE_IF}",
            f"isis enable {TAG}",
            f"isis ipv6 enable {TAG}",
            f"isis hello-interval {TAG} 2",
            f"isis ipv6 hello-interval {TAG} 2",
            "exit",
            f"if loop {loop_id}",
            f"isis enable {TAG}",
            f"isis ipv6 enable {TAG}",
            f"isis passive {TAG}",
            f"isis ipv6 passive {TAG}",
            "exit",
            "end",
        ],
    )


def _wait_operational(rt: TopologyRuntime, timeout: int = 100) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis neighbor {TAG}",
                "contains": [GE_IF, R2_GE4, "Up"],
                "not_regex": [ERROR_RE],
                "label": "r1 blue IS-IS neighbor Up",
            },
            {
                "device": "r2",
                "command": f"show isis neighbor {TAG}",
                "contains": [GE_IF, R1_GE4, "Up"],
                "not_regex": [ERROR_RE],
                "label": "r2 blue IS-IS neighbor Up",
            },
        ],
        timeout=timeout,
        interval=2,
    )
    for dev, peer4, peer6 in (("r1", R2_LOOP4, R2_LOOP6), ("r2", R1_LOOP4, R1_LOOP6)):
        for afi, peer, length in (("ipv4", peer4, 32), ("ipv6", peer6, 128)):
            wait_check(
                rt,
                device=dev,
                command=f"show route {afi} vrf {VRF} {peer} {length}",
                contains=[f"Routing entry for {peer}/{length}"],
                regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*isis\s*$"],
                timeout=timeout,
                interval=2,
                label=f"{dev} blue IS-IS {afi} RIB route",
            )
            wait_check(
                rt,
                device=dev,
                command=f"show route {afi} {peer} {length}",
                contains=["(no matching routes)"],
                timeout=30,
                interval=2,
                label=f"{dev} public {afi} RIB isolation",
            )
            wait_check(
                rt,
                device=dev,
                command=f"show fib {afi} vrf {VRF} {peer} {length}",
                contains=[f"{peer}/{length}", "isis"],
                timeout=timeout,
                interval=2,
                label=f"{dev} blue IS-IS {afi} FIB route",
            )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    try:
        step("Configure IS-IS process after process-id with vrf keyword")
        _cleanup(rt)
        _configure(rt, "r1", R1_GE4, R1_GE6, R1_LOOP_ID, R1_LOOP4, R1_LOOP6, R1_NET)
        _configure(rt, "r2", R2_GE4, R2_GE6, R2_LOOP_ID, R2_LOOP4, R2_LOOP6, R2_NET)
        _wait_operational(rt)

        step("Verify IS-IS VRF rendering, dual-stack forwarding, and process restore")
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            contains=[f"isis {TAG} vrf {VRF}", f"vrf forwarding {VRF}"],
            not_regex=[ERROR_RE],
            timeout=30,
            label="IS-IS VRF running configuration",
        )
        wait_check(
            rt,
            device="r1",
            command=f"show isis summary ipv4 {TAG}",
            contains=[VRF],
            not_regex=[ERROR_RE],
            timeout=30,
            label="IS-IS summary exposes VRF",
        )
        for command in (
            f"ping {R2_LOOP4} -a {R1_LOOP4} vrf {VRF}",
            f"ping ipv6 {R2_LOOP6} -a {R1_LOOP6} vrf {VRF}",
        ):
            wait_check(
                rt,
                device="r1",
                command=command,
                regex=[r"(?im)\b0(?:\.0)?%\s+packet loss\b"],
                timeout=60,
                interval=2,
                normalize_whitespace=False,
                label=f"IS-IS VRF forwarding: {command}",
            )
        process_reboot(rt, "r1", "isis", ready_timeout=90)
        _wait_operational(rt)
    finally:
        _cleanup(rt)
