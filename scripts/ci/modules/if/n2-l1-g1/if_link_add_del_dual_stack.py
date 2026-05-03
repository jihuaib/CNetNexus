#!/usr/bin/env python3
"""
IF link add/remove validation (dual-stack).

Goals:
- remove/add topology link via CI runtime generic helpers
- 在 stub 补位模型下，remove_link 不会让接口 DOWN（因为立刻挂上 stub），
  但会切断对端可达性。add_link 再把 stub 换回 link，对端恢复可达。
- 断言关注点：ping IPv4/IPv6 连通性的 down/up 切换；
  接口状态、直连路由在整个过程中保持 UP/present（指向新的 ifindex）。
"""

from __future__ import annotations

import ipaddress
import re

from module_api import add_link, g_top, remove_link, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"
TEST_V6_R1 = "2001:db8:12::1"
TEST_V6_R2 = "2001:db8:12::2"
TEST_V6_PREFIX = 64

PING_SUCCESS_RE = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
PING_FAIL_RE = (
    r"(?im)(?:\b100(?:\.0)?%\s+packet loss\b|network is unreachable|"
    r"destination host unreachable|no route to host|connect:|"
    r"module timed out or failed to respond)"
)


def _wait_os_route(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    prefix: str,
    table: str,
    route_type: str,
    proto: str,
    gateway: str,
    expect_present: bool,
    timeout: int = 30,
    interval: int = 2,
) -> None:
    row_regex = (
        rf"(?im)^\s*{re.escape(table)}\s+{re.escape(route_type)}\s+"
        rf"{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(proto)}\s+\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        regex=[row_regex] if expect_present else (),
        not_regex=[row_regex] if not expect_present else (),
        label=(
            f"{device} {command} route {prefix} "
            f"{'present' if expect_present else 'absent'} "
            f"(table={table} type={route_type} proto={proto} gw={gateway})"
        ),
    )


def _wait_rib_route(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    route_addr: str,
    route_len: int,
    prefix: str,
    nexthop: str,
    interface: str,
    expect_present: bool,
    timeout: int = 30,
    interval: int = 2,
) -> None:
    command = f"show route {afi} {route_addr} {route_len}"
    if expect_present:
        wait_check(
            rt,
            device=device,
            command=command,
            timeout=timeout,
            interval=interval,
            contains=[f"Routing entry for {prefix}", "Total 1 path(s)"],
            not_contains=["(no routes)", "(no matching routes)"],
            regex=[
                r"(?im)^\s*Path\s*\[1\]\s*:\s*connected\b",
                rf"(?im)^\s*Nexthop\s*:\s*{re.escape(nexthop)}\s*$",
                rf"(?im)^\s*Interface\s*:\s*{re.escape(interface)}\s*$",
            ],
            label=f"{device} {afi} rib route {prefix} via {interface} present",
        )
        wait_fib_route(
            rt,
            device=device,
            afi=afi,
            prefix_addr=route_addr,
            prefix_len=route_len,
            expect_present=True,
            skip_os=True,
            timeout=timeout,
            interval=interval,
            label=f"{device} {afi} fib route {prefix} present",
        )
        return

    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        contains=[f"Routing entry for {prefix}"],
        regex=[r"(?im)\((?:no routes|no matching routes)\)"],
        label=f"{device} {afi} rib route {prefix} absent",
    )
    wait_fib_route(
        rt,
        device=device,
        afi=afi,
        prefix_addr=route_addr,
        prefix_len=route_len,
        expect_present=False,
        timeout=timeout,
        interval=interval,
        label=f"{device} {afi} fib route {prefix} absent",
    )


def _wait_ping(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    expect_success: bool,
    timeout: int = 45,
    interval: int = 2,
) -> None:
    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        regex=[PING_SUCCESS_RE] if expect_success else [PING_FAIL_RE],
        not_regex=[PING_FAIL_RE] if expect_success else [PING_SUCCESS_RE],
        normalize_whitespace=False,
        label=f"{device} {command} {'success' if expect_success else 'fail'}",
    )


def _wait_if_state(
    rt: TopologyRuntime,
    *,
    if_name: str,
    expect_proto: str,
    expect_link: str,
    timeout: int = 25,
    interval: int = 2,
) -> None:
    proto_re = rf"(?im)^\s*Proto State\s*:\s*{re.escape(expect_proto)}\s*$"
    link_re = rf"(?im)^\s*Link State\s*:\s*{re.escape(expect_link)}\s*$"
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show if {if_name}",
                "contains": [f"Interface {if_name} Detail:"],
                "regex": [proto_re, link_re],
                "label": f"r1 {if_name} proto={expect_proto} link={expect_link}",
            },
            {
                "device": "r2",
                "command": f"show if {if_name}",
                "contains": [f"Interface {if_name} Detail:"],
                "regex": [proto_re, link_re],
                "label": f"r2 {if_name} proto={expect_proto} link={expect_link}",
            },
        ],
        timeout=timeout,
        interval=interval,
    )


def _configure_ipv6(rt: TopologyRuntime, *, if_name: str, r1_v6: str, r2_v6: str, prefix: int) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {if_name}",
            f"ipv6 address {r1_v6} {prefix}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"if {if_name}",
            f"ipv6 address {r2_v6} {prefix}",
            "no shutdown",
            "exit",
            "end",
        ],
    )


def _cleanup(rt: TopologyRuntime, *, link_name: str, if_name: str, r1_v6: str, r2_v6: str, prefix: int) -> None:
    add_link(rt, link_name, strict=False)
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"if {if_name}",
            "no shutdown",
            f"no ipv6 address {r1_v6} {prefix}",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"if {if_name}",
            "no shutdown",
            f"no ipv6 address {r2_v6} {prefix}",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    link_name = str(g_top.r1.GE_1.link_name)
    if not link_name:
        raise ValueError("missing link_name for r1 GE-1 in topology view")

    r1_v4 = str(g_top.r1.GE_1.ip)
    r1_v4_prefix = int(g_top.r1.GE_1.prefix)
    r2_v4 = str(g_top.r1.GE_1.peer_ip)
    r1_v4_net = str(ipaddress.ip_interface(f"{r1_v4}/{r1_v4_prefix}").network.network_address)
    r1_v4_connected = f"{r1_v4_net}/{r1_v4_prefix}"
    r1_v4_local = f"{r1_v4}/32"

    r1_v6 = str(ipaddress.ip_address(TEST_V6_R1))
    r2_v6 = str(ipaddress.ip_address(TEST_V6_R2))
    r1_v6_net = str(ipaddress.ip_interface(f"{r1_v6}/{TEST_V6_PREFIX}").network.network_address)
    r1_v6_connected = f"{r1_v6_net}/{TEST_V6_PREFIX}"
    r1_v6_local = f"{r1_v6}/128"

    try:
        step("Prepare baseline: link connected and interface up")
        add_link(rt, link_name, strict=False)
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", f"if {GE_IF}", "no shutdown", "exit", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"if {GE_IF}", "no shutdown", "exit", "end"],
        )
        _wait_if_state(rt, if_name=GE_IF, expect_proto="UP", expect_link="UP", timeout=20, interval=2)

        step("Prepare baseline: clear stale IPv6 config on GE-1")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", f"if {GE_IF}", f"no ipv6 address {r1_v6} {TEST_V6_PREFIX}", "exit", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"if {GE_IF}", f"no ipv6 address {r2_v6} {TEST_V6_PREFIX}", "exit", "end"],
        )

        step("Configure IPv6 underlay addresses on GE-1")
        _configure_ipv6(rt, if_name=GE_IF, r1_v6=r1_v6, r2_v6=r2_v6, prefix=TEST_V6_PREFIX)
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [f"IPv6 Addr  : {r1_v6}/{TEST_V6_PREFIX}"],
                    "label": "r1 GE-1 ipv6 configured",
                },
                {
                    "device": "r2",
                    "command": f"show if {GE_IF}",
                    "contains": [f"IPv6 Addr  : {r2_v6}/{TEST_V6_PREFIX}"],
                    "label": "r2 GE-1 ipv6 configured",
                },
            ],
            timeout=20,
            interval=2,
        )

        step("Baseline check: route RIB/OS has IPv4/IPv6 connected and local entries")
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv4",
            route_addr=r1_v4_net,
            route_len=r1_v4_prefix,
            prefix=r1_v4_connected,
            nexthop="0.0.0.0",
            interface=GE_IF,
            expect_present=True,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv4",
            route_addr=r1_v4,
            route_len=32,
            prefix=r1_v4_local,
            nexthop="0.0.0.0",
            interface=GE_IF,
            expect_present=True,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv6",
            route_addr=r1_v6_net,
            route_len=TEST_V6_PREFIX,
            prefix=r1_v6_connected,
            nexthop="::",
            interface=GE_IF,
            expect_present=True,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv6",
            route_addr=r1_v6,
            route_len=128,
            prefix=r1_v6_local,
            nexthop="::",
            interface=GE_IF,
            expect_present=True,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv4 os",
            prefix=r1_v4_connected,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv4 os",
            prefix=r1_v4_local,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv6 os",
            prefix=r1_v6_connected,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv6 os",
            prefix=r1_v6_local,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
        )

        step("Baseline check: ping IPv4/IPv6 reachable")
        _wait_ping(rt, device="r1", command=f"ping {r2_v4}", expect_success=True)
        _wait_ping(rt, device="r1", command=f"ping ipv6 {r2_v6}", expect_success=True)

        step("Remove link: stub 替换后接口仍 UP，直连路由仍在，但对端不可达")
        remove_link(rt, link_name)
        # stub 立刻占位，稳态接口保持 UP（carrier 来自 stub 桥），RIB/OS 直连路由仍指向新 ifindex
        _wait_if_state(rt, if_name=GE_IF, expect_proto="UP", expect_link="UP", timeout=30, interval=2)
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv4",
            route_addr=r1_v4_net,
            route_len=r1_v4_prefix,
            prefix=r1_v4_connected,
            nexthop="0.0.0.0",
            interface=GE_IF,
            expect_present=True,
            timeout=40,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv4",
            route_addr=r1_v4,
            route_len=32,
            prefix=r1_v4_local,
            nexthop="0.0.0.0",
            interface=GE_IF,
            expect_present=True,
            timeout=40,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv6",
            route_addr=r1_v6_net,
            route_len=TEST_V6_PREFIX,
            prefix=r1_v6_connected,
            nexthop="::",
            interface=GE_IF,
            expect_present=True,
            timeout=40,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv6",
            route_addr=r1_v6,
            route_len=128,
            prefix=r1_v6_local,
            nexthop="::",
            interface=GE_IF,
            expect_present=True,
            timeout=40,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv4 os",
            prefix=r1_v4_connected,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=40,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv4 os",
            prefix=r1_v4_local,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=40,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv6 os",
            prefix=r1_v6_connected,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=40,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv6 os",
            prefix=r1_v6_local,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=40,
        )
        _wait_ping(rt, device="r1", command=f"ping {r2_v4}", expect_success=False)
        _wait_ping(rt, device="r1", command=f"ping ipv6 {r2_v6}", expect_success=False)

        step("Add link back and verify route RIB/OS restore + ping success")
        add_link(rt, link_name)
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", f"if {GE_IF}", "no shutdown", "exit", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"if {GE_IF}", "no shutdown", "exit", "end"],
        )
        _wait_if_state(rt, if_name=GE_IF, expect_proto="UP", expect_link="UP", timeout=20, interval=2)
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv4",
            route_addr=r1_v4_net,
            route_len=r1_v4_prefix,
            prefix=r1_v4_connected,
            nexthop="0.0.0.0",
            interface=GE_IF,
            expect_present=True,
            timeout=40,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv4",
            route_addr=r1_v4,
            route_len=32,
            prefix=r1_v4_local,
            nexthop="0.0.0.0",
            interface=GE_IF,
            expect_present=True,
            timeout=40,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv6",
            route_addr=r1_v6_net,
            route_len=TEST_V6_PREFIX,
            prefix=r1_v6_connected,
            nexthop="::",
            interface=GE_IF,
            expect_present=True,
            timeout=40,
        )
        _wait_rib_route(
            rt,
            device="r1",
            afi="ipv6",
            route_addr=r1_v6,
            route_len=128,
            prefix=r1_v6_local,
            nexthop="::",
            interface=GE_IF,
            expect_present=True,
            timeout=40,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv4 os",
            prefix=r1_v4_connected,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=40,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv4 os",
            prefix=r1_v4_local,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=40,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv6 os",
            prefix=r1_v6_connected,
            table="main",
            route_type="unicast",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=40,
        )
        _wait_os_route(
            rt,
            device="r1",
            command="show fib ipv6 os",
            prefix=r1_v6_local,
            table="local",
            route_type="local",
            proto="kernel",
            gateway="-",
            expect_present=True,
            timeout=40,
        )
        _wait_ping(rt, device="r1", command=f"ping {r2_v4}", expect_success=True, timeout=60)
        _wait_ping(rt, device="r1", command=f"ping ipv6 {r2_v6}", expect_success=True, timeout=60)
    finally:
        step("Cleanup: restore link and remove test IPv6 config")
        _cleanup(rt, link_name=link_name, if_name=GE_IF, r1_v6=r1_v6, r2_v6=r2_v6, prefix=TEST_V6_PREFIX)

    print("IF link add/remove dual-stack check passed.")
