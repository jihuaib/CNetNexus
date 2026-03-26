#!/usr/bin/env python3
"""
Route calc best-path switch check (static metric based).

Goal:
- install two static paths for the same prefix (both reachable)
- verify route path metrics are updated as configured
- verify OS route stays installable via resolved reachable gateway
- verify withdraw/re-add keeps route availability consistent
"""

from __future__ import annotations

import re
import time
from typing import Optional

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TARGET_PREFIX_ADDR = "203.0.113.0"
TARGET_MASK = "255.255.255.0"
TARGET_PREFIX = f"{TARGET_PREFIX_ADDR}/24"
RESOLVER_ADDR = "198.51.100.1"
RESOLVER_MASK = "255.255.255.255"

PATH_TOTAL_RE = re.compile(r"Total\s+(\d+)\s+path\(s\)")
OS_TOTAL_RE = re.compile(r"Total\s+(\d+)\s+route\(s\)")


def _parse_path_total(output: str) -> Optional[int]:
    match = PATH_TOTAL_RE.search(output)
    if not match:
        return None
    return int(match.group(1))


def _parse_os_total(output: str) -> Optional[int]:
    match = OS_TOTAL_RE.search(output)
    if not match:
        return None
    return int(match.group(1))


def _parse_os_rows(output: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for raw in output.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("Table") or line.startswith("-------") or line.startswith("Total"):
            continue

        parts = line.split()
        if len(parts) < 7:
            continue

        table, route_type, prefix, gateway, interface, proto, metric = parts[:7]
        if "/" not in prefix:
            continue
        if not metric.isdigit():
            continue

        rows.append(
            {
                "table": table.lower(),
                "type": route_type.lower(),
                "prefix": prefix,
                "gateway": gateway,
                "interface": interface,
                "proto": proto.lower(),
                "metric": metric,
            }
        )
    return rows


def _parse_route_metrics(output: str) -> dict[str, int]:
    metrics: dict[str, int] = {}
    current_nh: Optional[str] = None

    for raw in output.splitlines():
        line = raw.strip()
        if not line:
            continue

        if line.startswith("Nexthop"):
            _, _, value = line.partition(":")
            current_nh = value.strip()
            continue

        if line.startswith("Metric"):
            if not current_nh:
                continue
            _, _, value = line.partition(":")
            value = value.strip()
            if value.isdigit():
                metrics[current_nh] = int(value)
            current_nh = None

    return metrics


def _wait_path_total(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    expect_total: int,
    timeout: int,
    interval: int = 2,
) -> None:
    deadline = time.time() + timeout
    last_out = ""

    while time.time() < deadline:
        out = cmd(rt, device, f"show route ipv4 {destination}", strict=False)
        last_out = out

        total = _parse_path_total(out)
        if total is not None and total == expect_total:
            return

        time.sleep(interval)

    raise RuntimeError(
        f"{device} route path total mismatch after {timeout}s\n"
        f"expect total={expect_total} destination={destination}\n"
        f"command: show route ipv4 {destination}\n"
        f"last output:\n{last_out}"
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
    deadline = time.time() + timeout
    last_out = ""
    absent_nhs = absent_nhs or []

    while time.time() < deadline:
        out = cmd(rt, device, f"show route ipv4 {destination}", strict=False)
        last_out = out

        metrics = _parse_route_metrics(out)

        ok = True
        for nh, metric in expect_metrics.items():
            if metrics.get(nh) != metric:
                ok = False
                break

        if ok:
            for nh in absent_nhs:
                if nh in metrics:
                    ok = False
                    break

        if ok:
            return

        time.sleep(interval)

    raise RuntimeError(
        f"{device} route metric mismatch after {timeout}s\n"
        f"expect metrics={expect_metrics} absent={absent_nhs} destination={destination}\n"
        f"command: show route ipv4 {destination}\n"
        f"last output:\n{last_out}"
    )


def _wait_os_main_gateway(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    expect_gateway: str,
    timeout: int,
    interval: int = 2,
) -> None:
    deadline = time.time() + timeout
    last_out = ""

    while time.time() < deadline:
        out = cmd(rt, device, "show route ipv4 os", strict=False)
        last_out = out

        total = _parse_os_total(out)
        if total is None:
            time.sleep(interval)
            continue

        rows = _parse_os_rows(out)
        gateway = None
        for row in rows:
            if row["table"] != "main":
                continue
            if row["type"] != "unicast":
                continue
            if row["proto"] != "static":
                continue
            if row["prefix"] != prefix:
                continue
            gateway = row["gateway"]
            break

        if gateway == expect_gateway:
            return

        time.sleep(interval)

    raise RuntimeError(
        f"{device} OS best route gateway mismatch after {timeout}s\n"
        f"expect prefix={prefix} table=main type=unicast proto=static gateway={expect_gateway}\n"
        f"command: show route ipv4 os\n"
        f"last output:\n{last_out}"
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
        _wait_os_main_gateway(rt, device="r1", prefix=TARGET_PREFIX, expect_gateway=primary_nh, timeout=30)

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
        # Secondary path is recursive and resolves to the same direct gateway on this topology.
        _wait_os_main_gateway(rt, device="r1", prefix=TARGET_PREFIX, expect_gateway=primary_nh, timeout=30)

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
        _wait_os_main_gateway(rt, device="r1", prefix=TARGET_PREFIX, expect_gateway=primary_nh, timeout=30)

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
        _wait_os_main_gateway(rt, device="r1", prefix=TARGET_PREFIX, expect_gateway=primary_nh, timeout=30)

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
        _wait_os_main_gateway(rt, device="r1", prefix=TARGET_PREFIX, expect_gateway=primary_nh, timeout=30)

        print("Route calc best-path switch check passed.")
    finally:
        step("Cleanup route-calc test config")
        _cleanup(rt, device="r1", primary_nh=primary_nh, secondary_nh=secondary_nh)
