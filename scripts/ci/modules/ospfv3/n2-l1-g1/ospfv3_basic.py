#!/usr/bin/env python3
"""OSPFv3 two-router point-to-point integration verification."""

from __future__ import annotations

from module_api import (  # noqa: E402
    process_reboot,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
    wait_fib_ipv6_route,
)
from top_runner import TopologyRuntime  # noqa: E402


PROCESS_ID = 300
AREA = 0
GE_IF = "GE-1"
HELLO = 2
DEAD = 8
R1_ROUTER_ID = "10.255.1.1"
R2_ROUTER_ID = "10.255.2.2"
R1_LOOP_ID = 61
R2_LOOP_ID = 62
R1_LOOP = "2001:db8:1::1"
R2_LOOP = "2001:db8:2::2"
PING_OK = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
COMMAND_ERROR = r"(?im)(?:unknown command|invalid input|module timed out|failed to start module|error:\s)"


def _cleanup(rt: TopologyRuntime) -> None:
    for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=[
                "end",
                "config",
                f"no ospfv3 {PROCESS_ID}",
                f"no if loop {loop_id}",
                "end",
            ],
        )


def _configure(rt: TopologyRuntime, device: str, router_id: str, loop_id: int, loop_addr: str) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if loop {loop_id}",
            f"ipv6 address {loop_addr} 128",
            "exit",
            f"ospfv3 {PROCESS_ID}",
            f"router-id {router_id}",
            f"area {AREA}",
            "exit",
            f"if {GE_IF}",
            f"ospfv3 enable {PROCESS_ID} area {AREA}",
            f"ospfv3 network-type {PROCESS_ID} point-to-point",
            f"ospfv3 hello-interval {PROCESS_ID} {HELLO}",
            f"ospfv3 dead-interval {PROCESS_ID} {DEAD}",
            "exit",
            f"if loop {loop_id}",
            f"ospfv3 enable {PROCESS_ID} area {AREA}",
            f"ospfv3 passive {PROCESS_ID}",
            "exit",
            "end",
        ],
    )


def _wait_full(rt: TopologyRuntime, timeout: int = 90) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show ospfv3 neighbor {PROCESS_ID}",
                "contains": [GE_IF, R2_ROUTER_ID, "Full"],
                "regex": [r"(?i)fe80:[0-9a-f:]+",],
                "not_regex": [COMMAND_ERROR],
                "label": "r1 OSPFv3 neighbor Full",
            },
            {
                "device": "r2",
                "command": f"show ospfv3 neighbor {PROCESS_ID}",
                "contains": [GE_IF, R1_ROUTER_ID, "Full"],
                "regex": [r"(?i)fe80:[0-9a-f:]+",],
                "not_regex": [COMMAND_ERROR],
                "label": "r2 OSPFv3 neighbor Full",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_routes(rt: TopologyRuntime, timeout: int = 90) -> None:
    for device, peer_loop in (("r1", R2_LOOP), ("r2", R1_LOOP)):
        wait_check(
            rt,
            device=device,
            command=f"show ospfv3 route {PROCESS_ID}",
            contains=[f"{peer_loop}/128"],
            regex=[r"(?i)fe80:[0-9a-f:]+"],
            not_regex=[COMMAND_ERROR],
            timeout=timeout,
            interval=2,
            label=f"{device} OSPFv3 route to {peer_loop}/128",
        )
        wait_check(
            rt,
            device=device,
            command=f"show route ipv6 {peer_loop} 128",
            contains=[f"Routing entry for {peer_loop}/128"],
            regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*ospfv3\s*$", r"(?im)^\s*Preference\s*:\s*110\s*$"],
            not_regex=[COMMAND_ERROR],
            timeout=timeout,
            interval=2,
            label=f"{device} Route RIB has OSPFv3 {peer_loop}/128",
        )
        wait_check(
            rt,
            device=device,
            command="show route ipv6 proto ospfv3",
            contains=[f"{peer_loop}/128"],
            not_regex=[COMMAND_ERROR],
            timeout=timeout,
            interval=2,
            label=f"{device} OSPFv3 protocol-filtered RIB",
        )
        wait_fib_ipv6_route(
            rt,
            device=device,
            prefix_addr=peer_loop,
            prefix_len=128,
            expect_present=True,
            installed=True,
            skip_os=False,
            timeout=timeout,
            interval=2,
            label=f"{device} FIB has OSPFv3 {peer_loop}/128",
        )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    test_error: BaseException | None = None
    try:
        step("Configure OSPFv3 point-to-point adjacency and passive IPv6 loopbacks")
        _cleanup(rt)
        _configure(rt, "r1", R1_ROUTER_ID, R1_LOOP_ID, R1_LOOP)
        _configure(rt, "r2", R2_ROUTER_ID, R2_LOOP_ID, R2_LOOP)

        _wait_full(rt)
        step("Verify OSPFv3 Router/Link/Intra-Area-Prefix LSAs")
        for device in ("r1", "r2"):
            wait_check(
                rt,
                device=device,
                command=f"show ospfv3 lsdb {PROCESS_ID}",
                contains=["Router", "Link", "Intra-Prefix", R1_ROUTER_ID, R2_ROUTER_ID],
                not_regex=[COMMAND_ERROR],
                timeout=60,
                interval=2,
                label=f"{device} OSPFv3 LSDB",
            )

        step("Verify IPv6 route installation and forwarding")
        _wait_routes(rt)
        for device, destination, source in (
            ("r1", R2_LOOP, R1_LOOP),
            ("r2", R1_LOOP, R2_LOOP),
        ):
            wait_check(
                rt,
                device=device,
                command=f"ping ipv6 {destination} -a {source}",
                regex=[PING_OK],
                not_regex=[COMMAND_ERROR],
                normalize_whitespace=False,
                timeout=60,
                interval=2,
                label=f"{device} OSPFv3 IPv6 forwarding",
            )

        step("Verify OSPFv3 process restart restores state")
        process_reboot(rt, "r1", "ospfv3", ready_timeout=90)
        _wait_full(rt, timeout=90)
        _wait_routes(rt, timeout=90)
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            contains=[
                f"ospfv3 {PROCESS_ID}",
                f"router-id {R1_ROUTER_ID}",
                f"ospfv3 enable {PROCESS_ID} area {AREA}",
                f"ospfv3 passive {PROCESS_ID}",
            ],
            not_regex=[COMMAND_ERROR],
            timeout=30,
            interval=2,
            label="OSPFv3 running configuration persisted",
        )
        print("OSPFv3 basic integration verification passed.")
    except BaseException as exc:
        test_error = exc
        raise
    finally:
        if not should_skip_cleanup():
            try:
                _cleanup(rt)
            except BaseException:
                if test_error is None:
                    raise
