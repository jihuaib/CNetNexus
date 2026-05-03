#!/usr/bin/env python3
"""
Route calc best-path switch check (IPv6 static metric based).

Goal:
- install two static IPv6 paths for the same prefix (both reachable)
- verify route path metrics are updated as configured
- verify OS route stays installable via resolved reachable gateway (kernel metric remains 0)
- verify withdraw/re-add keeps route availability consistent
"""

from __future__ import annotations

import ipaddress
import re
import time
from typing import Optional

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_ipv6_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"

TARGET_PREFIX_ADDR = "2001:db8:203:100::"
TARGET_PREFIX_LEN = 64
TARGET_PREFIX = f"{TARGET_PREFIX_ADDR}/{TARGET_PREFIX_LEN}"
RESOLVER_ADDR = "2001:db8:198:51::1"
RESOLVER_PREFIX_LEN = 128


def _wait_path_total(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    expect_total: int,
    timeout: int,
    interval: int = 2,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv6 {destination} {TARGET_PREFIX_LEN}",
        timeout=timeout,
        interval=interval,
        contains=[f"Total {expect_total} path(s)"],
        label=f"{device} path-total {destination}={expect_total}",
    )


def _wait_route_metrics(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    expect_metrics: dict[str, int],
    absent_nhs: Optional[list[str]] = None,
    timeout: int,
    interval: int = 2,
) -> None:
    absent_nhs = absent_nhs or []
    metric_regex = [
        rf"(?is)Nexthop\s*:\s*{re.escape(nh)}\b.*?Metric\s*:\s*{metric}\b"
        for nh, metric in expect_metrics.items()
    ]
    wait_check(
        rt,
        device=device,
        command=f"show route ipv6 {destination} {TARGET_PREFIX_LEN}",
        timeout=timeout,
        interval=interval,
        regex=metric_regex,
        not_contains=[f"Nexthop : {nh}" for nh in absent_nhs],
        label=f"{device} route-metrics {destination}",
    )


def _wait_first_path_os_installed_flag(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    timeout: int,
    interval: int = 2,
) -> None:
    def _path1_flag_set(output: str) -> bool:
        in_path1 = False
        for raw in output.splitlines():
            line = raw.rstrip()
            m_path = re.match(r"^\s*Path\s*\[(\d+)\]\s*:", line)
            if m_path:
                if m_path.group(1) == "1":
                    in_path1 = True
                    continue
                if in_path1:
                    break
                continue

            if not in_path1:
                continue

            m_flag = re.match(r"^\s*Flags\s*:\s*(0x[0-9A-Fa-f]+)\s*$", line)
            if not m_flag:
                continue
            try:
                value = int(m_flag.group(1), 16)
            except ValueError:
                return False
            return (value & 0x1) != 0
        return False

    deadline = time.time() + timeout
    last_out = ""
    while time.time() < deadline:
        out = cmd(rt, device, f"show route ipv6 {destination} {TARGET_PREFIX_LEN}", strict=False)
        last_out = out
        if _path1_flag_set(out):
            return
        time.sleep(interval)

    raise RuntimeError(
        f"{device} path[1] os-installed flag check timeout after {timeout}s\n"
        f"expect: Path [1] contains Flags with bit0 set\n"
        f"command: show route ipv6 {destination} {TARGET_PREFIX_LEN}\n"
        f"last output:\n{last_out}"
    )


def _wait_os_main_gateway(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    expect_gateway: str,
    expect_metric: Optional[int],
    timeout: int,
    interval: int = 2,
) -> None:
    if expect_metric is None:
        row_regex = (
            rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+"
            rf"\S+\s+static\s+\d+\s*$"
        )
        stale_metric_regexes: list[str] = []
    else:
        row_regex = (
            rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+"
            rf"\S+\s+static\s+{expect_metric}\s*$"
        )
        stale_metric_regexes = [
            (
                rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+"
                rf"\S+\s+static\s+(?!{expect_metric}\b)\d+\s*$"
            )
        ]
    wait_check(
        rt,
        device=device,
        command="show fib ipv6 os",
        timeout=timeout,
        interval=interval,
        regex=[row_regex],
        not_regex=stale_metric_regexes,
        label=f"{device} os-best {prefix} via {expect_gateway} metric={expect_metric if expect_metric is not None else '*'}",
    )
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_fib_ipv6_route(
        rt,
        device=device,
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=True,
        nexthop=expect_gateway,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=interval,
        label=f"{device} fib-best {prefix} via {expect_gateway}",
    )


def _cleanup(
    rt: TopologyRuntime,
    *,
    device: str,
    primary_nh: str,
    secondary_nh: str,
) -> None:
    run_cmds(
        rt=rt,
        device=device,
        strict=False,
        commands=[
            "config",
            f"no route ipv6 {TARGET_PREFIX_ADDR} {TARGET_PREFIX_LEN} {primary_nh}",
            f"no route ipv6 {TARGET_PREFIX_ADDR} {TARGET_PREFIX_LEN} {secondary_nh}",
            f"no route ipv6 {RESOLVER_ADDR} {RESOLVER_PREFIX_LEN} {primary_nh}",
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
            "config",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    primary_nh = str(g_top.r1.GE_1.peer_ip6)
    secondary_nh = RESOLVER_ADDR
    r1_link_show = f"{ipaddress.ip_address(str(g_top.r1.GE_1.ip6))}/{int(g_top.r1.GE_1.prefix6)}"

    try:
        step("Cleanup stale static/ipv6 config")
        _cleanup(rt, device="r1", primary_nh=primary_nh, secondary_nh=secondary_nh)

        step("Verify IPv6 underlay from top on GE-1")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv6 Addr  : {r1_link_show}",
                    ],
                    "label": "r1 GE-1 ipv6 up before route-calc check",
                }
            ],
            timeout=20,
            interval=2,
        )

        step("Add resolver route for secondary nexthop")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv6 {RESOLVER_ADDR} {RESOLVER_PREFIX_LEN} {primary_nh}",
                "end",
            ],
        )

        step("Add two static candidate paths for same prefix")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv6 {TARGET_PREFIX_ADDR} {TARGET_PREFIX_LEN} {primary_nh} metric 10",
                f"route ipv6 {TARGET_PREFIX_ADDR} {TARGET_PREFIX_LEN} {secondary_nh} metric 20",
                "end",
            ],
        )

        step("Verify lower metric path selected as OS best")
        _wait_path_total(rt, device="r1", destination=TARGET_PREFIX_ADDR, expect_total=2, timeout=30)
        _wait_route_metrics(
            rt,
            device="r1",
            destination=TARGET_PREFIX_ADDR,
            expect_metrics={
                primary_nh: 10,
                secondary_nh: 20,
            },
            timeout=30,
        )
        _wait_first_path_os_installed_flag(rt, device="r1", destination=TARGET_PREFIX_ADDR, timeout=30)
        _wait_os_main_gateway(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=primary_nh,
            expect_metric=None,
            timeout=30,
        )

        step("Raise primary metric and verify route metric update")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv6 {TARGET_PREFIX_ADDR} {TARGET_PREFIX_LEN} {primary_nh} metric 30",
                "end",
            ],
        )
        _wait_path_total(rt, device="r1", destination=TARGET_PREFIX_ADDR, expect_total=2, timeout=30)
        _wait_route_metrics(
            rt,
            device="r1",
            destination=TARGET_PREFIX_ADDR,
            expect_metrics={
                primary_nh: 30,
                secondary_nh: 20,
            },
            timeout=30,
        )
        _wait_first_path_os_installed_flag(rt, device="r1", destination=TARGET_PREFIX_ADDR, timeout=30)
        _wait_os_main_gateway(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=primary_nh,
            expect_metric=None,
            timeout=30,
        )

        step("Lower primary metric and verify route metric update")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv6 {TARGET_PREFIX_ADDR} {TARGET_PREFIX_LEN} {primary_nh} metric 5",
                "end",
            ],
        )
        _wait_path_total(rt, device="r1", destination=TARGET_PREFIX_ADDR, expect_total=2, timeout=30)
        _wait_route_metrics(
            rt,
            device="r1",
            destination=TARGET_PREFIX_ADDR,
            expect_metrics={
                primary_nh: 5,
                secondary_nh: 20,
            },
            timeout=30,
        )
        _wait_first_path_os_installed_flag(rt, device="r1", destination=TARGET_PREFIX_ADDR, timeout=30)
        _wait_os_main_gateway(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=primary_nh,
            expect_metric=None,
            timeout=30,
        )

        step("Withdraw primary and verify secondary takeover")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"no route ipv6 {TARGET_PREFIX_ADDR} {TARGET_PREFIX_LEN} {primary_nh}",
                "end",
            ],
        )
        _wait_path_total(rt, device="r1", destination=TARGET_PREFIX_ADDR, expect_total=1, timeout=30)
        _wait_route_metrics(
            rt,
            device="r1",
            destination=TARGET_PREFIX_ADDR,
            expect_metrics={
                secondary_nh: 20,
            },
            absent_nhs=[primary_nh],
            timeout=30,
        )
        _wait_first_path_os_installed_flag(rt, device="r1", destination=TARGET_PREFIX_ADDR, timeout=30)
        _wait_os_main_gateway(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=primary_nh,
            expect_metric=None,
            timeout=30,
        )

        step("Re-add primary and verify preemption")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv6 {TARGET_PREFIX_ADDR} {TARGET_PREFIX_LEN} {primary_nh} metric 1",
                "end",
            ],
        )
        _wait_path_total(rt, device="r1", destination=TARGET_PREFIX_ADDR, expect_total=2, timeout=30)
        _wait_route_metrics(
            rt,
            device="r1",
            destination=TARGET_PREFIX_ADDR,
            expect_metrics={
                primary_nh: 1,
                secondary_nh: 20,
            },
            timeout=30,
        )
        _wait_first_path_os_installed_flag(rt, device="r1", destination=TARGET_PREFIX_ADDR, timeout=30)
        _wait_os_main_gateway(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=primary_nh,
            expect_metric=None,
            timeout=30,
        )

        print("Route calc best-path switch check (IPv6) passed.")
    finally:
        step("Cleanup route-calc test config")
        _cleanup(rt, device="r1", primary_nh=primary_nh, secondary_nh=secondary_nh)
