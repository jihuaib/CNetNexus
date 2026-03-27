#!/usr/bin/env python3
"""
BGP nexthop iteration gate check.

Goal:
- r2 advertises a route whose BGP nexthop is initially unreachable from r1
- verify r1 keeps it in Loc-RIB but marks it invalid (not selected)
- add a resolving underlay route on r1 and verify the route becomes valid/best
- remove the underlay route and verify the route turns invalid again
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


PFX = "10.66.66.0/24"
PFX_ADDR = "10.66.66.0"
PFX_MASK = "255.255.255.0"
NH_UNRESOLVED = "198.51.100.1"
NH_MASK = "255.255.255.255"


def _wait_route_state(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    token: str,
    expect_valid: bool,
    expect_best: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    token_line = rf"(?im)^.*\b{re.escape(token)}\b.*$"
    expect_patterns: list[str] = [token_line]
    reject_patterns: list[str] = []

    best_pattern = rf"(?im)^>.*\b{re.escape(token)}\b"
    valid_pattern = rf"(?im)^.v.*\b{re.escape(token)}\b"
    if expect_best:
        expect_patterns.append(best_pattern)
    else:
        reject_patterns.append(best_pattern)
    if expect_valid:
        expect_patterns.append(valid_pattern)
    else:
        reject_patterns.append(valid_pattern)

    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        regex=expect_patterns,
        not_regex=reject_patterns,
        label=f"{device} route-state {token} best={expect_best} valid={expect_valid}",
    )


def _wait_relay_state(
    rt: TopologyRuntime,
    *,
    device: str,
    nexthop: str,
    expect_resolved: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    command = "show route ipv4 relay bgp"
    expect_str = "yes" if expect_resolved else "no"
    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        regex=[rf"(?im)^.*\b{re.escape(nexthop)}\b\s+{expect_str}\s*$"],
        label=f"{device} relay {nexthop} resolved={expect_str}",
    )


def _cleanup_case_config(rt: TopologyRuntime, *, r1_peer_ip: str, r2_peer_ip: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {NH_UNRESOLVED} {NH_MASK} {r1_peer_ip}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {PFX_ADDR} {PFX_MASK} {NH_UNRESOLVED}",
            f"no route ipv4 {NH_UNRESOLVED} {NH_MASK} {r2_peer_ip}",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    try:
        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", strict=False, commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", strict=False, commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

        step("Configure BGP neighbors and import-route on r2")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_ip} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
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
                "bgp 65002",
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Wait BGP sessions")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [r1_peer_ip, "Established"],
                    "label": "r1->r2 ipv4-unicast",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [r2_peer_ip, "Established"],
                    "label": "r2->r1 ipv4-unicast",
                },
            ],
            timeout=30,
        )

        step("Add resolver route on r2 for static import")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {NH_UNRESOLVED} {NH_MASK} {r2_peer_ip}",
                "end",
            ],
        )

        step("Inject route on r2 with unresolved nexthop for r1")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {PFX_ADDR} {PFX_MASK} {NH_UNRESOLVED}",
                "end",
            ],
        )

        step("Ensure r2 has local imported route")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [PFX],
                    "label": "r2 local imported route",
                }
            ],
            timeout=30,
        )

        step("Verify r1 keeps unresolved-nexthop route as invalid/non-best")
        _wait_route_state(
            rt,
            device="r1",
            command="show bgp route af ipv4-unicast",
            token=PFX,
            expect_valid=False,
            expect_best=False,
            timeout=12,
            interval=2,
        )
        _wait_relay_state(
            rt,
            device="r1",
            nexthop=NH_UNRESOLVED,
            expect_resolved=False,
            timeout=12,
            interval=2,
        )

        step("Add underlay resolving route on r1")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {NH_UNRESOLVED} {NH_MASK} {r1_peer_ip}",
                "end",
            ],
        )

        step("Verify r1 marks route valid/best after nexthop becomes resolvable")
        _wait_route_state(
            rt,
            device="r1",
            command="show bgp route af ipv4-unicast",
            token=PFX,
            expect_valid=True,
            expect_best=True,
            timeout=30,
            interval=2,
        )
        _wait_relay_state(
            rt,
            device="r1",
            nexthop=NH_UNRESOLVED,
            expect_resolved=True,
            timeout=30,
            interval=2,
        )

        step("Remove underlay resolving route on r1")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"no route ipv4 {NH_UNRESOLVED} {NH_MASK} {r1_peer_ip}",
                "end",
            ],
        )

        step("Verify r1 route turns invalid again after nexthop becomes unresolved")
        _wait_route_state(
            rt,
            device="r1",
            command="show bgp route af ipv4-unicast",
            token=PFX,
            expect_valid=False,
            expect_best=False,
            timeout=30,
            interval=2,
        )
        _wait_relay_state(
            rt,
            device="r1",
            nexthop=NH_UNRESOLVED,
            expect_resolved=False,
            timeout=30,
            interval=2,
        )

        print("BGP nexthop iteration gate check passed.")
    finally:
        _cleanup_case_config(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip)
