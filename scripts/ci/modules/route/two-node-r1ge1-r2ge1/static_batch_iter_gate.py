#!/usr/bin/env python3
"""
Route static nexthop iteration gate check with 10 routes.

Goal:
- add 10 static routes while nexthop is unresolved
- unresolved routes must not appear in `show route`
- `show route static` must always keep all configured routes
- `show route relay static` must reflect static nexthop resolve state
- add/remove one underlay resolver route to flip unresolved/resolved state
"""

from __future__ import annotations

import re
import time
from typing import Optional

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


PREFIX_MASK = "255.255.255.0"
PREFIX_LEN = 24
PREFIX_COUNT = 10
PREFIX_BASE_OCTET = 66
RESOLVER_MASK = "255.255.255.255"
UNRESOLVED_NH = "198.51.100.1"


def _build_prefixes() -> list[tuple[str, str]]:
    routes: list[tuple[str, str]] = []
    for idx in range(PREFIX_COUNT):
        addr = f"10.{PREFIX_BASE_OCTET}.{idx}.0"
        routes.append((addr, f"{addr}/{PREFIX_LEN}"))
    return routes


def _parse_static_rows(output: str) -> dict[str, tuple[str, str, str]]:
    rows: dict[str, tuple[str, str, str]] = {}
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < 7:
            continue
        if parts[0].lower() != "ipv4":
            continue
        prefix = parts[1]
        nexthop = parts[2]
        resolved = parts[-2].lower()
        in_rib = parts[-1].lower()
        rows[prefix] = (nexthop, resolved, in_rib)
    return rows


def _parse_static_total(output: str) -> Optional[int]:
    match = re.search(r"Total\s+(\d+)\s+static route\(s\)", output)
    if not match:
        return None
    return int(match.group(1))


def _parse_relay_row(output: str, nexthop: str) -> Optional[str]:
    for line in output.splitlines():
        if nexthop not in line:
            continue
        parts = line.split()
        if len(parts) < 6:
            continue
        if parts[4] != nexthop:
            continue
        return parts[5].lower()
    return None


def _parse_relay_total(output: str) -> Optional[int]:
    match = re.search(r"Total\s+(\d+)\s+entry", output)
    if not match:
        return None
    return int(match.group(1))
    return None


def _wait_route_presence(
    rt: TopologyRuntime,
    *,
    device: str,
    routes: list[tuple[str, str]],
    expect_present: bool,
    timeout: int,
) -> None:
    checks: list[dict[str, object]] = []
    for addr, pfx in routes:
        if expect_present:
            contains = [pfx, "Total 1 path(s)"]
            label = f"{device} route visible {pfx}"
        else:
            contains = ["(no routes)", "Total 0 path(s)"]
            label = f"{device} route hidden {pfx}"
        checks.append(
            {
                "device": device,
                "command": f"show route ipv4 {addr}",
                "contains": contains,
                "label": label,
            }
        )
    wait_checks(rt, checks, timeout=timeout, interval=2)


def _wait_static_state(
    rt: TopologyRuntime,
    *,
    device: str,
    routes: list[tuple[str, str]],
    nexthop: str,
    expect_total: int,
    expect_resolved: bool,
    expect_in_rib: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    expect_resolved_str = "yes" if expect_resolved else "no"
    expect_in_rib_str = "yes" if expect_in_rib else "no"
    deadline = time.time() + timeout
    last_out = ""

    while time.time() < deadline:
        out = cmd(rt, device, "show route static", strict=False)
        last_out = out

        total = _parse_static_total(out)
        rows = _parse_static_rows(out)
        ok = total == expect_total

        if ok:
            for _, pfx in routes:
                row = rows.get(pfx)
                if not row:
                    ok = False
                    break
                row_nexthop, row_resolved, row_in_rib = row
                if row_nexthop != nexthop:
                    ok = False
                    break
                if row_resolved != expect_resolved_str or row_in_rib != expect_in_rib_str:
                    ok = False
                    break

        if ok:
            return
        time.sleep(interval)

    raise RuntimeError(
        f"{device} static candidate state mismatch after {timeout}s\n"
        f"expect total={expect_total} resolved={expect_resolved_str} in_rib={expect_in_rib_str}\n"
        f"command: show route static\n"
        f"last output:\n{last_out}"
    )


def _wait_relay_state(
    rt: TopologyRuntime,
    *,
    device: str,
    nexthop: str,
    expect_total_entries: int,
    expect_resolved: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    expect_resolved_str = "yes" if expect_resolved else "no"
    deadline = time.time() + timeout
    last_out = ""

    while time.time() < deadline:
        out = cmd(rt, device, "show route relay static", strict=False)
        last_out = out
        row_resolved = _parse_relay_row(out, nexthop)
        total = _parse_relay_total(out)
        if row_resolved is None or total is None:
            time.sleep(interval)
            continue
        if row_resolved == expect_resolved_str and total == expect_total_entries:
            return
        time.sleep(interval)

    raise RuntimeError(
        f"{device} static relay state mismatch after {timeout}s\n"
        f"expect total_entries={expect_total_entries} resolved={expect_resolved_str}\n"
        f"command: show route relay static\n"
        f"last output:\n{last_out}"
    )


def _cleanup_case_config(
    rt: TopologyRuntime,
    *,
    device: str,
    route_nexthop: str,
    resolver_nexthop: str,
    routes: list[tuple[str, str]],
) -> None:
    commands = ["config"]
    for addr, _ in routes:
        commands.append(f"no route ipv4 {addr} {PREFIX_MASK} {route_nexthop}")
    commands.append(f"no route ipv4 {UNRESOLVED_NH} {RESOLVER_MASK} {resolver_nexthop}")
    commands.append("end")
    run_cmds(rt=rt, device=device, strict=False, commands=commands)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    routes = _build_prefixes()
    route_nexthop = UNRESOLVED_NH
    resolver_nexthop = str(g_top.r1.GE_1.peer_ip)

    try:
        step("Cleanup stale config")
        _cleanup_case_config(
            rt,
            device="r1",
            route_nexthop=route_nexthop,
            resolver_nexthop=resolver_nexthop,
            routes=routes,
        )

        step("Add 10 static routes with unresolved nexthop")
        add_cmds = ["config"]
        for addr, _ in routes:
            add_cmds.append(f"route ipv4 {addr} {PREFIX_MASK} {route_nexthop}")
        add_cmds.append("end")
        run_cmds(rt=rt, device="r1", strict=False, commands=add_cmds)

        step("Verify unresolved gate behavior")
        _wait_route_presence(rt, device="r1", routes=routes, expect_present=False, timeout=30)
        _wait_static_state(
            rt,
            device="r1",
            routes=routes,
            nexthop=route_nexthop,
            expect_total=PREFIX_COUNT,
            expect_resolved=False,
            expect_in_rib=False,
            timeout=30,
            interval=2,
        )
        _wait_relay_state(
            rt,
            device="r1",
            nexthop=route_nexthop,
            expect_total_entries=1,
            expect_resolved=False,
            timeout=30,
            interval=2,
        )

        step("Add resolver route so nexthop becomes reachable")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {UNRESOLVED_NH} {RESOLVER_MASK} {resolver_nexthop}",
                "end",
            ],
        )

        step("Verify resolved gate behavior")
        _wait_route_presence(rt, device="r1", routes=routes, expect_present=True, timeout=30)
        _wait_static_state(
            rt,
            device="r1",
            routes=routes,
            nexthop=route_nexthop,
            expect_total=PREFIX_COUNT + 1,
            expect_resolved=True,
            expect_in_rib=True,
            timeout=30,
            interval=2,
        )
        _wait_relay_state(
            rt,
            device="r1",
            nexthop=route_nexthop,
            expect_total_entries=2,
            expect_resolved=True,
            timeout=30,
            interval=2,
        )

        step("Remove resolver route so nexthop becomes unreachable again")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"no route ipv4 {UNRESOLVED_NH} {RESOLVER_MASK} {resolver_nexthop}",
                "end",
            ],
        )

        step("Verify route invalidation after nexthop loss")
        _wait_route_presence(rt, device="r1", routes=routes, expect_present=False, timeout=30)
        _wait_static_state(
            rt,
            device="r1",
            routes=routes,
            nexthop=route_nexthop,
            expect_total=PREFIX_COUNT,
            expect_resolved=False,
            expect_in_rib=False,
            timeout=30,
            interval=2,
        )
        _wait_relay_state(
            rt,
            device="r1",
            nexthop=route_nexthop,
            expect_total_entries=1,
            expect_resolved=False,
            timeout=30,
            interval=2,
        )

        print("Route static batch nexthop iteration gate check passed.")
    finally:
        step("Cleanup static batch routes")
        _cleanup_case_config(
            rt,
            device="r1",
            route_nexthop=route_nexthop,
            resolver_nexthop=resolver_nexthop,
            routes=routes,
        )
