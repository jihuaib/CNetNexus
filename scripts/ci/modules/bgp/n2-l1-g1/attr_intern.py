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
    verb = "no route static" if negate else "route static"
    for i in range(count):
        second = 100 + i // 256
        third = i % 256
        if "{second}" in prefix_fmt:
            net = prefix_fmt.format(second=second, third=third)
        else:
            net = prefix_fmt.format(third=i)
        cmds.append(f"{verb} ipv4 {net} 24 {nexthop}")
    return cmds


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

        step("Verify dedup: unique attr count < route count")
        wait_check(
            rt,
            device="r2",
            command="show bgp attr",
            regex=[r"(?m)Unique attributes:\s+([1-9]|[1-4]\d)\s*$"],
            timeout=10,
            label="r2 unique attrs < 50",
        )

        step("Verify refcnt >= route count on 10.100.0.0/24")
        wait_check(
            rt,
            device="r2",
            command="show bgp route af ipv4-unicast 10.100.0.0 24",
            regex=[r"Attr-ID\s*:\s*\d+\s*\(refcnt=([5-9]\d|\d{3,})\)"],
            timeout=10,
            label="r2 refcnt >= 50",
        )

        # extract attr_id for cross-phase checks
        output = cmd(rt, "r2", "show bgp route af ipv4-unicast 10.100.0.0 24", strict=False)
        attr_id_p1 = int(re.search(r"Attr-ID\s*:\s*(\d+)", output).group(1))

        step("Verify peer attr detail has AS 65001")
        wait_check(
            rt,
            device="r2",
            command=f"show bgp attr {attr_id_p1}",
            contains=["RefCount", "AS-Path", "65001"],
            timeout=10,
            label="r2 peer attr detail",
        )

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

        step("Verify refcnt decreased, attr_id unchanged")
        wait_check(
            rt,
            device="r2",
            command=f"show bgp route af ipv4-unicast {surviving_prefix} 24",
            regex=[rf"Attr-ID\s*:\s*{attr_id_p1}\s*\(refcnt=([1-9]|[1-4]\d)\)"],
            timeout=10,
            label="r2 refcnt decreased & attr unchanged",
        )

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

        step("Verify unique count increased after local import")
        wait_check(
            rt,
            device="r2",
            command="show bgp attr",
            regex=[r"(?m)Unique attributes:\s+([2-9]|\d{2,})\s*$"],
            timeout=10,
            label="r2 unique >= 2",
        )

        step("Verify local attr differs from peer, no AS 65001")
        wait_check(
            rt,
            device="r2",
            command="show bgp route af ipv4-unicast 10.200.0.0 24",
            regex=[r"Attr-ID\s*:\s*\d+"],
            not_regex=[rf"Attr-ID\s*:\s*{attr_id_p1}\b", r"AS-Path\s*:.*\b65001\b"],
            timeout=10,
            label="r2 local attr differs, no AS 65001",
        )

        print("BGP attr intern test passed.")
    finally:
        _cleanup_case_config(rt, r1_nh=r1_peer_ip, r2_nh=r2_local_nh)
