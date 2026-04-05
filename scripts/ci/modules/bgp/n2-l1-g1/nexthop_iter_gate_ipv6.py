#!/usr/bin/env python3
"""
BGP nexthop iteration gate IPv6 check.

Goal:
- r2 advertises a route whose BGP nexthop is initially unreachable
- verify r1 keeps it in Loc-RIB but marks it invalid (not selected)
- add/remove resolver route on r2 to flip r1 nexthop resolved state
- verify r1 route valid/best toggles together with nexthop resolution
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


PFX = "2001:db8:6666::/64"
PFX_ADDR = "2001:db8:6666::"
PFX_MASK = "64"
NH_UNRESOLVED = "2001:db8:51:100::1"
NH_MASK = "128"


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
    token_line = rf"(?im)^.*{re.escape(token)}.*$"
    expect_patterns: list[str] = [token_line]
    reject_patterns: list[str] = []

    best_pattern = rf"(?im)^>.*{re.escape(token)}"
    valid_pattern = rf"(?im)^.v.*{re.escape(token)}"
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


def _wait_route_invalid_or_absent(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    token: str,
    timeout: int,
    interval: int = 2,
) -> None:
    best_pattern = rf"(?im)^>.*{re.escape(token)}"
    valid_pattern = rf"(?im)^.v.*{re.escape(token)}"
    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        regex=[rf"(?im)(?:{re.escape(token)}|\(no routes\))"],
        not_regex=[best_pattern, valid_pattern],
        label=f"{device} route-invalid-or-absent {token}",
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
    command = "show route ipv6 relay bgp"
    if expect_resolved:
        regex = [rf"(?im)^.*{re.escape(nexthop)}\s+yes\s*$"]
    else:
        regex = [rf"(?im)(?:^.*{re.escape(nexthop)}\s+no\s*$|^\s*\(no entries\)\s*$)"]
    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        regex=regex,
        label=f"{device} relay {nexthop} resolved={expect_resolved}",
    )


def _cleanup_case_config(rt: TopologyRuntime, *, r1_peer_ip6: str, r2_peer_ip6: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route ipv6 {NH_UNRESOLVED} {NH_MASK} {r1_peer_ip6}",
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
            f"no route ipv6 {PFX_ADDR} {PFX_MASK} {NH_UNRESOLVED}",
            f"no route ipv6 {NH_UNRESOLVED} {NH_MASK} {r2_peer_ip6}",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)

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
                f"neighbor {r1_peer_ip6} as 65002",
                "af ipv6-unicast",
                f"neighbor {r1_peer_ip6} enable",
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
                f"neighbor {r2_peer_ip6} as 65001",
                "af ipv6-unicast",
                f"neighbor {r2_peer_ip6} enable",
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
                    "command": "show bgp neighbor af ipv6-unicast",
                    "contains": [r1_peer_ip6, "Established"],
                    "label": "r1->r2 ipv6-unicast",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "contains": [r2_peer_ip6, "Established"],
                    "label": "r2->r1 ipv6-unicast",
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
                f"route ipv6 {NH_UNRESOLVED} {NH_MASK} {r2_peer_ip6}",
                "end",
            ],
        )

        step("Inject route on r2 with unresolved-nh static route")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route ipv6 {PFX_ADDR} {PFX_MASK} {NH_UNRESOLVED}",
                "end",
            ],
        )

        step("Ensure r2 has local imported route")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [PFX],
                    "label": "r2 local imported route",
                }
            ],
            timeout=30,
        )

        step("Verify r1 route is valid/best while resolver route exists")
        _wait_route_state(
            rt,
            device="r1",
            command="show bgp route af ipv6-unicast",
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

        step("Remove resolver route on r2")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"no route ipv6 {NH_UNRESOLVED} {NH_MASK} {r2_peer_ip6}",
                "end",
            ],
        )

        step("Verify r1 route becomes invalid or withdrawn after resolver route is removed")
        _wait_route_invalid_or_absent(
            rt,
            device="r1",
            command="show bgp route af ipv6-unicast",
            token=PFX,
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

        step("Add resolver route on r2 again")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route ipv6 {NH_UNRESOLVED} {NH_MASK} {r2_peer_ip6}",
                "end",
            ],
        )

        step("Verify r1 route turns valid again after resolver returns")
        _wait_route_state(
            rt,
            device="r1",
            command="show bgp route af ipv6-unicast",
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

        print("BGP nexthop iteration gate IPv6 check passed.")
    finally:
        _cleanup_case_config(rt, r1_peer_ip6=r1_peer_ip6, r2_peer_ip6=r2_peer_ip6)
