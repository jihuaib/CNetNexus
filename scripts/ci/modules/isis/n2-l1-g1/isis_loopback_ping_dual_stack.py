#!/usr/bin/env python3
"""
ISIS loopback forwarding check (dual-stack).

Coverage:
- advertise IPv4/IPv6 loopback host routes through ISIS
- verify remote loopback routes in Route RIB and FIB
- verify forwarding with sourced IPv4/IPv6 ping between loopbacks
"""

from __future__ import annotations

import ipaddress
import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TAG = 110
GE_IF = "GE-1"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"

R1_LOOP_ID = 11
R2_LOOP_ID = 22

R1_LOOP_V4 = "10.255.11.11"
R1_LOOP_V4_LEN = 32
R2_LOOP_V4 = "10.255.22.22"
R2_LOOP_V4_LEN = 32

R1_LOOP_V6 = "2001:db8:255:11::11"
R1_LOOP_V6_LEN = 128
R2_LOOP_V6 = "2001:db8:255:22::22"
R2_LOOP_V6_LEN = 128

R1_LOOP_V4_PREFIX = f"{R1_LOOP_V4}/{R1_LOOP_V4_LEN}"
R2_LOOP_V4_PREFIX = f"{R2_LOOP_V4}/{R2_LOOP_V4_LEN}"
R1_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R1_LOOP_V6}/{R1_LOOP_V6_LEN}", strict=False))
R2_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R2_LOOP_V6}/{R2_LOOP_V6_LEN}", strict=False))

PING_SUCCESS_RE = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
PING_FAIL_RE = (
    r"(?im)(?:\b100(?:\.0)?%\s+packet loss\b|network is unreachable|"
    r"destination host unreachable|no route to host|connect:|"
    r"module timed out or failed to respond|failed to start ping)"
)


def _cleanup_case_config(rt: TopologyRuntime) -> None:
    step("Cleanup stale ISIS/loopback config")
    for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=[
                "end",
                "config",
                f"no isis {TAG}",
                f"no if loop {loop_id}",
                f"if {GE_IF}",
                "no shutdown",
                "exit",
                "end",
            ],
        )


def _wait_route_in_rib_and_fib(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    prefix: str,
    timeout: int = 90,
) -> None:
    prefix_len = prefix.split("/", 1)[1]
    wait_check(
        rt,
        device=device,
        command=f"show route {afi} {destination} {prefix_len}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {prefix}"],
        not_contains=["(no routes)", "(no matching routes)"],
        regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*isis\b"],
        label=f"{device} {afi} {prefix} learned via ISIS",
    )
    wait_fib_route(
        rt,
        device=device,
        afi=afi,
        prefix_addr=destination,
        prefix_len=prefix_len,
        expect_present=True,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} {afi} fib route {prefix} learned via ISIS",
    )


def _wait_ping_success(rt: TopologyRuntime, *, device: str, command: str, timeout: int = 60) -> None:
    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=2,
        regex=[PING_SUCCESS_RE],
        not_regex=[PING_FAIL_RE],
        normalize_whitespace=False,
        label=f"{device} {command} success",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_ip4 = str(g_top.r1.GE_1.ip)
    r1_ip6 = str(g_top.r1.GE_1.ip6)
    r1_peer_ip4 = str(g_top.r1.GE_1.peer_ip)
    r2_ip4 = str(g_top.r2.GE_1.ip)
    r2_ip6 = str(g_top.r2.GE_1.ip6)
    r2_peer_ip4 = str(g_top.r2.GE_1.peer_ip)

    try:
        _cleanup_case_config(rt)

        step("Ensure GE-1 baseline state")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r1_ip4}/30",
                        f"IPv6 Addr  : {r1_ip6}/64",
                    ],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "r2",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r2_ip4}/30",
                        f"IPv6 Addr  : {r2_ip6}/64",
                    ],
                    "label": "r2 GE-1 up",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure IPv4/IPv6 loopbacks")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {R1_LOOP_ID}",
                f"ip address {R1_LOOP_V4} {R1_LOOP_V4_LEN}",
                f"ipv6 address {R1_LOOP_V6} {R1_LOOP_V6_LEN}",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"if loop {R2_LOOP_ID}",
                f"ip address {R2_LOOP_V4} {R2_LOOP_V4_LEN}",
                f"ipv6 address {R2_LOOP_V6} {R2_LOOP_V6_LEN}",
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
                    "contains": [R1_LOOP_V4_PREFIX, R1_LOOP_V6_PREFIX],
                    "label": "r1 loopback addresses installed",
                },
                {
                    "device": "r2",
                    "command": f"show if loop {R2_LOOP_ID}",
                    "contains": [R2_LOOP_V4_PREFIX, R2_LOOP_V6_PREFIX],
                    "label": "r2 loopback addresses installed",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure ISIS dual-stack instance")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"isis {TAG}",
                f"net {R1_NET}",
                "is-type level-1-2",
                "af ipv4",
                "af ipv6",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"isis {TAG}",
                f"net {R2_NET}",
                "is-type level-1-2",
                "af ipv4",
                "af ipv6",
                "end",
            ],
        )

        step("Enable ISIS on transit interface and passive loopbacks")
        for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
            run_cmds(
                rt=rt,
                device=device,
                commands=[
                    "config",
                    f"if {GE_IF}",
                    f"isis enable {TAG}",
                    f"isis ipv6 enable {TAG}",
                    f"isis hello-interval {TAG} 3",
                    f"isis ipv6 hello-interval {TAG} 3",
                    f"isis hold-multiplier {TAG} 3",
                    f"isis ipv6 hold-multiplier {TAG} 3",
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

        step("Wait ISIS adjacency")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis neighbor {TAG}",
                    "contains": ["ISIS Neighbors", GE_IF, r1_peer_ip4],
                    "regex": [
                        rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\s+yes\s+yes\s+\d+\s+\d+\s+"
                        rf"{re.escape(r1_peer_ip4)}\s+fe80:[0-9a-f:]+\s*$"
                    ],
                    "label": "r1 ISIS neighbor up",
                },
                {
                    "device": "r2",
                    "command": f"show isis neighbor {TAG}",
                    "contains": ["ISIS Neighbors", GE_IF, r2_peer_ip4],
                    "regex": [
                        rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\s+yes\s+yes\s+\d+\s+\d+\s+"
                        rf"{re.escape(r2_peer_ip4)}\s+fe80:[0-9a-f:]+\s*$"
                    ],
                    "label": "r2 ISIS neighbor up",
                },
            ],
            timeout=80,
            interval=2,
        )

        step("Wait remote loopback routes in Route RIB and FIB")
        _wait_route_in_rib_and_fib(rt, device="r1", afi="ipv4", destination=R2_LOOP_V4, prefix=R2_LOOP_V4_PREFIX)
        _wait_route_in_rib_and_fib(rt, device="r1", afi="ipv6", destination=R2_LOOP_V6, prefix=R2_LOOP_V6_PREFIX)
        _wait_route_in_rib_and_fib(rt, device="r2", afi="ipv4", destination=R1_LOOP_V4, prefix=R1_LOOP_V4_PREFIX)
        _wait_route_in_rib_and_fib(rt, device="r2", afi="ipv6", destination=R1_LOOP_V6, prefix=R1_LOOP_V6_PREFIX)

        step("Verify loopback-to-loopback forwarding with IPv4/IPv6 ping")
        _wait_ping_success(rt, device="r1", command=f"ping {R2_LOOP_V4} -a {R1_LOOP_V4}")
        _wait_ping_success(rt, device="r2", command=f"ping {R1_LOOP_V4} -a {R2_LOOP_V4}")
        _wait_ping_success(rt, device="r1", command=f"ping ipv6 {R2_LOOP_V6} -a {R1_LOOP_V6}")
        _wait_ping_success(rt, device="r2", command=f"ping ipv6 {R1_LOOP_V6} -a {R2_LOOP_V6}")

        print("ISIS loopback dual-stack ping check passed.")
    finally:
        _cleanup_case_config(rt)
