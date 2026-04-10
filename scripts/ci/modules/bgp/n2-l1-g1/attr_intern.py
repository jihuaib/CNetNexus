#!/usr/bin/env python3
"""
BGP attr intern test script.

Phases:
1. Inject 50 static routes on r1, import into BGP, propagate to r2
   - verify dedup: unique attr count << route count, refcnt > 1
2. Withdraw 20 routes on r1
   - verify r2 refcnt decreases accordingly
3. On r2 inject 10 local static routes with import-route static
   - local-import routes have different attrs (no AS_PATH, different nexthop)
   - verify unique attr count increases (>= 2 distinct attrs)
   - verify local route attr_id differs from peer route attr_id
"""

from __future__ import annotations

import re

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402

PEER_ROUTE_COUNT = 50
WITHDRAW_COUNT = 20
LOCAL_ROUTE_COUNT = 10

PEER_NET = "10.{second}.{third}.0"
LOCAL_NET = "10.200.{third}.0"


def _gen_static_cmds(prefix_fmt: str, count: int, nexthop: str, *, negate: bool = False) -> list[str]:
    """Generate route ipv4 commands from a prefix template."""
    cmds: list[str] = []
    verb = "no route" if negate else "route"
    for i in range(count):
        second = 100 + i // 256
        third = i % 256
        if "{second}" in prefix_fmt:
            net = prefix_fmt.format(second=second, third=third)
        else:
            net = prefix_fmt.format(third=i)
        cmds.append(f"{verb} ipv4 {net} 24 {nexthop}")
    return cmds


def _get_unique_count(rt: TopologyRuntime, device: str) -> int:
    """Parse 'show bgp attr' output for unique attribute count."""
    output = cmd(rt, device, "show bgp attr", strict=False)
    m = re.search(r"Unique attributes:\s*(\d+)", output)
    assert m, f"'show bgp attr' parse failed on {device}, output: {output}"
    return int(m.group(1))


def _get_route_attr(rt: TopologyRuntime, device: str, prefix: str, masklen: int) -> tuple[int, int]:
    """Get (attr_id, refcnt) from route detail."""
    output = cmd(rt, device, f"show bgp route af ipv4-unicast {prefix} {masklen}", strict=False)
    m = re.search(r"Attr-ID\s*:\s*(\d+)\s*\(refcnt=(\d+)\)", output)
    assert m, f"Route detail missing Attr-ID for {prefix}/{masklen} on {device}, output: {output}"
    return int(m.group(1)), int(m.group(2))


def _cleanup_case_config(rt: TopologyRuntime, *, r1_nh: str, r2_nh: str) -> None:
    step("Cleanup BGP/static config")
    peer_no = _gen_static_cmds(PEER_NET, PEER_ROUTE_COUNT, r1_nh, negate=True)
    local_no = _gen_static_cmds(LOCAL_NET, LOCAL_ROUTE_COUNT, r2_nh, negate=True)
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config"] + peer_no + ["no bgp", "end"],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=["config"] + local_no + ["no bgp", "end"],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    """Entry called by module_runner."""
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_local_nh = str(g_top.r2.GE_1.ip)

    try:
        # ==================================================================
        # Setup: BGP base + neighbors
        # ==================================================================
        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

        step("Configure BGP neighbors + r1 import-route static")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_ip} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                "bgp 65002",
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
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
                    "contains": [r1_peer_ip],
                    "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 ipv4-unicast",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [r2_peer_ip],
                    "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2->r1 ipv4-unicast",
                },
            ],
            timeout=30,
        )

        # ==================================================================
        # Phase 1: Inject 50 routes, verify dedup
        # ==================================================================
        step(f"Phase 1: Inject {PEER_ROUTE_COUNT} static routes on r1")
        peer_cmds = _gen_static_cmds(PEER_NET, PEER_ROUTE_COUNT, r1_peer_ip)
        run_cmds(rt=rt, device="r1", commands=["config"] + peer_cmds + ["end"])

        last_i = PEER_ROUTE_COUNT - 1
        last_prefix = PEER_NET.format(second=100 + last_i // 256, third=last_i % 256)

        step(f"Wait r2 learns last route {last_prefix}/24")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{last_prefix}/24"],
                    "label": f"r2 has {last_prefix}/24",
                },
            ],
            timeout=60,
        )

        unique_phase1 = _get_unique_count(rt, "r2")
        print(f"  Phase 1: unique attrs = {unique_phase1}, routes = {PEER_ROUTE_COUNT}")
        assert unique_phase1 < PEER_ROUTE_COUNT, (
            f"Dedup failed: unique={unique_phase1} >= routes={PEER_ROUTE_COUNT}"
        )

        attr_id_p1, refcnt_p1 = _get_route_attr(rt, "r2", "10.100.0.0", 24)
        print(f"  10.100.0.0/24 -> Attr-ID={attr_id_p1}, refcnt={refcnt_p1}")
        assert refcnt_p1 >= PEER_ROUTE_COUNT, (
            f"RefCount too low: {refcnt_p1} < {PEER_ROUTE_COUNT}"
        )

        attr_out = cmd(rt, "r2", f"show bgp attr {attr_id_p1}", strict=False)
        assert "RefCount" in attr_out, f"show bgp attr {attr_id_p1} missing RefCount"
        assert "AS-Path" in attr_out, f"show bgp attr {attr_id_p1} missing AS-Path"
        assert "65001" in attr_out, f"show bgp attr {attr_id_p1} missing AS 65001 in path"
        print(f"  show bgp attr {attr_id_p1} -> OK (AS-Path contains 65001)")

        # ==================================================================
        # Phase 2: Withdraw 20 routes, verify refcnt decreases
        # ==================================================================
        step(f"Phase 2: Withdraw {WITHDRAW_COUNT} routes on r1")
        withdraw_cmds = _gen_static_cmds(PEER_NET, WITHDRAW_COUNT, r1_peer_ip, negate=True)
        run_cmds(rt=rt, device="r1", commands=["config"] + withdraw_cmds + ["end"])

        step("Wait r2 loses withdrawn route 10.100.0.0/24")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": ["10.100.0.0/24"],
                    "label": "r2 lost 10.100.0.0/24",
                },
            ],
            timeout=60,
        )

        surviving_i = WITHDRAW_COUNT
        surviving_prefix = PEER_NET.format(second=100 + surviving_i // 256, third=surviving_i % 256)
        attr_id_p2, refcnt_p2 = _get_route_attr(rt, "r2", surviving_prefix, 24)
        expected_remaining = PEER_ROUTE_COUNT - WITHDRAW_COUNT
        print(f"  {surviving_prefix}/24 -> Attr-ID={attr_id_p2}, refcnt={refcnt_p2}")
        print(f"  Expected remaining routes: {expected_remaining}")
        assert refcnt_p2 < refcnt_p1, (
            f"RefCount did not decrease: before={refcnt_p1}, after={refcnt_p2}"
        )
        assert attr_id_p2 == attr_id_p1, (
            f"Attr-ID changed unexpectedly: before={attr_id_p1}, after={attr_id_p2}"
        )
        print(f"  RefCount decreased: {refcnt_p1} -> {refcnt_p2}")

        # ==================================================================
        # Phase 3: r2 import local routes with different attributes
        # ==================================================================
        step(f"Phase 3: r2 enable import-route static + inject {LOCAL_ROUTE_COUNT} local routes")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                "bgp 65002",
                "af ipv4-unicast",
                "import-route static",
                "exit",
                "end",
            ],
        )
        local_cmds = _gen_static_cmds(LOCAL_NET, LOCAL_ROUTE_COUNT, r2_local_nh)
        run_cmds(rt=rt, device="r2", commands=["config"] + local_cmds + ["end"])

        local_last = LOCAL_NET.format(third=LOCAL_ROUTE_COUNT - 1)
        step(f"Wait r2 has local route {local_last}/24 in BGP RIB")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{local_last}/24"],
                    "label": f"r2 has local {local_last}/24",
                },
            ],
            timeout=30,
        )

        unique_phase3 = _get_unique_count(rt, "r2")
        print(f"  Phase 3: unique attrs = {unique_phase3} (was {unique_phase1} in phase 1)")
        assert unique_phase3 > unique_phase1, (
            f"Unique count did not increase after local import: {unique_phase3} <= {unique_phase1}"
        )

        local_attr_id, local_refcnt = _get_route_attr(rt, "r2", "10.200.0.0", 24)
        print(f"  Local 10.200.0.0/24 -> Attr-ID={local_attr_id}, refcnt={local_refcnt}")
        assert local_attr_id != attr_id_p1, (
            f"Local attr should differ from peer attr: both={local_attr_id}"
        )

        local_attr_out = cmd(rt, "r2", f"show bgp attr {local_attr_id}", strict=False)
        assert "Origin" in local_attr_out, "Local attr missing Origin"
        m_asp = re.search(r"AS-Path\s*:\s*(.+)", local_attr_out)
        if m_asp:
            local_as_path = m_asp.group(1).strip()
            assert "65001" not in local_as_path, (
                f"Local import should not have AS 65001, got: {local_as_path}"
            )
            print(f"  Local attr AS-Path: '{local_as_path}' (no 65001, correct)")

        print(f"\nBGP attr intern test passed.")
        print(
            f"  Phase 1: {PEER_ROUTE_COUNT} routes, {unique_phase1} unique attrs "
            f"(dedup {PEER_ROUTE_COUNT / unique_phase1:.0f}x)"
        )
        print(f"  Phase 2: withdrew {WITHDRAW_COUNT}, refcnt {refcnt_p1} -> {refcnt_p2}")
        print(
            f"  Phase 3: +{LOCAL_ROUTE_COUNT} local routes, unique {unique_phase1} -> {unique_phase3} "
            f"(peer attr_id={attr_id_p1}, local attr_id={local_attr_id})"
        )
    finally:
        _cleanup_case_config(rt, r1_nh=r1_peer_ip, r2_nh=r2_local_nh)
