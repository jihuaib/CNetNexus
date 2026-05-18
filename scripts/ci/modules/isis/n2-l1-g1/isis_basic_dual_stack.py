#!/usr/bin/env python3
"""
ISIS basic dual-stack scenario check.

Coverage:
- ISIS instance/AF bring-up on two routers
- ISIS LAN neighbor negotiation on GE-1
- IPv4/IPv6 prefix learning via ISIS (loopback prefixes)
- Route module RIB and OS route programming verification
- AF-specific interface show filtering sanity check
"""

from __future__ import annotations

import ipaddress
import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TAG = 100
GE_IF = "GE-1"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"

R1_LOOP_ID = 11
R2_LOOP_ID = 22

R1_LOOP_V4 = "10.255.1.1"
R1_LOOP_V4_LEN = 32
R2_LOOP_V4 = "10.255.2.2"
R2_LOOP_V4_LEN = 32

R1_LOOP_V6 = "2001:db8:255:1::1"
R1_LOOP_V6_LEN = 128
R2_LOOP_V6 = "2001:db8:255:2::2"
R2_LOOP_V6_LEN = 128

R1_LOOP_V4_PREFIX = f"{R1_LOOP_V4}/{R1_LOOP_V4_LEN}"
R2_LOOP_V4_PREFIX = f"{R2_LOOP_V4}/{R2_LOOP_V4_LEN}"
R1_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R1_LOOP_V6}/{R1_LOOP_V6_LEN}", strict=False))
R2_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R2_LOOP_V6}/{R2_LOOP_V6_LEN}", strict=False))


def _cleanup_case_config(rt: TopologyRuntime) -> None:
    step("Cleanup stale ISIS/loopback config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no isis {TAG}",
            f"no if loop {R1_LOOP_ID}",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "end",
            "config",
            f"no isis {TAG}",
            f"no if loop {R2_LOOP_ID}",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )


def _wait_route_in_rib(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    prefix: str,
    timeout: int = 90,
) -> None:
    prefix_len = prefix.split("/", 1)[1] if "/" in prefix else ("32" if afi == "ipv4" else "128")
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


def _wait_isis_route_detail(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    mask: int,
    prefix: str,
    timeout: int = 90,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show isis route {afi} {TAG} {destination} {mask}",
        timeout=timeout,
        interval=2,
        contains=[
            f"ISIS {afi} Routes Detail (tag {TAG}, {prefix})",
            f"Prefix       : {prefix}",
        ],
        not_contains=["(no matching routes)", "(instance not found)"],
        regex=[r"(?im)^\s*Route\s+\d+\s*$"],
        label=f"{device} show isis route {afi} detail {prefix}",
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

        step("Configure loopback prefixes for ISIS advertisement")
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
                    "contains": [
                        f"Interface loop{R1_LOOP_ID} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {R1_LOOP_V4}/{R1_LOOP_V4_LEN}",
                        f"IPv6 Addr  : {R1_LOOP_V6}/{R1_LOOP_V6_LEN}",
                    ],
                    "label": "r1 loop up",
                },
                {
                    "device": "r2",
                    "command": f"show if loop {R2_LOOP_ID}",
                    "contains": [
                        f"Interface loop{R2_LOOP_ID} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {R2_LOOP_V4}/{R2_LOOP_V4_LEN}",
                        f"IPv6 Addr  : {R2_LOOP_V6}/{R2_LOOP_V6_LEN}",
                    ],
                    "label": "r2 loop up",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure ISIS instance and AF")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"isis {TAG}",
                f"net {R1_NET}",
                "is-type level-1-2",
                "cost-style wide",
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
                "cost-style wide",
                "af ipv4",
                "af ipv6",
                "end",
            ],
        )

        step("Enable ISIS on GE-1 and loopback (loopback passive)")
        run_cmds(
            rt=rt,
            device="r1",
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
                f"if loop {R1_LOOP_ID}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis passive {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
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
                f"if loop {R2_LOOP_ID}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis passive {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )

        step("Wait ISIS neighbor negotiation (interface-based)")
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

        step("Check ISIS LSDB entries (IPv4/IPv6 views)")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis lsdb ipv4 {TAG}",
                    "contains": ["ISIS LSDB", GE_IF],
                    "not_contains": ["(no entries)"],
                    "regex": [
                        r"(?im)^\s*LSP Entry\s+\d+\s*$",
                        rf"(?im)^\s*Tag\s*:\s*{TAG}\s*$",
                        rf"(?im)^\s*Rx-If\s*:\s*{re.escape(GE_IF)}\s*$",
                        r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                        r"(?im)^\s*TLV\[\d+\]\s*:\s*type=135\b",
                    ],
                    "label": "r1 lsdb ipv4 has remote lsp",
                },
                {
                    "device": "r1",
                    "command": f"show isis lsdb ipv6 {TAG}",
                    "contains": ["ISIS LSDB", GE_IF],
                    "not_contains": ["(no entries)"],
                    "regex": [
                        r"(?im)^\s*LSP Entry\s+\d+\s*$",
                        rf"(?im)^\s*Tag\s*:\s*{TAG}\s*$",
                        rf"(?im)^\s*Rx-If\s*:\s*{re.escape(GE_IF)}\s*$",
                        r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                        r"(?im)^\s*TLV\[\d+\]\s*:\s*type=236\b",
                    ],
                    "label": "r1 lsdb ipv6 has remote lsp",
                },
                {
                    "device": "r2",
                    "command": f"show isis lsdb ipv4 {TAG}",
                    "contains": ["ISIS LSDB", GE_IF],
                    "not_contains": ["(no entries)"],
                    "regex": [
                        r"(?im)^\s*LSP Entry\s+\d+\s*$",
                        rf"(?im)^\s*Tag\s*:\s*{TAG}\s*$",
                        rf"(?im)^\s*Rx-If\s*:\s*{re.escape(GE_IF)}\s*$",
                        r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                        r"(?im)^\s*TLV\[\d+\]\s*:\s*type=135\b",
                    ],
                    "label": "r2 lsdb ipv4 has remote lsp",
                },
                {
                    "device": "r2",
                    "command": f"show isis lsdb ipv6 {TAG}",
                    "contains": ["ISIS LSDB", GE_IF],
                    "not_contains": ["(no entries)"],
                    "regex": [
                        r"(?im)^\s*LSP Entry\s+\d+\s*$",
                        rf"(?im)^\s*Tag\s*:\s*{TAG}\s*$",
                        rf"(?im)^\s*Rx-If\s*:\s*{re.escape(GE_IF)}\s*$",
                        r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                        r"(?im)^\s*TLV\[\d+\]\s*:\s*type=236\b",
                    ],
                    "label": "r2 lsdb ipv6 has remote lsp",
                },
            ],
            timeout=80,
            interval=2,
        )

        step("Wait ISIS learned routes in Route RIB")
        _wait_route_in_rib(rt, device="r1", afi="ipv4", destination=R2_LOOP_V4, prefix=R2_LOOP_V4_PREFIX)
        _wait_route_in_rib(rt, device="r1", afi="ipv6", destination=R2_LOOP_V6, prefix=R2_LOOP_V6_PREFIX)
        _wait_route_in_rib(rt, device="r2", afi="ipv4", destination=R1_LOOP_V4, prefix=R1_LOOP_V4_PREFIX)
        _wait_route_in_rib(rt, device="r2", afi="ipv6", destination=R1_LOOP_V6, prefix=R1_LOOP_V6_PREFIX)

        step("Verify ISIS route show list/detail")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis route ipv4 {TAG}",
                    "contains": [f"ISIS ipv4 Routes (tag {TAG})", R2_LOOP_V4_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r1 show isis route ipv4 list",
                },
                {
                    "device": "r2",
                    "command": f"show isis route ipv4 {TAG}",
                    "contains": [f"ISIS ipv4 Routes (tag {TAG})", R1_LOOP_V4_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r2 show isis route ipv4 list",
                },
                {
                    "device": "r1",
                    "command": f"show isis route ipv6 {TAG}",
                    "contains": [f"ISIS ipv6 Routes (tag {TAG})", R2_LOOP_V6_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r1 show isis route ipv6 list",
                },
                {
                    "device": "r2",
                    "command": f"show isis route ipv6 {TAG}",
                    "contains": [f"ISIS ipv6 Routes (tag {TAG})", R1_LOOP_V6_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r2 show isis route ipv6 list",
                },
            ],
            timeout=80,
            interval=2,
        )
        _wait_isis_route_detail(
            rt,
            device="r1",
            afi="ipv4",
            destination=R2_LOOP_V4,
            mask=R2_LOOP_V4_LEN,
            prefix=R2_LOOP_V4_PREFIX,
        )
        _wait_isis_route_detail(
            rt,
            device="r1",
            afi="ipv6",
            destination=R2_LOOP_V6,
            mask=R2_LOOP_V6_LEN,
            prefix=R2_LOOP_V6_PREFIX,
        )
        _wait_isis_route_detail(
            rt,
            device="r2",
            afi="ipv4",
            destination=R1_LOOP_V4,
            mask=R1_LOOP_V4_LEN,
            prefix=R1_LOOP_V4_PREFIX,
        )
        _wait_isis_route_detail(
            rt,
            device="r2",
            afi="ipv6",
            destination=R1_LOOP_V6,
            mask=R1_LOOP_V6_LEN,
            prefix=R1_LOOP_V6_PREFIX,
        )

        step("Verify ISIS routes appear in protocol views")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show route ipv4 proto isis",
                    "contains": [R2_LOOP_V4_PREFIX],
                    "label": "r1 ipv4 proto isis contains r2 loop",
                },
                {
                    "device": "r1",
                    "command": "show route ipv6 proto isis",
                    "contains": [R2_LOOP_V6_PREFIX],
                    "label": "r1 ipv6 proto isis contains r2 loop",
                },
                {
                    "device": "r2",
                    "command": "show route ipv4 proto isis",
                    "contains": [R1_LOOP_V4_PREFIX],
                    "label": "r2 ipv4 proto isis contains r1 loop",
                },
                {
                    "device": "r2",
                    "command": "show route ipv6 proto isis",
                    "contains": [R1_LOOP_V6_PREFIX],
                    "label": "r2 ipv6 proto isis contains r1 loop",
                },
            ],
            timeout=80,
            interval=2,
        )

        step("Verify ISIS routes programmed to OS FIB view")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show fib ipv4 os",
                    "contains": [R2_LOOP_V4_PREFIX],
                    "label": "r1 os ipv4 has r2 loop",
                },
                {
                    "device": "r1",
                    "command": "show fib ipv6 os",
                    "contains": [R2_LOOP_V6_PREFIX],
                    "label": "r1 os ipv6 has r2 loop",
                },
                {
                    "device": "r2",
                    "command": "show fib ipv4 os",
                    "contains": [R1_LOOP_V4_PREFIX],
                    "label": "r2 os ipv4 has r1 loop",
                },
                {
                    "device": "r2",
                    "command": "show fib ipv6 os",
                    "contains": [R1_LOOP_V6_PREFIX],
                    "label": "r2 os ipv6 has r1 loop",
                },
            ],
            timeout=80,
            interval=2,
        )

        step("Check AF-specific ISIS interface displays")
        wait_check(
            rt,
            device="r1",
            command=f"show isis interface ipv4 {TAG}",
            timeout=20,
            interval=2,
            contains=[f"Tag {TAG}", GE_IF, f"loop{R1_LOOP_ID}", "ipv4"],
            not_regex=[r"(?im)\bipv6\b"],
            label="r1 show isis interface ipv4 filtered",
        )
        wait_check(
            rt,
            device="r1",
            command=f"show isis interface ipv6 {TAG}",
            timeout=20,
            interval=2,
            contains=[f"Tag {TAG}", GE_IF, f"loop{R1_LOOP_ID}", "ipv6"],
            not_regex=[r"(?im)\bipv4\b"],
            label="r1 show isis interface ipv6 filtered",
        )

        print("ISIS basic dual-stack check passed.")
    finally:
        _cleanup_case_config(rt)
