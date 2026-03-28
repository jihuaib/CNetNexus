#!/usr/bin/env python3
"""
Route calc best-path switch check (static metric based).

Goal:
- install two static paths for the same prefix (both reachable)
- verify route path metrics are updated as configured
- verify OS route stays installable via resolved reachable gateway (kernel metric remains 0)
- verify withdraw/re-add keeps route availability consistent
"""

from __future__ import annotations

import re
import time
from typing import Optional

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TARGET_PREFIX_ADDR = "203.0.113.0"
TARGET_MASK = "255.255.255.0"
TARGET_PREFIX = f"{TARGET_PREFIX_ADDR}/24"
RESOLVER_ADDR = "198.51.100.1"
RESOLVER_MASK = "255.255.255.255"

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
        command=f"show route ipv4 {destination}",
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
        command=f"show route ipv4 {destination}",
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
        out = cmd(rt, device, f"show route ipv4 {destination}", strict=False)
        last_out = out
        if _path1_flag_set(out):
            return
        time.sleep(interval)

    raise RuntimeError(
        f"{device} path[1] os-installed flag check timeout after {timeout}s\n"
        f"expect: Path [1] contains Flags with bit0 set\n"
        f"command: show route ipv4 {destination}\n"
        f"last output:\n{last_out}"
    )


def _wait_os_main_gateway(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    expect_gateway: str,
    expect_metric: int,
    timeout: int,
    interval: int = 2,
) -> None:
    row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+"
        rf"\S+\s+static\s+{expect_metric}\s*$"
    )
    stale_metric_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+"
        rf"\S+\s+static\s+(?!{expect_metric}\b)\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show route ipv4 os",
        timeout=timeout,
        interval=interval,
        regex=[row_regex],
        not_regex=[stale_metric_regex],
        label=f"{device} os-best {prefix} via {expect_gateway} metric={expect_metric}",
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
            f"no route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {primary_nh}",
            f"no route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {secondary_nh}",
            f"no route ipv4 {RESOLVER_ADDR} {RESOLVER_MASK} {primary_nh}",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    primary_nh = str(g_top.r1.GE_1.peer_ip)
    secondary_nh = RESOLVER_ADDR

    try:
        step("Cleanup stale static config")
        _cleanup(rt, device="r1", primary_nh=primary_nh, secondary_nh=secondary_nh)

        step("Ensure r1 GE-1 is up")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "if GE-1",
                "no shutdown",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show if GE-1",
                    "contains": ["Interface GE-1 Detail:", "State      : UP"],
                    "label": "r1 GE-1 up before route-calc check",
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
                f"route ipv4 {RESOLVER_ADDR} {RESOLVER_MASK} {primary_nh}",
                "end",
            ],
        )

        step("Add two static candidate paths for same prefix")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {primary_nh} metric 10",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {secondary_nh} metric 20",
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
            expect_metric=0,
            timeout=30,
        )

        step("Raise primary metric and verify route metric update")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {primary_nh} metric 30",
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
        # Secondary path is recursive and resolves to the same direct gateway on this topology.
        _wait_os_main_gateway(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=primary_nh,
            expect_metric=0,
            timeout=30,
        )

        step("Lower primary metric and verify route metric update")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {primary_nh} metric 5",
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
            expect_metric=0,
            timeout=30,
        )

        step("Withdraw primary and verify secondary takeover")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"no route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {primary_nh}",
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
            expect_metric=0,
            timeout=30,
        )

        step("Re-add primary and verify preemption")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {primary_nh} metric 1",
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
            expect_metric=0,
            timeout=30,
        )

        print("Route calc best-path switch check passed.")
    finally:
        step("Cleanup route-calc test config")
        _cleanup(rt, device="r1", primary_nh=primary_nh, secondary_nh=secondary_nh)
