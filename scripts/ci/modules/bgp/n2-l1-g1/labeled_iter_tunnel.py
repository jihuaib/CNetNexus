#!/usr/bin/env python3
"""
BGP labeled-unicast tunnel programming check (IPv4)。

拓扑：r1 --- r2

目标：
- r1 在 IPv4 labeled-unicast 引入直连路由（loop1）；loop1 直连不打 NO_ADV，可被通告。
- r1 端：分配本地标签，下发 MPLS ILM/POP 隧道。
- r2 端：收到 LU 路由后停留在 BGP labeled RIB，labeled AF 不下刷 ROUTE；但仍注册
  bgp-lu 隧道候选 + NHLFE + FTN（labeled 隧道平面）。
- 注：route/FIB/OS install 以及转发面 ping 由 import-rib 测试单独验证（见
  import_rib_labeled_unicast.py），本测试只覆盖 labeled AF 自身的隧道面。
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


AS_R1 = "65001"
AS_R2 = "65002"

R1_LOOP_ID = 1
R1_LOOP_V4 = "1.1.1.1"
R1_LOOP_V4_PREFIX = 32

R2_LOOP_ID = 2
R2_LOOP_V4 = "2.2.2.2"
R2_LOOP_V4_PREFIX = 32

TEST_PREFIX_ADDR = R1_LOOP_V4
TEST_PREFIX_LEN = str(R1_LOOP_V4_PREFIX)
TEST_PREFIX = f"{TEST_PREFIX_ADDR}/{TEST_PREFIX_LEN}"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup BGP/loop config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"no if loop {R1_LOOP_ID}",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"no if loop {R2_LOOP_ID}",
            "end",
        ],
    )


def _wait_r2_lu_route(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    wait_check(
        rt,
        device="r2",
        command=f"show bgp route af ipv4-labeled {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        contains=[f"BGP Route Detail: {TEST_PREFIX}", f"NextHop  : {lu_nexthop}"],
        regex=[
            r"(?im)^\s*Paths\s*:\s*1\s*$",
            r"(?im)^>v\s+From Peer\s*:",
            r"(?im)^\s*RecvLabel\s*:\s*[1-9]\d*\s*$",
            r"(?im)^\s*Valid\s*:\s*Yes\s*$",
            r"(?im)^\s*IterState:\s*Resolved\s*$",
            r"(?im)^\s*Tunnel-ID:\s*[1-9]\d*\s*$",
        ],
        not_contains=[f"Route {TEST_PREFIX} not found"],
        label="r2 LU route is valid and resolved through a tunnel",
    )


def _wait_r2_tunnel_fec(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": "show tunnel candidate",
                "contains": [f"endpoint {TEST_PREFIX_ADDR}", f"nh {lu_nexthop}", "src bgp-lu"],
                "regex": [
                    rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+endpoint\s+{re.escape(TEST_PREFIX_ADDR)}\s+nh\s+"
                    rf"{re.escape(lu_nexthop)}\s+relay\s+-\s+oif\s+\d+\s+src\s+bgp-lu\s+pref\s+\d+\s+"
                    r"labels\s+\[[0-9,]+\]\s*$",
                ],
                "label": "r2 tunnel candidate carries LU label stack",
            },
            {
                "device": "r2",
                "command": "show tunnel nhlfe",
                "contains": [f"endpoint {TEST_PREFIX_ADDR}", f"relay {lu_nexthop}"],
                "regex": [
                    rf"(?im)^\s*id\s+[1-9]\d*\s+endpoint\s+{re.escape(TEST_PREFIX_ADDR)}\s+relay\s+"
                    rf"{re.escape(lu_nexthop)}\s+oif\s+\d+\s+src\s+\S+\s+labels\s+\[[0-9,]+\]\s*$",
                ],
                "label": "r2 tunnel NHLFE resolves LU FEC through relay",
            },
            {
                "device": "r2",
                "command": "show tunnel ftn",
                "regex": [
                    rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+fec\s+{re.escape(TEST_PREFIX_ADDR)}/{TEST_PREFIX_LEN}\s+"
                    r"->\s+nhlfe\s+[1-9]\d*\s+src\s+bgp-lu\s+state\s+up\s*$",
                ],
                "label": "r2 tunnel FTN maps LU FEC to NHLFE",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_to_r2_peer = str(g_top.r1.GE_1.peer_ip)  # r2 GE-1
    r2_to_r1_peer = str(g_top.r2.GE_1.peer_ip)  # r1 GE-1

    try:
        _cleanup(rt)

        step("Configure loop interfaces on both routers")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {R1_LOOP_ID}",
                f"ip address {R1_LOOP_V4} {R1_LOOP_V4_PREFIX}",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"if loop {R2_LOOP_ID}",
                f"ip address {R2_LOOP_V4} {R2_LOOP_V4_PREFIX}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show route ipv4 {R1_LOOP_V4} {R1_LOOP_V4_PREFIX}",
                    "contains": [f"Routing entry for {R1_LOOP_V4}/{R1_LOOP_V4_PREFIX}"],
                    "label": "r1 loop1 connected route present",
                },
                {
                    "device": "r2",
                    "command": f"show route ipv4 {R2_LOOP_V4} {R2_LOOP_V4_PREFIX}",
                    "contains": [f"Routing entry for {R2_LOOP_V4}/{R2_LOOP_V4_PREFIX}"],
                    "label": "r2 loop2 connected route present",
                },
            ],
            timeout=15,
        )

        step("Configure BGP labeled-unicast with import-route connected")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {AS_R1}",
                "router-id 1.1.1.1",
                "timer connect-retry 5",
                f"neighbor {r1_to_r2_peer} as {AS_R2}",
                "af ipv4-labeled",
                f"neighbor {r1_to_r2_peer} enable",
                "import-route connected",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {AS_R2}",
                "router-id 2.2.2.2",
                "timer connect-retry 5",
                f"neighbor {r2_to_r1_peer} as {AS_R1}",
                "af ipv4-labeled",
                f"neighbor {r2_to_r1_peer} enable",
                "import-route connected",
                "exit",
                "end",
            ],
        )

        step("Wait BGP LU session")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-labeled",
                    "regex": [_established_regex(r1_to_r2_peer)],
                    "label": "r1->r2 IPv4 LU established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-labeled",
                    "regex": [_established_regex(r2_to_r1_peer)],
                    "label": "r2->r1 IPv4 LU established",
                },
            ],
            timeout=70,
        )

        step("Verify r1 allocates label and r2 receives LU route for loop1")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show tunnel label",
                    "regex": [rf"(?im)^\s*\d+\s+0\s+ipv4\s+{re.escape(TEST_PREFIX)}\s+bgp:\d+\s+bgp-lu\s*$"],
                    "label": "r1 tunnel label allocated for imported LU FEC",
                },
                {
                    "device": "r1",
                    "command": "show tunnel ilm",
                    "regex": [
                        r"(?im)^\s*vrf\s+0\s+label\s+[1-9]\d*\s+->\s+nhlfe\s+0\s+action\s+pop\(3\)\s+state\s+up\s*$"
                    ],
                    "label": "r1 tunnel ILM pops locally allocated LU label",
                },
                {
                    "device": "r1",
                    "command": "show fib mpls",
                    "regex": [r"(?im)^\s*0\s+[1-9]\d*\s+pop\s+0\s+up\s+[1-9]\d*\s+-\s+-\s+yes\s+yes\s*$"],
                    "label": "r1 FIB MPLS ILM is installed",
                },
                {
                    "device": "r1",
                    "command": "show fib mpls os",
                    "regex": [r"(?im)^\s*main\s+unicast\s+[1-9]\d*\s+-\s+\S+\s+static\s+\d+\s+pop\s*$"],
                    "label": "r1 OS MPLS route pops local LU label",
                },
            ],
            timeout=40,
            interval=2,
        )
        _wait_r2_lu_route(rt, lu_nexthop=r2_to_r1_peer, timeout=50)

        step("Verify r2 LU tunnel candidate/NHLFE/FTN are programmed (labeled plane only)")
        _wait_r2_tunnel_fec(rt, lu_nexthop=r2_to_r1_peer, timeout=50)

        step("Verify r2 labeled AF does NOT install to ROUTE/FIB/OS without import-rib")
        # show route 即使没匹配路由也会打印 "Routing entry for ..." 头部，需要用
        # "(no matching routes)" 或 Path[N] 是否出现来判别
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show route ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
                    "contains": ["(no matching routes)"],
                    "not_regex": [r"Path\s*\[\d+\]\s*:\s*bgp"],
                    "label": "r2 Route RIB has no BGP path for the LU prefix (labeled AF skips flush)",
                },
                {
                    "device": "r2",
                    "command": f"show fib ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
                    "not_contains": [f"Routing entry for {TEST_PREFIX}"],
                    "label": "r2 FIB does not have the LU prefix",
                },
                {
                    "device": "r2",
                    "command": "show fib ipv4 os",
                    "not_regex": [
                        rf"(?im)^\s*main\s+unicast\s+{re.escape(TEST_PREFIX)}\s+\S+\s+\S+\s+bgp\b",
                    ],
                    "label": "r2 OS route table does not have the LU prefix",
                },
            ],
            timeout=10,
            interval=2,
        )

        print("BGP labeled-unicast tunnel programming check passed.")
    finally:
        _cleanup(rt)
