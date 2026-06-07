#!/usr/bin/env python3
"""
BGP nexthop iteration gate dual-stack check.

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


PFX_V4 = "10.66.66.0/24"
PFX_ADDR_V4 = "10.66.66.0"
PFX_MASK_V4 = "24"
NH_UNRESOLVED_V4 = "198.51.100.1"
NH_MASK_V4 = "32"
IMPORTED_PFX_V4 = f"{NH_UNRESOLVED_V4}/{NH_MASK_V4}"

PFX_V6 = "2001:db8:6666::/64"
PFX_ADDR_V6 = "2001:db8:6666::"
PFX_MASK_V6 = "64"
NH_UNRESOLVED_V6 = "2001:db8:51:100::1"
NH_MASK_V6 = "128"
IMPORTED_PFX_V6 = f"{NH_UNRESOLVED_V6}/{NH_MASK_V6}"


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


def _wait_relay_state_ipv4(
    rt: TopologyRuntime,
    *,
    device: str,
    nexthop: str,
    expect_resolved: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    command = "show route relay ipv4 proto bgp"
    if expect_resolved:
        regex = [rf"(?im)^.*\b{re.escape(nexthop)}\b\s+yes\s*$"]
    else:
        regex = [rf"(?im)(?:^.*\b{re.escape(nexthop)}\b\s+no\s*$|^\s*\(no entries\)\s*$)"]
    wait_check(
        rt,
        device=device,
        command=command,
        timeout=timeout,
        interval=interval,
        regex=regex,
        label=f"{device} relay {nexthop} resolved={expect_resolved}",
    )


def _wait_relay_state_ipv6(
    rt: TopologyRuntime,
    *,
    device: str,
    nexthop: str,
    expect_resolved: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    command = "show route relay ipv6 proto bgp"
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


def _cleanup_case_config_ipv4(rt: TopologyRuntime, *, r1_peer_ip: str, r2_peer_ip: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {NH_UNRESOLVED_V4} {NH_MASK_V4} {r1_peer_ip}",
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
            f"no route static ipv4 {PFX_ADDR_V4} {PFX_MASK_V4} {NH_UNRESOLVED_V4}",
            f"no route static ipv4 {NH_UNRESOLVED_V4} {NH_MASK_V4} {r2_peer_ip}",
            "no bgp",
            "end",
        ],
    )


def _cleanup_case_config_ipv6(rt: TopologyRuntime, *, r1_peer_ip6: str, r2_peer_ip6: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv6 {NH_UNRESOLVED_V6} {NH_MASK_V6} {r1_peer_ip6}",
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
            f"no route static ipv6 {PFX_ADDR_V6} {PFX_MASK_V6} {NH_UNRESOLVED_V6}",
            f"no route static ipv6 {NH_UNRESOLVED_V6} {NH_MASK_V6} {r2_peer_ip6}",
            "no bgp",
            "end",
        ],
    )


def _run_ipv4(rt: TopologyRuntime, *, r1_peer_ip: str, r2_peer_ip: str) -> None:
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
                "command": f"show bgp neighbor af ipv4-unicast {r1_peer_ip}",
                "contains": [r1_peer_ip, "Session State", "Established", "afi=1 safi=1 (ipv4-unicast)"],
                "label": "r1->r2 ipv4-unicast",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast {r2_peer_ip}",
                "contains": [r2_peer_ip, "Session State", "Established", "afi=1 safi=1 (ipv4-unicast)"],
                "label": "r2->r1 ipv4-unicast",
            },
        ],
        timeout=40,
    )

    step("Add resolver route on r2 for static import")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"route static ipv4 {NH_UNRESOLVED_V4} {NH_MASK_V4} {r2_peer_ip}",
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
            f"route static ipv4 {PFX_ADDR_V4} {PFX_MASK_V4} {NH_UNRESOLVED_V4}",
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
                "contains": [IMPORTED_PFX_V4],
                "label": "r2 local imported route",
            }
        ],
        timeout=30,
    )

    step("Verify r1 imported route is valid/best while resolver route exists")
    _wait_route_state(
        rt,
        device="r1",
        command="show bgp route af ipv4-unicast",
        token=IMPORTED_PFX_V4,
        expect_valid=True,
        expect_best=True,
        timeout=30,
        interval=2,
    )
    _wait_relay_state_ipv4(
        rt,
        device="r1",
        nexthop=r1_peer_ip,
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
            f"no route static ipv4 {NH_UNRESOLVED_V4} {NH_MASK_V4} {r2_peer_ip}",
            "end",
        ],
    )

    step("Verify r1 route becomes invalid or withdrawn after resolver route is removed")
    _wait_route_invalid_or_absent(
        rt,
        device="r1",
        command="show bgp route af ipv4-unicast",
        token=IMPORTED_PFX_V4,
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
            f"route static ipv4 {NH_UNRESOLVED_V4} {NH_MASK_V4} {r2_peer_ip}",
            "end",
        ],
    )

    step("Verify r1 route turns valid again after resolver returns")
    _wait_route_state(
        rt,
        device="r1",
        command="show bgp route af ipv4-unicast",
        token=IMPORTED_PFX_V4,
        expect_valid=True,
        expect_best=True,
        timeout=30,
        interval=2,
    )
    _wait_relay_state_ipv4(
        rt,
        device="r1",
        nexthop=r1_peer_ip,
        expect_resolved=True,
        timeout=30,
        interval=2,
    )


def _run_ipv6(rt: TopologyRuntime, *, r1_peer_ip6: str, r2_peer_ip6: str) -> None:
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
                "command": f"show bgp neighbor af ipv6-unicast {r1_peer_ip6}",
                "contains": [r1_peer_ip6, "Session State", "Established", "afi=2 safi=1 (ipv6-unicast)"],
                "label": "r1->r2 ipv6-unicast",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast {r2_peer_ip6}",
                "contains": [r2_peer_ip6, "Session State", "Established", "afi=2 safi=1 (ipv6-unicast)"],
                "label": "r2->r1 ipv6-unicast",
            },
        ],
        timeout=40,
    )

    step("Add resolver route on r2 for static import")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"route static ipv6 {NH_UNRESOLVED_V6} {NH_MASK_V6} {r2_peer_ip6}",
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
            f"route static ipv6 {PFX_ADDR_V6} {PFX_MASK_V6} {NH_UNRESOLVED_V6}",
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
                "contains": [IMPORTED_PFX_V6],
                "label": "r2 local imported route",
            }
        ],
        timeout=30,
    )

    step("Verify r1 imported route is valid/best while resolver route exists")
    _wait_route_state(
        rt,
        device="r1",
        command="show bgp route af ipv6-unicast",
        token=IMPORTED_PFX_V6,
        expect_valid=True,
        expect_best=True,
        timeout=30,
        interval=2,
    )
    _wait_relay_state_ipv6(
        rt,
        device="r1",
        nexthop=r1_peer_ip6,
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
            f"no route static ipv6 {NH_UNRESOLVED_V6} {NH_MASK_V6} {r2_peer_ip6}",
            "end",
        ],
    )

    step("Verify r1 route becomes invalid or withdrawn after resolver route is removed")
    _wait_route_invalid_or_absent(
        rt,
        device="r1",
        command="show bgp route af ipv6-unicast",
        token=IMPORTED_PFX_V6,
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
            f"route static ipv6 {NH_UNRESOLVED_V6} {NH_MASK_V6} {r2_peer_ip6}",
            "end",
        ],
    )

    step("Verify r1 route turns valid again after resolver returns")
    _wait_route_state(
        rt,
        device="r1",
        command="show bgp route af ipv6-unicast",
        token=IMPORTED_PFX_V6,
        expect_valid=True,
        expect_best=True,
        timeout=30,
        interval=2,
    )
    _wait_relay_state_ipv6(
        rt,
        device="r1",
        nexthop=r1_peer_ip6,
        expect_resolved=True,
        timeout=30,
        interval=2,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        _run_ipv4(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip)
        _cleanup_case_config_ipv4(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip)
        _run_ipv6(rt, r1_peer_ip6=r1_peer_ip6, r2_peer_ip6=r2_peer_ip6)
        print("BGP nexthop iteration gate dual-stack check passed.")
    finally:
        _cleanup_case_config_ipv4(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip)
        _cleanup_case_config_ipv6(rt, r1_peer_ip6=r1_peer_ip6, r2_peer_ip6=r2_peer_ip6)
