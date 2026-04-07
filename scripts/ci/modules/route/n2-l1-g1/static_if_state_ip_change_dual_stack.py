#!/usr/bin/env python3
"""
Route static interface-bind behavior check (dual-stack).

Coverage:
- static route with `nexthop + interface` (IPv4/IPv6)
- interface shutdown / no shutdown impact on static route install state
- interface address renumber impact on static route resolve/install state
- OS route table (`show route ... os`) consistency
"""

from __future__ import annotations

import ipaddress
import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"

TARGET4_ADDR = "198.18.66.0"
TARGET4_LEN = 24
TARGET4_PREFIX = f"{TARGET4_ADDR}/{TARGET4_LEN}"

TARGET6_ADDR = "2001:db8:198:18:66::"
TARGET6_LEN = 64
TARGET6_NET_ADDR = str(ipaddress.ip_network(f"{TARGET6_ADDR}/{TARGET6_LEN}", strict=False).network_address)
TARGET6_PREFIX = f"{TARGET6_NET_ADDR}/{TARGET6_LEN}"

NEW_R1_V4 = "10.12.99.1"
NEW_R2_V4 = "10.12.99.2"
NEW_V4_LEN = 30

NEW_R1_V6 = "2001:db8:1299::1"
NEW_R2_V6 = "2001:db8:1299::2"
NEW_V6_LEN = 64


def _wait_rib_static(
    rt: TopologyRuntime,
    *,
    afi: str,
    destination: str,
    nexthop: str,
    expect_present: bool,
    timeout: int = 30,
    interval: int = 2,
) -> None:
    if expect_present:
        wait_check(
            rt,
            device="r1",
            command=f"show route {afi} {destination}",
            timeout=timeout,
            interval=interval,
            contains=[f"Routing entry for {destination}", "Total 1 path(s)"],
            not_contains=["(no routes)", "(no matching routes)"],
            regex=[
                r"(?im)^\s*Path\s*\[1\]\s*:\s*static\b",
                rf"(?im)^\s*Nexthop\s*:\s*{re.escape(nexthop)}\b",
            ],
            label=f"r1 {afi} route {destination} via {nexthop} present",
        )
        return

    wait_check(
        rt,
        device="r1",
        command=f"show route {afi} {destination}",
        timeout=timeout,
        interval=interval,
        regex=[r"(?im)\((?:no routes|no matching routes)\)"],
        label=f"r1 {afi} route {destination} absent",
    )


def _wait_static_candidate(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix: str,
    nexthop: str,
    expect_resolved: bool,
    expect_in_rib: bool,
    timeout: int = 30,
    interval: int = 2,
) -> None:
    resolved = "yes" if expect_resolved else "no"
    in_rib = "yes" if expect_in_rib else "no"
    # Newer `show route <afi> static` includes Interface column:
    # AFI Prefix Nexthop Interface Met Pref Resolved InRIB
    # Keep interface token optional to remain compatible with older output.
    row_regex = (
        rf"(?im)^\s*{re.escape(afi)}\s+{re.escape(prefix)}\s+{re.escape(nexthop)}\s+"
        rf"(?:\S+\s+)?\d+\s+\d+\s+{resolved}\s+{in_rib}\s*$"
    )
    wait_check(
        rt,
        device="r1",
        command=f"show route {afi} static",
        timeout=timeout,
        interval=interval,
        contains=["Total 1 static route(s)"],
        regex=[row_regex],
        label=f"r1 {afi} static candidate {prefix} resolved={resolved} inrib={in_rib}",
    )


def _wait_os_route(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix: str,
    gateway: str,
    expect_present: bool,
    timeout: int = 30,
    interval: int = 2,
) -> None:
    row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+static\s+\d+\s*$"
    )
    wait_check(
        rt,
        device="r1",
        command=f"show route {afi} os",
        timeout=timeout,
        interval=interval,
        regex=[row_regex] if expect_present else (),
        not_regex=[row_regex] if not expect_present else (),
        label=f"r1 {afi} os route {prefix} via {gateway} {'present' if expect_present else 'absent'}",
    )


def _restore_ipv4(
    rt: TopologyRuntime,
    *,
    r1_base_ip: str,
    r1_base_len: int,
    r2_base_ip: str,
    r2_base_len: int,
) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"no ip address {NEW_R1_V4} {NEW_V4_LEN}",
            f"ip address {r1_base_ip} {r1_base_len}",
            "exit",
            f"no route ipv4 {TARGET4_ADDR} {TARGET4_LEN} {r2_base_ip} interface {GE_IF}",
            f"no route ipv4 {TARGET4_ADDR} {TARGET4_LEN} {NEW_R2_V4} interface {GE_IF}",
            f"no route ipv4 {TARGET4_ADDR} {TARGET4_LEN} {r2_base_ip}",
            f"no route ipv4 {TARGET4_ADDR} {TARGET4_LEN} {NEW_R2_V4}",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"no ip address {NEW_R2_V4} {NEW_V4_LEN}",
            f"ip address {r2_base_ip} {r2_base_len}",
            "exit",
            "end",
        ],
    )


def _restore_ipv6(
    rt: TopologyRuntime,
    *,
    r1_base_ip: str,
    r1_base_len: int,
    r2_base_ip: str,
    r2_base_len: int,
) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"no ipv6 address {NEW_R1_V6} {NEW_V6_LEN}",
            f"ipv6 address {r1_base_ip} {r1_base_len}",
            "exit",
            f"no route ipv6 {TARGET6_ADDR} {TARGET6_LEN} {r2_base_ip} interface {GE_IF}",
            f"no route ipv6 {TARGET6_ADDR} {TARGET6_LEN} {NEW_R2_V6} interface {GE_IF}",
            f"no route ipv6 {TARGET6_ADDR} {TARGET6_LEN} {r2_base_ip}",
            f"no route ipv6 {TARGET6_ADDR} {TARGET6_LEN} {NEW_R2_V6}",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"no ipv6 address {NEW_R2_V6} {NEW_V6_LEN}",
            f"ipv6 address {r2_base_ip} {r2_base_len}",
            "exit",
            "end",
        ],
    )


def _run_ipv4(
    rt: TopologyRuntime,
    *,
    r1_base_ip: str,
    r1_base_len: int,
    r2_base_ip: str,
    r2_base_len: int,
) -> None:
    step("IPv4: ensure baseline address and link state")
    _restore_ipv4(rt, r1_base_ip=r1_base_ip, r1_base_len=r1_base_len, r2_base_ip=r2_base_ip, r2_base_len=r2_base_len)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show if {GE_IF}",
                "contains": [
                    f"Interface {GE_IF} Detail:",
                    "State      : UP",
                    f"IPv4 Addr  : {r1_base_ip}/{r1_base_len}",
                ],
                "label": "r1 GE-1 baseline ipv4",
            },
            {
                "device": "r2",
                "command": f"show if {GE_IF}",
                "contains": [
                    f"Interface {GE_IF} Detail:",
                    "State      : UP",
                    f"IPv4 Addr  : {r2_base_ip}/{r2_base_len}",
                ],
                "label": "r2 GE-1 baseline ipv4",
            },
        ],
        timeout=20,
        interval=2,
    )

    step("IPv4: add static route with nexthop + interface and verify install")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"route ipv4 {TARGET4_ADDR} {TARGET4_LEN} {r2_base_ip} interface {GE_IF}",
            "end",
        ],
    )
    _wait_static_candidate(
        rt,
        afi="ipv4",
        prefix=TARGET4_PREFIX,
        nexthop=r2_base_ip,
        expect_resolved=True,
        expect_in_rib=True,
    )
    _wait_rib_static(rt, afi="ipv4", destination=TARGET4_ADDR, nexthop=r2_base_ip, expect_present=True)
    _wait_os_route(rt, afi="ipv4", prefix=TARGET4_PREFIX, gateway=r2_base_ip, expect_present=True)

    step("IPv4: shutdown interface and verify static route withdraw")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            "shutdown",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device="r1",
        command=f"show if {GE_IF}",
        timeout=20,
        interval=2,
        contains=[f"Interface {GE_IF} Detail:", "State      : DOWN"],
        label="r1 GE-1 down (ipv4 phase)",
    )
    _wait_static_candidate(
        rt,
        afi="ipv4",
        prefix=TARGET4_PREFIX,
        nexthop=r2_base_ip,
        expect_resolved=False,
        expect_in_rib=False,
    )
    _wait_rib_static(rt, afi="ipv4", destination=TARGET4_ADDR, nexthop=r2_base_ip, expect_present=False)
    _wait_os_route(rt, afi="ipv4", prefix=TARGET4_PREFIX, gateway=r2_base_ip, expect_present=False)

    step("IPv4: no shutdown interface and verify static route restore")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device="r1",
        command=f"show if {GE_IF}",
        timeout=20,
        interval=2,
        contains=[f"Interface {GE_IF} Detail:", "State      : UP", f"IPv4 Addr  : {r1_base_ip}/{r1_base_len}"],
        label="r1 GE-1 up after no shutdown (ipv4 phase)",
    )
    _wait_static_candidate(
        rt,
        afi="ipv4",
        prefix=TARGET4_PREFIX,
        nexthop=r2_base_ip,
        expect_resolved=True,
        expect_in_rib=True,
    )
    _wait_rib_static(rt, afi="ipv4", destination=TARGET4_ADDR, nexthop=r2_base_ip, expect_present=True)
    _wait_os_route(rt, afi="ipv4", prefix=TARGET4_PREFIX, gateway=r2_base_ip, expect_present=True)

    step("IPv4: renumber GE-1 on r1/r2, verify old nexthop route invalidated")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            f"no ip address {r1_base_ip} {r1_base_len}",
            f"ip address {NEW_R1_V4} {NEW_V4_LEN}",
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
            f"no ip address {r2_base_ip} {r2_base_len}",
            f"ip address {NEW_R2_V4} {NEW_V4_LEN}",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show if {GE_IF}",
                "contains": [
                    f"Interface {GE_IF} Detail:",
                    "State      : UP",
                    f"IPv4 Addr  : {NEW_R1_V4}/{NEW_V4_LEN}",
                ],
                "label": "r1 GE-1 renumbered ipv4",
            },
            {
                "device": "r2",
                "command": f"show if {GE_IF}",
                "contains": [
                    f"Interface {GE_IF} Detail:",
                    "State      : UP",
                    f"IPv4 Addr  : {NEW_R2_V4}/{NEW_V4_LEN}",
                ],
                "label": "r2 GE-1 renumbered ipv4",
            },
        ],
        timeout=20,
        interval=2,
    )
    _wait_static_candidate(
        rt,
        afi="ipv4",
        prefix=TARGET4_PREFIX,
        nexthop=r2_base_ip,
        expect_resolved=False,
        expect_in_rib=False,
    )
    _wait_rib_static(rt, afi="ipv4", destination=TARGET4_ADDR, nexthop=r2_base_ip, expect_present=False)
    _wait_os_route(rt, afi="ipv4", prefix=TARGET4_PREFIX, gateway=r2_base_ip, expect_present=False)

    step("IPv4: update static route nexthop to new peer address and verify recovery")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"no route ipv4 {TARGET4_ADDR} {TARGET4_LEN} {r2_base_ip} interface {GE_IF}",
            f"route ipv4 {TARGET4_ADDR} {TARGET4_LEN} {NEW_R2_V4} interface {GE_IF}",
            "end",
        ],
    )
    _wait_static_candidate(
        rt,
        afi="ipv4",
        prefix=TARGET4_PREFIX,
        nexthop=NEW_R2_V4,
        expect_resolved=True,
        expect_in_rib=True,
    )
    _wait_rib_static(rt, afi="ipv4", destination=TARGET4_ADDR, nexthop=NEW_R2_V4, expect_present=True)
    _wait_os_route(rt, afi="ipv4", prefix=TARGET4_PREFIX, gateway=NEW_R2_V4, expect_present=True)


def _run_ipv6(
    rt: TopologyRuntime,
    *,
    r1_base_ip: str,
    r1_base_len: int,
    r2_base_ip: str,
    r2_base_len: int,
) -> None:
    step("IPv6: ensure baseline address and link state")
    _restore_ipv6(rt, r1_base_ip=r1_base_ip, r1_base_len=r1_base_len, r2_base_ip=r2_base_ip, r2_base_len=r2_base_len)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show if {GE_IF}",
                "contains": [
                    f"Interface {GE_IF} Detail:",
                    "State      : UP",
                    f"IPv6 Addr  : {r1_base_ip}/{r1_base_len}",
                ],
                "label": "r1 GE-1 baseline ipv6",
            },
            {
                "device": "r2",
                "command": f"show if {GE_IF}",
                "contains": [
                    f"Interface {GE_IF} Detail:",
                    "State      : UP",
                    f"IPv6 Addr  : {r2_base_ip}/{r2_base_len}",
                ],
                "label": "r2 GE-1 baseline ipv6",
            },
        ],
        timeout=20,
        interval=2,
    )

    step("IPv6: add static route with nexthop + interface and verify install")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"route ipv6 {TARGET6_ADDR} {TARGET6_LEN} {r2_base_ip} interface {GE_IF}",
            "end",
        ],
    )
    _wait_static_candidate(
        rt,
        afi="ipv6",
        prefix=TARGET6_PREFIX,
        nexthop=r2_base_ip,
        expect_resolved=True,
        expect_in_rib=True,
    )
    _wait_rib_static(rt, afi="ipv6", destination=TARGET6_NET_ADDR, nexthop=r2_base_ip, expect_present=True)
    _wait_os_route(rt, afi="ipv6", prefix=TARGET6_PREFIX, gateway=r2_base_ip, expect_present=True)

    step("IPv6: shutdown interface and verify static route withdraw")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            "shutdown",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device="r1",
        command=f"show if {GE_IF}",
        timeout=20,
        interval=2,
        contains=[f"Interface {GE_IF} Detail:", "State      : DOWN"],
        label="r1 GE-1 down (ipv6 phase)",
    )
    _wait_static_candidate(
        rt,
        afi="ipv6",
        prefix=TARGET6_PREFIX,
        nexthop=r2_base_ip,
        expect_resolved=False,
        expect_in_rib=False,
    )
    _wait_rib_static(rt, afi="ipv6", destination=TARGET6_NET_ADDR, nexthop=r2_base_ip, expect_present=False)
    _wait_os_route(rt, afi="ipv6", prefix=TARGET6_PREFIX, gateway=r2_base_ip, expect_present=False)

    step("IPv6: no shutdown interface and verify static route restore")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device="r1",
        command=f"show if {GE_IF}",
        timeout=20,
        interval=2,
        contains=[f"Interface {GE_IF} Detail:", "State      : UP", f"IPv6 Addr  : {r1_base_ip}/{r1_base_len}"],
        label="r1 GE-1 up after no shutdown (ipv6 phase)",
    )
    _wait_static_candidate(
        rt,
        afi="ipv6",
        prefix=TARGET6_PREFIX,
        nexthop=r2_base_ip,
        expect_resolved=True,
        expect_in_rib=True,
    )
    _wait_rib_static(rt, afi="ipv6", destination=TARGET6_NET_ADDR, nexthop=r2_base_ip, expect_present=True)
    _wait_os_route(rt, afi="ipv6", prefix=TARGET6_PREFIX, gateway=r2_base_ip, expect_present=True)

    step("IPv6: renumber GE-1 on r1/r2, verify old nexthop route invalidated")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            f"no ipv6 address {r1_base_ip} {r1_base_len}",
            f"ipv6 address {NEW_R1_V6} {NEW_V6_LEN}",
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
            f"no ipv6 address {r2_base_ip} {r2_base_len}",
            f"ipv6 address {NEW_R2_V6} {NEW_V6_LEN}",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show if {GE_IF}",
                "contains": [
                    f"Interface {GE_IF} Detail:",
                    "State      : UP",
                    f"IPv6 Addr  : {NEW_R1_V6}/{NEW_V6_LEN}",
                ],
                "label": "r1 GE-1 renumbered ipv6",
            },
            {
                "device": "r2",
                "command": f"show if {GE_IF}",
                "contains": [
                    f"Interface {GE_IF} Detail:",
                    "State      : UP",
                    f"IPv6 Addr  : {NEW_R2_V6}/{NEW_V6_LEN}",
                ],
                "label": "r2 GE-1 renumbered ipv6",
            },
        ],
        timeout=20,
        interval=2,
    )
    _wait_static_candidate(
        rt,
        afi="ipv6",
        prefix=TARGET6_PREFIX,
        nexthop=r2_base_ip,
        expect_resolved=False,
        expect_in_rib=False,
    )
    _wait_rib_static(rt, afi="ipv6", destination=TARGET6_NET_ADDR, nexthop=r2_base_ip, expect_present=False)
    _wait_os_route(rt, afi="ipv6", prefix=TARGET6_PREFIX, gateway=r2_base_ip, expect_present=False)

    step("IPv6: update static route nexthop to new peer address and verify recovery")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"no route ipv6 {TARGET6_ADDR} {TARGET6_LEN} {r2_base_ip} interface {GE_IF}",
            f"route ipv6 {TARGET6_ADDR} {TARGET6_LEN} {NEW_R2_V6} interface {GE_IF}",
            "end",
        ],
    )
    _wait_static_candidate(
        rt,
        afi="ipv6",
        prefix=TARGET6_PREFIX,
        nexthop=NEW_R2_V6,
        expect_resolved=True,
        expect_in_rib=True,
    )
    _wait_rib_static(rt, afi="ipv6", destination=TARGET6_NET_ADDR, nexthop=NEW_R2_V6, expect_present=True)
    _wait_os_route(rt, afi="ipv6", prefix=TARGET6_PREFIX, gateway=NEW_R2_V6, expect_present=True)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_base_v4 = str(ipaddress.ip_address(str(g_top.r1.GE_1.ip)))
    r1_base_v4_len = int(g_top.r1.GE_1.prefix)
    r2_base_v4 = str(ipaddress.ip_address(str(g_top.r2.GE_1.ip)))
    r2_base_v4_len = int(g_top.r2.GE_1.prefix)

    r1_base_v6 = str(ipaddress.ip_address(str(g_top.r1.GE_1.ip6)))
    r1_base_v6_len = int(g_top.r1.GE_1.prefix6)
    r2_base_v6 = str(ipaddress.ip_address(str(g_top.r2.GE_1.ip6)))
    r2_base_v6_len = int(g_top.r2.GE_1.prefix6)

    try:
        step("Pre-clean dual-stack static route interface case")
        _restore_ipv4(
            rt,
            r1_base_ip=r1_base_v4,
            r1_base_len=r1_base_v4_len,
            r2_base_ip=r2_base_v4,
            r2_base_len=r2_base_v4_len,
        )
        _restore_ipv6(
            rt,
            r1_base_ip=r1_base_v6,
            r1_base_len=r1_base_v6_len,
            r2_base_ip=r2_base_v6,
            r2_base_len=r2_base_v6_len,
        )

        _run_ipv4(
            rt,
            r1_base_ip=r1_base_v4,
            r1_base_len=r1_base_v4_len,
            r2_base_ip=r2_base_v4,
            r2_base_len=r2_base_v4_len,
        )
        _run_ipv6(
            rt,
            r1_base_ip=r1_base_v6,
            r1_base_len=r1_base_v6_len,
            r2_base_ip=r2_base_v6,
            r2_base_len=r2_base_v6_len,
        )

        print("Route static interface dual-stack state/ip-change check passed.")
    finally:
        step("Cleanup dual-stack static route interface case")
        _restore_ipv4(
            rt,
            r1_base_ip=r1_base_v4,
            r1_base_len=r1_base_v4_len,
            r2_base_ip=r2_base_v4,
            r2_base_len=r2_base_v4_len,
        )
        _restore_ipv6(
            rt,
            r1_base_ip=r1_base_v6,
            r1_base_len=r1_base_v6_len,
            r2_base_ip=r2_base_v6,
            r2_base_len=r2_base_v6_len,
        )
