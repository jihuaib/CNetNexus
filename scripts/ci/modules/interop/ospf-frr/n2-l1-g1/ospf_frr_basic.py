#!/usr/bin/env python3
"""
NetNexus <-> FRR OSPFv2 point-to-point interoperability check.

Coverage:
- Hello and database synchronization to Full on both implementations
- Router-LSA and passive loopback /32 exchange
- Route installation and sourced forwarding in both directions
- Adjacency and route withdrawal after FRR disables OSPF on the link
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    frr_config,
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


PROCESS_ID = 100
AREA = 0
GE_IF = "GE-1"
FRR_LINUX_IF = "eth1"
HELLO_INTERVAL = 2
DEAD_INTERVAL = 8

NN_ROUTER_ID = "10.255.1.1"
FRR_ROUTER_ID = "10.255.2.2"
NN_LOOP_ID = 11
NN_LOOP_PREFIX = f"{NN_ROUTER_ID}/32"
FRR_LOOP_PREFIX = f"{FRR_ROUTER_ID}/32"

PING_SUCCESS_RE = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
PING_FAILURE_RE = (
    r"(?im)(?:\b100(?:\.0)?%\s+packet loss\b|network is unreachable|"
    r"destination host unreachable|no route to host|connect:|"
    r"module timed out or failed to respond|failed to start ping)"
)
COMMAND_FAILURE_RE = (
    r"(?im)(?:unknown command|invalid input|command not found|can't find|"
    r"module timed out or failed to respond|failed to start module|error:\s)"
)


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup NetNexus and FRR OSPF config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no ospf {PROCESS_ID}",
            f"no if loop {NN_LOOP_ID}",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    for command in (
        "no ip ospf area 0.0.0.0",
        "no ip ospf network",
        "no ip ospf hello-interval",
        "no ip ospf dead-interval",
    ):
        frr_config(rt, "f1", [f"interface {FRR_LINUX_IF}", command], strict=False)
    frr_config(rt, "f1", ["interface lo", "no ip ospf area 0.0.0.0"], strict=False)
    frr_config(rt, "f1", ["no router ospf"], strict=False)
    rt.exec_cmd("f1", f"ip addr del {FRR_ROUTER_ID}/32 dev lo", strict=False)


def _verify_cleanup(rt: TopologyRuntime) -> None:
    step("Verify NetNexus and FRR OSPF cleanup")
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show current-configuration",
                "not_regex": [
                    rf"(?im)^\s*ospf\s+{PROCESS_ID}\s*$",
                    rf"(?im)^\s*ospf\s+\S+\s+{PROCESS_ID}(?:\s|$)",
                    rf"(?im)^\s*if\s+loop\s+{NN_LOOP_ID}\s*$",
                    COMMAND_FAILURE_RE,
                ],
                "label": "NetNexus OSPF process and loopback config removed",
            },
            {
                "device": "f1",
                "command": "vtysh -c 'show running-config'",
                "not_regex": [
                    r"(?im)^\s*router\s+ospf(?:\s|$)",
                    r"(?im)^\s*(?:no\s+)?ip\s+ospf(?:\s|$)",
                    COMMAND_FAILURE_RE,
                ],
                "label": "FRR OSPF process and all interface OSPF commands removed",
            },
            {
                "device": "f1",
                "command": "ip -4 addr show dev lo",
                "not_contains": [FRR_LOOP_PREFIX],
                "not_regex": [COMMAND_FAILURE_RE],
                "label": "FRR loopback test address removed",
            },
        ],
        timeout=30,
        interval=2,
    )


def _final_cleanup(rt: TopologyRuntime, test_error: BaseException | None) -> None:
    try:
        _cleanup(rt)
        _verify_cleanup(rt)
    except BaseException as cleanup_error:
        if test_error is None:
            raise
        message = f"final NetNexus/FRR OSPF cleanup verification also failed: {cleanup_error}"
        add_note = getattr(test_error, "add_note", None)
        if callable(add_note):
            add_note(message)
        print(f"ERROR: {message}")


def _configure_netnexus(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if loop {NN_LOOP_ID}",
            f"ip address {NN_ROUTER_ID} 32",
            "exit",
            f"ospf {PROCESS_ID}",
            f"router-id {NN_ROUTER_ID}",
            f"area {AREA}",
            "exit",
            f"if {GE_IF}",
            f"ospf enable {PROCESS_ID} area {AREA}",
            f"ospf network-type {PROCESS_ID} point-to-point",
            f"ospf hello-interval {PROCESS_ID} {HELLO_INTERVAL}",
            f"ospf dead-interval {PROCESS_ID} {DEAD_INTERVAL}",
            "exit",
            f"if loop {NN_LOOP_ID}",
            f"ospf enable {PROCESS_ID} area {AREA}",
            f"ospf passive {PROCESS_ID}",
            "exit",
            "end",
        ],
    )


def _configure_frr(rt: TopologyRuntime) -> None:
    rt.exec_cmd("f1", "ip link set dev lo up")
    rt.exec_cmd("f1", f"ip addr replace {FRR_ROUTER_ID}/32 dev lo")
    frr_config(
        rt,
        "f1",
        [
            "router ospf",
            f"ospf router-id {FRR_ROUTER_ID}",
            "exit",
            f"interface {FRR_LINUX_IF}",
            "ip ospf area 0.0.0.0",
            "ip ospf network point-to-point",
            f"ip ospf hello-interval {HELLO_INTERVAL}",
            f"ip ospf dead-interval {DEAD_INTERVAL}",
            "exit",
            "interface lo",
            "ip ospf area 0.0.0.0",
            "exit",
        ],
    )


def _wait_netnexus_route_present(
    rt: TopologyRuntime,
    *,
    nexthop: str,
    timeout: int = 120,
) -> None:
    wait_check(
        rt,
        device="r1",
        command=f"show ospf route {PROCESS_ID}",
        timeout=timeout,
        interval=3,
        regex=[
            rf"(?im)^\s*{PROCESS_ID}\s+{re.escape(FRR_LOOP_PREFIX)}\s+\d+\s+"
            rf"{re.escape(nexthop)}\s+\S+\s+{re.escape(FRR_ROUTER_ID)}\s*$"
        ],
        not_contains=["(no OSPF route)", "(instance not found)"],
        not_regex=[COMMAND_FAILURE_RE],
        label="NetNexus OSPF RIB has FRR loopback",
    )
    wait_check(
        rt,
        device="r1",
        command=f"show route ipv4 {FRR_ROUTER_ID} 32",
        timeout=timeout,
        interval=3,
        contains=[f"Routing entry for {FRR_LOOP_PREFIX}"],
        regex=[
            r"(?im)^\s*Path\s*\[\d+\]\s*:\s*ospf\s*$",
            rf"(?im)^\s*Nexthop\s*:\s*{re.escape(nexthop)}\s*$",
            r"(?im)^\s*Preference\s*:\s*110\s*$",
        ],
        label="NetNexus Route RIB has FRR loopback via OSPF",
    )
    wait_fib_route(
        rt,
        device="r1",
        afi="ipv4",
        prefix_addr=FRR_ROUTER_ID,
        prefix_len=32,
        expect_present=True,
        nexthop=nexthop,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=3,
        label="NetNexus FIB has installed FRR loopback",
    )
    wait_check(
        rt,
        device="r1",
        command="show fib os ipv4",
        timeout=timeout,
        interval=3,
        regex=[
            rf"(?im)^\s*main\s+unicast\s+{re.escape(FRR_LOOP_PREFIX)}\s+"
            rf"{re.escape(nexthop)}\s+\S+\s+ospf\s+\d+\b"
        ],
        label="NetNexus OS FIB has FRR loopback via OSPF",
    )


def _wait_netnexus_route_absent(rt: TopologyRuntime, *, timeout: int = 45) -> None:
    wait_check(
        rt,
        device="r1",
        command=f"show ospf route {PROCESS_ID}",
        timeout=timeout,
        interval=2,
        not_contains=[FRR_LOOP_PREFIX],
        not_regex=[COMMAND_FAILURE_RE],
        label="NetNexus OSPF RIB withdrew FRR loopback",
    )
    wait_check(
        rt,
        device="r1",
        command=f"show route ipv4 {FRR_ROUTER_ID} 32",
        timeout=timeout,
        interval=2,
        contains=["(no matching routes)"],
        not_regex=[
            r"(?im)^\s*Path\s*\[\d+\]\s*:\s*ospf\s*$",
            COMMAND_FAILURE_RE,
        ],
        label="NetNexus Route RIB withdrew FRR loopback",
    )
    wait_fib_route(
        rt,
        device="r1",
        afi="ipv4",
        prefix_addr=FRR_ROUTER_ID,
        prefix_len=32,
        expect_present=False,
        timeout=timeout,
        interval=2,
        label="NetNexus FIB withdrew FRR loopback",
    )
    wait_check(
        rt,
        device="r1",
        command="show fib os ipv4",
        timeout=timeout,
        interval=2,
        not_contains=[FRR_LOOP_PREFIX],
        not_regex=[COMMAND_FAILURE_RE],
        label="NetNexus OS FIB withdrew FRR loopback",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "f1"))

    nn_link_ip = str(g_top.r1.GE_1.ip)
    frr_link_ip = str(g_top.f1.GE_1.ip)
    nn_peer_ip = str(g_top.r1.GE_1.peer_ip)

    test_error: BaseException | None = None
    try:
        _cleanup(rt)

        step("Verify GE-1 and eth1 baseline connectivity")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {nn_link_ip}/30",
                    ],
                    "label": "NetNexus GE-1 up",
                },
                {
                    "device": "f1",
                    "command": f"ip -4 addr show dev {FRR_LINUX_IF}",
                    "contains": [f"{frr_link_ip}/30"],
                    "label": "FRR eth1 has topology address",
                },
                {
                    "device": "f1",
                    "command": f"ping -c 1 -W 2 {nn_link_ip}",
                    "contains": ["1 received"],
                    "label": "FRR reaches NetNexus link address",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure point-to-point OSPF on NetNexus and FRR")
        _configure_netnexus(rt)
        _configure_frr(rt)

        step("Wait for OSPF Full adjacency on both implementations")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show ospf neighbor {PROCESS_ID}",
                    "contains": [GE_IF, FRR_ROUTER_ID],
                    "not_contains": ["(no OSPF neighbor)", "(instance not found)"],
                    "regex": [r"(?im)\bFull\b"],
                    "label": "NetNexus sees FRR Full",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show ip ospf neighbor'",
                    "contains": [NN_ROUTER_ID, FRR_LINUX_IF],
                    "regex": [
                        rf"(?im)^\s*{re.escape(NN_ROUTER_ID)}\s+\d+\s+Full/\S+\s+\S+\s+"
                        rf"(?:\S+\s+){{0,2}}{re.escape(nn_link_ip)}\s+"
                        rf"{re.escape(FRR_LINUX_IF)}(?::\S+)?\b"
                    ],
                    "label": "FRR sees NetNexus Full",
                },
            ],
            timeout=90,
            interval=2,
        )
        hold_check(
            rt,
            device="r1",
            command=f"show ospf neighbor {PROCESS_ID}",
            duration=6,
            interval=2,
            contains=[GE_IF, FRR_ROUTER_ID],
            regex=[r"(?im)\bFull\b"],
            label="NetNexus-FRR OSPF adjacency remains Full",
        )

        step("Verify Router-LSAs cross the implementation boundary")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show ospf lsdb {PROCESS_ID}",
                    "contains": [NN_ROUTER_ID, FRR_ROUTER_ID],
                    "not_contains": ["(no OSPF LSA)", "(instance not found)"],
                    "label": "NetNexus LSDB has local and FRR Router-LSAs",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show ip ospf database router'",
                    "contains": [NN_ROUTER_ID, FRR_ROUTER_ID],
                    "label": "FRR LSDB has local and NetNexus Router-LSAs",
                },
            ],
            timeout=90,
            interval=3,
        )

        step("Verify loopback routes are learned in both directions")
        _wait_netnexus_route_present(rt, nexthop=nn_peer_ip)
        wait_check(
            rt,
            device="f1",
            command="vtysh -c 'show ip route ospf'",
            timeout=120,
            interval=3,
            regex=[
                rf"(?im)^\s*O[>*\s]*\s+{re.escape(NN_LOOP_PREFIX)}\s+"
                rf"\[110/\d+\]\s+via\s+{re.escape(nn_link_ip)},\s+"
                rf"{re.escape(FRR_LINUX_IF)}(?:,|\s|$)"
            ],
            label="FRR RIB has NetNexus loopback via OSPF",
        )

        step("Verify loopback-to-loopback forwarding both ways")
        wait_check(
            rt,
            device="r1",
            command=f"ping {FRR_ROUTER_ID} -a {NN_ROUTER_ID}",
            timeout=45,
            interval=2,
            regex=[PING_SUCCESS_RE],
            not_regex=[PING_FAILURE_RE],
            normalize_whitespace=False,
            label="NetNexus loopback reaches FRR loopback",
        )
        wait_check(
            rt,
            device="f1",
            command=f"ping -c 3 -W 2 -I {FRR_ROUTER_ID} {NN_ROUTER_ID}",
            timeout=30,
            interval=2,
            regex=[PING_SUCCESS_RE],
            not_regex=[PING_FAILURE_RE],
            normalize_whitespace=False,
            label="FRR loopback reaches NetNexus loopback",
        )

        step("Disable FRR OSPF on eth1 and verify adjacency and routes withdraw")
        frr_config(
            rt,
            "f1",
            [
                f"interface {FRR_LINUX_IF}",
                "no ip ospf area 0.0.0.0",
                "exit",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show ospf neighbor {PROCESS_ID}",
            timeout=30,
            interval=2,
            not_contains=[FRR_ROUTER_ID],
            not_regex=[COMMAND_FAILURE_RE],
            label="NetNexus removes FRR OSPF neighbor",
        )
        _wait_netnexus_route_absent(rt)
        wait_check(
            rt,
            device="f1",
            command="vtysh -c 'show ip ospf'",
            timeout=30,
            interval=2,
            contains=[FRR_ROUTER_ID],
            not_regex=[COMMAND_FAILURE_RE],
            label="FRR ospfd remains healthy after interface withdrawal",
        )
        wait_check(
            rt,
            device="f1",
            command="vtysh -c 'show ip route ospf'",
            timeout=45,
            interval=2,
            not_contains=[NN_LOOP_PREFIX],
            not_regex=[COMMAND_FAILURE_RE],
            label="FRR withdraws NetNexus OSPF loopback",
        )

        print("NetNexus <-> FRR OSPFv2 point-to-point interop check passed.")
    except BaseException as error:
        test_error = error
        raise
    finally:
        if not should_skip_cleanup():
            _final_cleanup(rt, test_error)
