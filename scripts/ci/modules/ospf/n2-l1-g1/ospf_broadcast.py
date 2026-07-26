#!/usr/bin/env python3
"""
OSPFv2 two-router broadcast-network integration check.

Coverage:
- Deterministic DR/BDR election with explicit interface priorities
- Full adjacency and Router-LSA exchange
- DR-originated Network-LSA synchronization
- Passive loopback /32 learning through OSPF
- Route RIB, FIB, OS FIB, and bidirectional sourced forwarding
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    g_top,
    hold_check,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
    wait_fib_route,
)
from top_runner import TopologyRuntime  # noqa: E402


PROCESS_ID = 200
AREA = 0
GE_IF = "GE-1"
HELLO_INTERVAL = 2
DEAD_INTERVAL = 8

R1_ROUTER_ID = "10.255.11.1"
R2_ROUTER_ID = "10.255.22.2"
R1_PRIORITY = 10
R2_PRIORITY = 200
R1_LOOP_ID = 31
R2_LOOP_ID = 32
R1_LOOP_PREFIX = f"{R1_ROUTER_ID}/32"
R2_LOOP_PREFIX = f"{R2_ROUTER_ID}/32"

PING_SUCCESS_RE = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
PING_FAILURE_RE = (
    r"(?im)(?:\b100(?:\.0)?%\s+packet loss\b|network is unreachable|"
    r"destination host unreachable|no route to host|connect:|"
    r"module timed out or failed to respond|failed to start ping)"
)
COMMAND_FAILURE_RE = (
    r"(?im)(?:unknown command|invalid input|command not found|"
    r"module timed out or failed to respond|failed to start module|error:\s)"
)


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup broadcast OSPF and loopback config")
    for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=[
                "end",
                "config",
                f"no ospf {PROCESS_ID}",
                f"no if loop {loop_id}",
                f"if {GE_IF}",
                "no shutdown",
                "exit",
                "end",
            ],
        )


def _verify_cleanup(rt: TopologyRuntime) -> None:
    step("Verify broadcast OSPF and loopback config cleanup")
    checks: list[dict[str, object]] = []
    for device, loop_id, peer_router_id, peer_prefix in (
        ("r1", R1_LOOP_ID, R2_ROUTER_ID, R2_LOOP_PREFIX),
        ("r2", R2_LOOP_ID, R1_ROUTER_ID, R1_LOOP_PREFIX),
    ):
        checks.append(
            {
                "device": device,
                "command": "show current-configuration",
                "not_regex": [
                    rf"(?im)^\s*ospf\s+{PROCESS_ID}\s*$",
                    rf"(?im)^\s*ospf\s+\S+\s+{PROCESS_ID}(?:\s|$)",
                    rf"(?im)^\s*if\s+loop\s+{loop_id}\s*$",
                    COMMAND_FAILURE_RE,
                ],
                "label": f"{device} broadcast OSPF process and loopback config removed",
            }
        )
        checks.extend(
            [
                {
                    "device": device,
                    "command": f"show if {GE_IF}",
                    "contains": ["Proto State: UP", "Link State : UP"],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": f"{device} {GE_IF} restored up",
                },
                {
                    "device": device,
                    "command": f"show route ipv4 {peer_router_id} 32",
                    "contains": ["(no matching routes)"],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": f"{device} Route RIB cleanup for {peer_prefix}",
                },
                {
                    "device": device,
                    "command": "show fib os ipv4",
                    "not_contains": [peer_prefix],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": f"{device} OS FIB cleanup for {peer_prefix}",
                },
            ]
        )
    wait_checks(rt, checks, timeout=30, interval=2)
    wait_fib_route(
        rt,
        device="r1",
        afi="ipv4",
        prefix_addr=R2_ROUTER_ID,
        prefix_len=32,
        expect_present=False,
        timeout=30,
        interval=2,
        label=f"r1 FIB cleanup for {R2_LOOP_PREFIX}",
    )
    wait_fib_route(
        rt,
        device="r2",
        afi="ipv4",
        prefix_addr=R1_ROUTER_ID,
        prefix_len=32,
        expect_present=False,
        timeout=30,
        interval=2,
        label=f"r2 FIB cleanup for {R1_LOOP_PREFIX}",
    )


def _final_cleanup(rt: TopologyRuntime, test_error: BaseException | None) -> None:
    try:
        _cleanup(rt)
        _verify_cleanup(rt)
    except BaseException as cleanup_error:
        if test_error is None:
            raise
        message = f"final broadcast OSPF cleanup verification also failed: {cleanup_error}"
        add_note = getattr(test_error, "add_note", None)
        if callable(add_note):
            add_note(message)
        print(f"ERROR: {message}")


def _configure_router(
    rt: TopologyRuntime,
    *,
    device: str,
    router_id: str,
    priority: int,
    loop_id: int,
) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if loop {loop_id}",
            f"ip address {router_id} 32",
            "exit",
            f"ospf {PROCESS_ID}",
            f"router-id {router_id}",
            f"area {AREA}",
            "exit",
            f"if {GE_IF}",
            f"ospf enable {PROCESS_ID} area {AREA}",
            f"ospf network-type {PROCESS_ID} broadcast",
            f"ospf priority {PROCESS_ID} {priority}",
            f"ospf hello-interval {PROCESS_ID} {HELLO_INTERVAL}",
            f"ospf dead-interval {PROCESS_ID} {DEAD_INTERVAL}",
            "exit",
            f"if loop {loop_id}",
            f"ospf enable {PROCESS_ID} area {AREA}",
            f"ospf passive {PROCESS_ID}",
            "exit",
            "end",
        ],
    )


def _wait_full_adjacencies(rt: TopologyRuntime, *, timeout: int = 90) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show ospf neighbor {PROCESS_ID}",
                "contains": [GE_IF, R2_ROUTER_ID],
                "not_contains": ["(no OSPF neighbor)", "(instance not found)"],
                "regex": [r"(?im)\bFull\b"],
                "label": "r1 sees r2 broadcast OSPF neighbor Full",
            },
            {
                "device": "r2",
                "command": f"show ospf neighbor {PROCESS_ID}",
                "contains": [GE_IF, R1_ROUTER_ID],
                "not_contains": ["(no OSPF neighbor)", "(instance not found)"],
                "regex": [r"(?im)\bFull\b"],
                "label": "r2 sees r1 broadcast OSPF neighbor Full",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_route_present(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    nexthop: str,
    advertising_router: str,
    timeout: int = 90,
) -> None:
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_check(
        rt,
        device=device,
        command=f"show ospf route {PROCESS_ID}",
        timeout=timeout,
        interval=2,
        regex=[
            rf"(?im)^\s*{PROCESS_ID}\s+{re.escape(prefix)}\s+\d+\s+"
            rf"{re.escape(nexthop)}\s+\S+\s+{re.escape(advertising_router)}\s*$"
        ],
        not_contains=["(no OSPF route)", "(instance not found)"],
        not_regex=[COMMAND_FAILURE_RE],
        label=f"{device} OSPF protocol route {prefix}",
    )
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {prefix_addr} {prefix_len}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {prefix}"],
        regex=[
            r"(?im)^\s*Path\s*\[\d+\]\s*:\s*ospf\s*$",
            rf"(?im)^\s*Nexthop\s*:\s*{re.escape(nexthop)}\s*$",
            r"(?im)^\s*Preference\s*:\s*110\s*$",
        ],
        not_regex=[COMMAND_FAILURE_RE],
        label=f"{device} Route RIB has OSPF route {prefix}",
    )
    wait_fib_route(
        rt,
        device=device,
        afi="ipv4",
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=True,
        nexthop=nexthop,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} FIB has installed OSPF route {prefix}",
    )
    wait_check(
        rt,
        device=device,
        command="show fib os ipv4",
        timeout=timeout,
        interval=2,
        regex=[
            rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(nexthop)}\s+\S+\s+ospf\s+\d+\b"
        ],
        not_regex=[COMMAND_FAILURE_RE],
        label=f"{device} OS FIB has OSPF route {prefix}",
    )


def _wait_sourced_ping(
    rt: TopologyRuntime,
    *,
    device: str,
    source: str,
    destination: str,
    timeout: int = 45,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"ping {destination} -a {source}",
        timeout=timeout,
        interval=2,
        regex=[PING_SUCCESS_RE],
        not_regex=[PING_FAILURE_RE],
        normalize_whitespace=False,
        label=f"{device} sourced ping {source} -> {destination}",
    )


def _interface_row_regex(state: str) -> str:
    return (
        rf"(?im)^\s*{PROCESS_ID}\s+{re.escape(GE_IF)}\s+0\.0\.0\.0\s+"
        rf"{state}\s+Broadcast\s+\d+\s+{HELLO_INTERVAL}\s+{DEAD_INTERVAL}\s*$"
    )


def _neighbor_row_regex(
    *,
    peer_router_id: str,
    peer_link_ip: str,
    peer_priority: int,
    dr_link_ip: str,
    bdr_link_ip: str,
) -> str:
    return (
        rf"(?im)^\s*{PROCESS_ID}\s+{re.escape(peer_router_id)}\s+"
        rf"{re.escape(peer_link_ip)}\s+{re.escape(GE_IF)}\s+Full\s+\d+\s+"
        rf"{peer_priority}\s+{re.escape(dr_link_ip)}\s+{re.escape(bdr_link_ip)}\b"
    )


def _lsa_row_regex(
    *,
    lsa_type: str,
    link_state_id: str,
    advertising_router: str,
) -> str:
    return (
        rf"(?im)^\s*{PROCESS_ID}\s+0\.0\.0\.0\s+{lsa_type}\s+"
        rf"{re.escape(link_state_id)}\s+{re.escape(advertising_router)}\s+"
        r"0x[0-9a-f]+\s+\d+\s+0x[0-9a-f]+\s*$"
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_link_ip = str(g_top.r1.GE_1.ip)
    r2_link_ip = str(g_top.r2.GE_1.ip)
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    test_error: BaseException | None = None
    try:
        _cleanup(rt)

        step("Verify GE-1 baseline connectivity")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r1_link_ip}/30",
                    ],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "r2",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r2_link_ip}/30",
                    ],
                    "label": "r2 GE-1 up",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure broadcast OSPF with deterministic priorities")
        _configure_router(
            rt,
            device="r2",
            router_id=R2_ROUTER_ID,
            priority=R2_PRIORITY,
            loop_id=R2_LOOP_ID,
        )
        _configure_router(
            rt,
            device="r1",
            router_id=R1_ROUTER_ID,
            priority=R1_PRIORITY,
            loop_id=R1_LOOP_ID,
        )

        step("Wait for bidirectional broadcast OSPF Full adjacency")
        _wait_full_adjacencies(rt)
        hold_check(
            rt,
            device="r1",
            command=f"show ospf neighbor {PROCESS_ID}",
            duration=6,
            interval=2,
            contains=[GE_IF, R2_ROUTER_ID],
            regex=[r"(?im)\bFull\b"],
            label="r1 broadcast OSPF adjacency remains Full",
        )

        step("Verify r2 is DR and r1 is Backup")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show ospf interface {PROCESS_ID}",
                    "regex": [_interface_row_regex("Backup")],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r1 GE-1 is broadcast Backup",
                },
                {
                    "device": "r2",
                    "command": f"show ospf interface {PROCESS_ID}",
                    "regex": [_interface_row_regex("DR")],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r2 GE-1 is broadcast DR",
                },
                {
                    "device": "r1",
                    "command": f"show ospf neighbor {PROCESS_ID} verbose",
                    "regex": [
                        _neighbor_row_regex(
                            peer_router_id=R2_ROUTER_ID,
                            peer_link_ip=r2_link_ip,
                            peer_priority=R2_PRIORITY,
                            dr_link_ip=r2_link_ip,
                            bdr_link_ip=r1_link_ip,
                        )
                    ],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r1 sees r2 priority and DR/BDR election",
                },
                {
                    "device": "r2",
                    "command": f"show ospf neighbor {PROCESS_ID} verbose",
                    "regex": [
                        _neighbor_row_regex(
                            peer_router_id=R1_ROUTER_ID,
                            peer_link_ip=r1_link_ip,
                            peer_priority=R1_PRIORITY,
                            dr_link_ip=r2_link_ip,
                            bdr_link_ip=r1_link_ip,
                        )
                    ],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r2 sees r1 priority and DR/BDR election",
                },
            ],
            timeout=60,
            interval=2,
        )

        step("Verify Router-LSAs and the DR Network-LSA are synchronized")
        router_lsa_patterns = [
            _lsa_row_regex(
                lsa_type="Router",
                link_state_id=R1_ROUTER_ID,
                advertising_router=R1_ROUTER_ID,
            ),
            _lsa_row_regex(
                lsa_type="Router",
                link_state_id=R2_ROUTER_ID,
                advertising_router=R2_ROUTER_ID,
            ),
            _lsa_row_regex(
                lsa_type="Network",
                link_state_id=r2_link_ip,
                advertising_router=R2_ROUTER_ID,
            ),
        ]
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show ospf lsdb {PROCESS_ID}",
                    "regex": router_lsa_patterns,
                    "not_contains": ["(no OSPF LSA)", "(instance not found)"],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r1 LSDB has both Router-LSAs and DR Network-LSA",
                },
                {
                    "device": "r2",
                    "command": f"show ospf lsdb {PROCESS_ID}",
                    "regex": router_lsa_patterns,
                    "not_contains": ["(no OSPF LSA)", "(instance not found)"],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r2 LSDB has both Router-LSAs and DR Network-LSA",
                },
            ],
            timeout=60,
            interval=2,
        )

        step("Verify learned loopbacks in OSPF RIB, Route RIB, FIB, and OS")
        _wait_route_present(
            rt, device="r1", prefix=R2_LOOP_PREFIX, nexthop=r1_peer_ip, advertising_router=R2_ROUTER_ID
        )
        _wait_route_present(
            rt, device="r2", prefix=R1_LOOP_PREFIX, nexthop=r2_peer_ip, advertising_router=R1_ROUTER_ID
        )

        step("Verify bidirectional sourced forwarding")
        _wait_sourced_ping(rt, device="r1", source=R1_ROUTER_ID, destination=R2_ROUTER_ID)
        _wait_sourced_ping(rt, device="r2", source=R2_ROUTER_ID, destination=R1_ROUTER_ID)

        step("Verify broadcast OSPF running-configuration rendering")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "contains": [
                        f"ospf {PROCESS_ID}",
                        f"router-id {R1_ROUTER_ID}",
                        f"ospf enable {PROCESS_ID} area {AREA}",
                        f"ospf priority {PROCESS_ID} {R1_PRIORITY}",
                        f"ospf hello-interval {PROCESS_ID} {HELLO_INTERVAL}",
                        f"ospf dead-interval {PROCESS_ID} {DEAD_INTERVAL}",
                        f"ospf passive {PROCESS_ID}",
                    ],
                    "regex": [rf"(?m)^\s+area\s+{AREA}\r?$"],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r1 broadcast OSPF running config",
                },
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "contains": [
                        f"ospf {PROCESS_ID}",
                        f"router-id {R2_ROUTER_ID}",
                        f"ospf enable {PROCESS_ID} area {AREA}",
                        f"ospf priority {PROCESS_ID} {R2_PRIORITY}",
                        f"ospf hello-interval {PROCESS_ID} {HELLO_INTERVAL}",
                        f"ospf dead-interval {PROCESS_ID} {DEAD_INTERVAL}",
                        f"ospf passive {PROCESS_ID}",
                    ],
                    "regex": [rf"(?m)^\s+area\s+{AREA}\r?$"],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r2 broadcast OSPF running config",
                },
            ],
            timeout=30,
            interval=2,
        )

        print("OSPFv2 broadcast DR/BDR check passed.")
    except BaseException as error:
        test_error = error
        raise
    finally:
        if not should_skip_cleanup():
            _final_cleanup(rt, test_error)
