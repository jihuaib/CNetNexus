#!/usr/bin/env python3
"""NetNexus/FRR OSPFv3 point-to-point interoperability verification."""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    frr_config,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
    wait_fib_ipv6_route,
)
from top_runner import TopologyRuntime  # noqa: E402


PROCESS_ID = 301
GE_IF = "GE-1"
FRR_IF = "eth1"
NN_ROUTER_ID = "10.255.1.1"
FRR_ROUTER_ID = "10.255.2.2"
NN_LOOP_ID = 63
NN_LOOP = "2001:db8:1::1"
FRR_LOOP = "2001:db8:2::2"
HELLO = 2
DEAD = 8
PING_OK = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
COMMAND_ERROR = r"(?im)(?:unknown command|invalid input|command not found|failed to start module|error:\s)"


def _cleanup(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no ospfv3 {PROCESS_ID}",
            f"no if loop {NN_LOOP_ID}",
            "end",
        ],
    )
    frr_config(
        rt,
        "f1",
        [
            "interface eth1",
            "no ipv6 ospf6 area 0.0.0.0",
            "no ipv6 ospf6 network",
            "no ipv6 ospf6 hello-interval",
            "no ipv6 ospf6 dead-interval",
            "exit",
        ],
        strict=False,
    )
    frr_config(
        rt,
        "f1",
        [
            "interface lo",
            "no ipv6 ospf6 area 0.0.0.0",
            "exit",
        ],
        strict=False,
    )
    frr_config(rt, "f1", ["no router ospf6"], strict=False)
    rt.exec_cmd("f1", f"ip -6 addr del {FRR_LOOP}/128 dev lo", strict=False)


def _configure_netnexus(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if loop {NN_LOOP_ID}",
            f"ipv6 address {NN_LOOP} 128",
            "exit",
            f"ospfv3 {PROCESS_ID}",
            f"router-id {NN_ROUTER_ID}",
            "area 0",
            "exit",
            f"if {GE_IF}",
            f"ospfv3 enable {PROCESS_ID} area 0",
            f"ospfv3 network-type {PROCESS_ID} point-to-point",
            f"ospfv3 hello-interval {PROCESS_ID} {HELLO}",
            f"ospfv3 dead-interval {PROCESS_ID} {DEAD}",
            "exit",
            f"if loop {NN_LOOP_ID}",
            f"ospfv3 enable {PROCESS_ID} area 0",
            f"ospfv3 passive {PROCESS_ID}",
            "exit",
            "end",
        ],
    )


def _configure_frr(rt: TopologyRuntime) -> None:
    rt.exec_cmd("f1", "ip link set dev lo up")
    rt.exec_cmd("f1", f"ip -6 addr replace {FRR_LOOP}/128 dev lo")
    frr_config(
        rt,
        "f1",
        [
            "router ospf6",
            f"ospf6 router-id {FRR_ROUTER_ID}",
            "exit",
            f"interface {FRR_IF}",
            "ipv6 ospf6 area 0.0.0.0",
            "ipv6 ospf6 network point-to-point",
            f"ipv6 ospf6 hello-interval {HELLO}",
            f"ipv6 ospf6 dead-interval {DEAD}",
            "exit",
            "interface lo",
            "ipv6 ospf6 area 0.0.0.0",
            "exit",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "f1"))
    test_error: BaseException | None = None
    try:
        step("Configure NetNexus/FRR OSPFv3 point-to-point adjacency")
        _cleanup(rt)
        _configure_netnexus(rt)
        _configure_frr(rt)

        step("Verify OSPFv3 Full adjacency on both implementations")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show ospfv3 neighbor {PROCESS_ID}",
                    "contains": [FRR_ROUTER_ID, GE_IF, "Full"],
                    "regex": [r"(?i)fe80:[0-9a-f:]+"],
                    "not_regex": [COMMAND_ERROR],
                    "label": "NetNexus sees FRR Full",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show ipv6 ospf6 neighbor'",
                    "contains": [NN_ROUTER_ID, FRR_IF, "Full"],
                    "not_regex": [COMMAND_ERROR],
                    "label": "FRR sees NetNexus Full",
                },
            ],
            timeout=120,
            interval=3,
        )

        step("Verify Router and Intra-Area-Prefix LSAs across implementations")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show ospfv3 lsdb {PROCESS_ID}",
                    "contains": ["Router", "Intra-Prefix", NN_ROUTER_ID, FRR_ROUTER_ID],
                    "not_regex": [COMMAND_ERROR],
                    "label": "NetNexus LSDB contains FRR LSAs",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show ipv6 ospf6 database'",
                    "contains": [NN_ROUTER_ID, FRR_ROUTER_ID],
                    "not_regex": [COMMAND_ERROR],
                    "label": "FRR LSDB contains NetNexus LSAs",
                },
            ],
            timeout=120,
            interval=3,
        )

        step("Verify IPv6 routes and forwarding in both directions")
        wait_check(
            rt,
            device="r1",
            command=f"show ospfv3 route {PROCESS_ID}",
            contains=[f"{FRR_LOOP}/128"],
            regex=[r"(?i)fe80:[0-9a-f:]+"],
            timeout=120,
            interval=3,
            label="NetNexus learns FRR loopback",
        )
        wait_fib_ipv6_route(
            rt,
            device="r1",
            prefix_addr=FRR_LOOP,
            prefix_len=128,
            expect_present=True,
            installed=True,
            skip_os=False,
            timeout=120,
            interval=3,
            label="NetNexus installs FRR loopback",
        )
        wait_check(
            rt,
            device="f1",
            command="vtysh -c 'show ipv6 route ospf6'",
            regex=[rf"(?im)^\s*O[>*\s]*\s+{re.escape(NN_LOOP)}/128\b"],
            not_regex=[COMMAND_ERROR],
            timeout=120,
            interval=3,
            label="FRR learns NetNexus loopback",
        )
        wait_check(
            rt,
            device="r1",
            command=f"ping ipv6 {FRR_LOOP} -a {NN_LOOP}",
            regex=[PING_OK],
            normalize_whitespace=False,
            timeout=60,
            interval=2,
            label="NetNexus reaches FRR loopback",
        )
        wait_check(
            rt,
            device="f1",
            command=f"ping -6 -c 3 -W 2 -I {FRR_LOOP} {NN_LOOP}",
            regex=[PING_OK],
            normalize_whitespace=False,
            timeout=60,
            interval=2,
            label="FRR reaches NetNexus loopback",
        )
        print("NetNexus/FRR OSPFv3 interoperability verification passed.")
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
