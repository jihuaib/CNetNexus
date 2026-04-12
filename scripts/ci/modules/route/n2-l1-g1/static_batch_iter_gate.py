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

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


PREFIX_MASK = "24"
PREFIX_LEN = 24
PREFIX_COUNT = 10
PREFIX_BASE_OCTET = 66
RESOLVER_MASK = "32"
UNRESOLVED_NH = "198.51.100.1"


def _build_prefixes() -> list[tuple[str, str]]:
    routes: list[tuple[str, str]] = []
    for idx in range(PREFIX_COUNT):
        addr = f"10.{PREFIX_BASE_OCTET}.{idx}.0"
        routes.append((addr, f"{addr}/{PREFIX_LEN}"))
    return routes


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
        check: dict[str, object] = {
            "device": device,
            "command": f"show route ipv4 {addr} {PREFIX_LEN}",
            "label": f"{device} route presence {pfx}",
        }
        if expect_present:
            check["not_contains"] = ["(no matching routes)"]
        else:
            check["contains"] = ["(no matching routes)"]
        checks.append(check)

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
    route_regex = [
        rf"(?im)^\s*ipv4\s+{re.escape(pfx)}\s+{re.escape(nexthop)}\b.*\b{expect_resolved_str}\s+{expect_in_rib_str}\s*$"
        for _, pfx in routes
    ]
    wait_check(
        rt,
        device=device,
        command="show route ipv4 static",
        timeout=timeout,
        interval=interval,
        contains=[f"Total {expect_total} static route(s)"],
        regex=route_regex,
        label=f"{device} static state",
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
    wait_check(
        rt,
        device=device,
        command="show route ipv4 relay static",
        timeout=timeout,
        interval=interval,
        regex=[
            rf"(?im)\bTotal\s+{expect_total_entries}\s+entry\b",
            rf"(?im)^.*\b{re.escape(nexthop)}\b\s+{expect_resolved_str}\s*$",
        ],
        label=f"{device} static relay state",
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
