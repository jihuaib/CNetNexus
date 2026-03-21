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

import time
from typing import Optional

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


PFX = "10.66.66.0/24"
PFX_ADDR = "10.66.66.0"
PFX_MASK = "255.255.255.0"
NH_UNRESOLVED = "198.51.100.1"
NH_MASK = "255.255.255.255"


def _find_prefix_line(output: str, token: str) -> Optional[str]:
    for line in output.splitlines():
        if token in line:
            return line
    return None


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
    deadline = time.time() + timeout
    last_out = ""
    while time.time() < deadline:
        out = cmd(rt, device, command, strict=False)
        last_out = out
        line = _find_prefix_line(out, token)
        if line is None:
            time.sleep(interval)
            continue

        marker = line[:2] if len(line) >= 2 else line
        best = len(marker) >= 1 and marker[0] == ">"
        valid = len(marker) >= 2 and marker[1] == "v"
        if best == expect_best and valid == expect_valid:
            return
        time.sleep(interval)

    raise RuntimeError(
        f"{device} route '{token}' state mismatch after {timeout}s\n"
        f"expect: best={expect_best} valid={expect_valid}\n"
        f"command: {command}\n"
        f"last output:\n{last_out}"
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
    command = "show route relay bgp"
    expect_str = "yes" if expect_resolved else "no"
    deadline = time.time() + timeout
    last_out = ""
    while time.time() < deadline:
        out = cmd(rt, device, command, strict=False)
        last_out = out
        line = _find_prefix_line(out, nexthop)
        if line is None:
            time.sleep(interval)
            continue
        got = line.strip().split()[-1].lower()
        if got == expect_str:
            return
        time.sleep(interval)

    raise RuntimeError(
        f"{device} relay '{nexthop}' state mismatch after {timeout}s\n"
        f"expect resolved={expect_str}\n"
        f"command: {command}\n"
        f"last output:\n{last_out}"
    )


def _cleanup_case_config(rt: TopologyRuntime, *, r1_peer_ip: str) -> None:
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

        step("Inject route on r2 with initially unreachable BGP nexthop")
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
        _cleanup_case_config(rt, r1_peer_ip=r1_peer_ip)
