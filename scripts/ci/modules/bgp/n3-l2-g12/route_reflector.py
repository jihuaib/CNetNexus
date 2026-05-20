#!/usr/bin/env python3
"""
BGP Route Reflection (RFC 4456) CI check.

Topology: r1 --- r2 --- r3  (single AS 65001, iBGP, r2 in the middle)

Goal:
- Baseline (no RR): r2 must NOT re-advertise r1's iBGP-learned routes to r3
  (iBGP split-horizon).
- Enable reflection on r2 by marking r1 and r3 as `neighbor ... reflect-client`
  in both ipv4-unicast and ipv6-unicast AFs. r3 should then receive r1's routes,
  carrying ORIGINATOR_ID (= r1's router-id) and CLUSTER_LIST (= r2's cluster-id).
- Configure an explicit `reflector cluster-id` on r2 — show output on r3 must
  reflect the new cluster-id value.
- Loop detection: set r3's cluster-id equal to r2's cluster-id. r3 must drop the
  reflected route (CLUSTER_LIST already contains its own cluster-id).
- Restore default cluster-id on r3 and verify the route is re-accepted.
"""

from __future__ import annotations

import re
import time

from module_api import (  # noqa: E402
    cmd,
    g_top,
    hold_check,
    mark_step_failed,
    require_devices,
    run_cmds,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


BGP_AS = "65001"

V4_PREFIX_ADDR = "10.88.88.0"
V4_PREFIX_LEN = "24"
V4_PREFIX = f"{V4_PREFIX_ADDR}/{V4_PREFIX_LEN}"

V6_PREFIX_ADDR = "2001:db8:8888::"
V6_PREFIX_LEN = "64"
V6_PREFIX = f"{V6_PREFIX_ADDR}/{V6_PREFIX_LEN}"

R1_ROUTER_ID = "1.1.1.1"
R2_ROUTER_ID = "2.2.2.2"
R3_ROUTER_ID = "3.3.3.3"
R2_EXPLICIT_CLUSTER = "9.9.9.9"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _cleanup(rt: TopologyRuntime, *, r1_nh4: str, r1_nh6: str) -> None:
    step("Cleanup RR config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {V4_PREFIX_ADDR} {V4_PREFIX_LEN} {r1_nh4}",
            f"no route static ipv6 {V6_PREFIX_ADDR} {V6_PREFIX_LEN} {r1_nh6}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(rt=rt, device="r2", strict=False, commands=["config", "no bgp", "end"])
    run_cmds(rt=rt, device="r3", strict=False, commands=["config", "no bgp", "end"])


def _wait_route_with_attrs(
    rt: TopologyRuntime,
    *,
    device: str,
    af: str,
    prefix: str,
    expect_originator: str | None = None,
    expect_cluster_id: str | None = None,
    timeout: int = 30,
    label: str = "",
) -> None:
    """Poll `show bgp route af <af> <prefix-base> <len>` until the detail block shows
    the prefix and (optionally) the expected Originator / Cluster-List value."""
    base, plen = prefix.split("/")
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        out = cmd(rt, device, f"show bgp route af {af} {base} {plen}", strict=False)
        last = out
        if prefix not in out:
            time.sleep(2)
            continue
        if expect_originator and f"Originator: {expect_originator}" not in out:
            time.sleep(2)
            continue
        if expect_cluster_id and expect_cluster_id not in out:
            time.sleep(2)
            continue
        return
    mark_step_failed()
    raise RuntimeError(f"{device}/{af}/{prefix} ({label}) not seen with required attrs.\nlast output:\n{last}")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    # r1 <-> r2 link (GE-1 on both ends)
    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)   # r2's GE-1 IPv4
    r1_peer_v6 = str(g_top.r1.GE_1.peer_ip6)
    r2_to_r1_v4 = str(g_top.r2.GE_1.peer_ip)   # r1's GE-1 IPv4
    r2_to_r1_v6 = str(g_top.r2.GE_1.peer_ip6)

    # r2 <-> r3 link (r2 GE-2, r3 GE-1)
    r2_to_r3_v4 = str(g_top.r2.GE_2.peer_ip)   # r3's GE-1 IPv4
    r2_to_r3_v6 = str(g_top.r2.GE_2.peer_ip6)
    r3_peer_v4 = str(g_top.r3.GE_1.peer_ip)     # r2's GE-2 IPv4
    r3_peer_v6 = str(g_top.r3.GE_1.peer_ip6)

    try:
        step("Configure BGP base on all three routers")
        run_cmds(
            rt=rt, device="r1",
            commands=["config", f"bgp {BGP_AS}", f"router-id {R1_ROUTER_ID}", "end"],
        )
        run_cmds(
            rt=rt, device="r2",
            commands=["config", f"bgp {BGP_AS}", f"router-id {R2_ROUTER_ID}", "end"],
        )
        run_cmds(
            rt=rt, device="r3",
            commands=["config", f"bgp {BGP_AS}", f"router-id {R3_ROUTER_ID}", "end"],
        )

        step("Configure iBGP neighbors (dual-stack)")
        run_cmds(
            rt=rt, device="r1",
            commands=[
                "config", f"bgp {BGP_AS}",
                f"neighbor {r1_peer_v4} as {BGP_AS}",
                f"neighbor {r1_peer_v6} as {BGP_AS}",
                "af ipv4-unicast",
                f"neighbor {r1_peer_v4} enable",
                f"neighbor {r1_peer_v6} enable",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r1_peer_v4} enable",
                f"neighbor {r1_peer_v6} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt, device="r2",
            commands=[
                "config", f"bgp {BGP_AS}",
                f"neighbor {r2_to_r1_v4} as {BGP_AS}",
                f"neighbor {r2_to_r1_v6} as {BGP_AS}",
                f"neighbor {r2_to_r3_v4} as {BGP_AS}",
                f"neighbor {r2_to_r3_v6} as {BGP_AS}",
                "af ipv4-unicast",
                f"neighbor {r2_to_r1_v4} enable",
                f"neighbor {r2_to_r1_v6} enable",
                f"neighbor {r2_to_r3_v4} enable",
                f"neighbor {r2_to_r3_v6} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_to_r1_v4} enable",
                f"neighbor {r2_to_r1_v6} enable",
                f"neighbor {r2_to_r3_v4} enable",
                f"neighbor {r2_to_r3_v6} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt, device="r3",
            commands=[
                "config", f"bgp {BGP_AS}",
                f"neighbor {r3_peer_v4} as {BGP_AS}",
                f"neighbor {r3_peer_v6} as {BGP_AS}",
                "af ipv4-unicast",
                f"neighbor {r3_peer_v4} enable",
                f"neighbor {r3_peer_v6} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r3_peer_v4} enable",
                f"neighbor {r3_peer_v6} enable",
                "exit",
                "end",
            ],
        )

        step("Wait sessions established")
        wait_checks(
            rt,
            [
                {"device": "r1", "command": "show bgp neighbor af ipv4-unicast",
                 "regex": [_established_regex(r1_peer_v4)], "label": "r1->r2 v4"},
                {"device": "r1", "command": "show bgp neighbor af ipv6-unicast",
                 "regex": [_established_regex(r1_peer_v6)], "label": "r1->r2 v6"},
                {"device": "r2", "command": "show bgp neighbor af ipv4-unicast",
                 "regex": [_established_regex(r2_to_r1_v4), _established_regex(r2_to_r3_v4)],
                 "label": "r2 v4 peers"},
                {"device": "r2", "command": "show bgp neighbor af ipv6-unicast",
                 "regex": [_established_regex(r2_to_r1_v6), _established_regex(r2_to_r3_v6)],
                 "label": "r2 v6 peers"},
                {"device": "r3", "command": "show bgp neighbor af ipv4-unicast",
                 "regex": [_established_regex(r3_peer_v4)], "label": "r3->r2 v4"},
                {"device": "r3", "command": "show bgp neighbor af ipv6-unicast",
                 "regex": [_established_regex(r3_peer_v6)], "label": "r3->r2 v6"},
            ],
            timeout=40,
        )

        step("Inject static routes on r1")
        run_cmds(
            rt=rt, device="r1",
            commands=[
                "config",
                f"route static ipv4 {V4_PREFIX_ADDR} {V4_PREFIX_LEN} {r1_peer_v4}",
                f"route static ipv6 {V6_PREFIX_ADDR} {V6_PREFIX_LEN} {r1_peer_v6}",
                "end",
            ],
        )

        step("Baseline: r2 has the routes, r3 must NOT (iBGP split-horizon)")
        wait_checks(
            rt,
            [
                {"device": "r2", "command": "show bgp route af ipv4-unicast",
                 "contains": [V4_PREFIX], "label": "r2 has v4"},
                {"device": "r2", "command": "show bgp route af ipv6-unicast",
                 "contains": [V6_PREFIX], "label": "r2 has v6"},
            ],
            timeout=30,
        )
        hold_check(rt, device="r3", command="show bgp route af ipv4-unicast",
                   duration=8, interval=2, not_contains=[V4_PREFIX],
                   label="r3 baseline v4 absent")
        hold_check(rt, device="r3", command="show bgp route af ipv6-unicast",
                   duration=8, interval=2, not_contains=[V6_PREFIX],
                   label="r3 baseline v6 absent")

        step("Enable RR on r2: mark r3 (both v4 and v6 transport) as reflect-client in both AFs")
        # r3 是反射目标客户端，需要在两个 AF 视图下分别标其 v4/v6 transport peer，
        # 这样 r3 的两个 transport 都会落到 r2 的 RR-client UG（target_is_rr_client=true）。
        # r1 是路由源头但无入向路由，不需要标 client。
        run_cmds(
            rt=rt, device="r2",
            commands=[
                "config", f"bgp {BGP_AS}",
                "af ipv4-unicast",
                f"neighbor {r2_to_r3_v4} reflect-client",
                f"neighbor {r2_to_r3_v6} reflect-client",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_to_r3_v4} reflect-client",
                f"neighbor {r2_to_r3_v6} reflect-client",
                "exit",
                "end",
            ],
        )

        step("r3 now receives reflected routes with ORIGINATOR_ID=r1 + CLUSTER_LIST=r2")
        _wait_route_with_attrs(
            rt, device="r3", af="ipv4-unicast", prefix=V4_PREFIX,
            expect_originator=R1_ROUTER_ID, expect_cluster_id=R2_ROUTER_ID,
            timeout=30, label="r3 v4 reflected",
        )
        _wait_route_with_attrs(
            rt, device="r3", af="ipv6-unicast", prefix=V6_PREFIX,
            expect_originator=R1_ROUTER_ID, expect_cluster_id=R2_ROUTER_ID,
            timeout=30, label="r3 v6 reflected",
        )

        step(f"Configure explicit cluster-id {R2_EXPLICIT_CLUSTER} on r2 (per AF); r3 sees the new value")
        # cluster-id 配置变更自动触发本 AF 所有 peer 的：(1) 出向 soft-out 重发；
        # (2) 给协商过 RR 能力的 peer 发 ROUTE-REFRESH 让对端重传
        run_cmds(
            rt=rt, device="r2",
            commands=[
                "config", f"bgp {BGP_AS}",
                "af ipv4-unicast",
                f"reflector cluster-id {R2_EXPLICIT_CLUSTER}",
                "exit",
                "af ipv6-unicast",
                f"reflector cluster-id {R2_EXPLICIT_CLUSTER}",
                "exit",
                "end",
            ],
        )
        _wait_route_with_attrs(
            rt, device="r3", af="ipv4-unicast", prefix=V4_PREFIX,
            expect_originator=R1_ROUTER_ID, expect_cluster_id=R2_EXPLICIT_CLUSTER,
            timeout=20, label="r3 v4 sees new cluster-id",
        )
        _wait_route_with_attrs(
            rt, device="r3", af="ipv6-unicast", prefix=V6_PREFIX,
            expect_originator=R1_ROUTER_ID, expect_cluster_id=R2_EXPLICIT_CLUSTER,
            timeout=20, label="r3 v6 sees new cluster-id",
        )

        step("Loop check: set r3 cluster-id = r2 cluster-id; r3 must drop the reflected route")
        # r3 改 cluster-id 自动触发：给 r2 发 ROUTE-REFRESH → r2 重传 →
        # r3 ingest 按新 cluster-id 命中环路检测 → reach 转 unreach 撤销
        run_cmds(
            rt=rt, device="r3",
            commands=[
                "config", f"bgp {BGP_AS}",
                "af ipv4-unicast",
                f"reflector cluster-id {R2_EXPLICIT_CLUSTER}",
                "exit",
                "af ipv6-unicast",
                f"reflector cluster-id {R2_EXPLICIT_CLUSTER}",
                "exit",
                "end",
            ],
        )
        hold_check(rt, device="r3", command="show bgp route af ipv4-unicast",
                   duration=10, interval=2, not_contains=[V4_PREFIX],
                   label="r3 drops v4 (cluster-list loop)")
        hold_check(rt, device="r3", command="show bgp route af ipv6-unicast",
                   duration=10, interval=2, not_contains=[V6_PREFIX],
                   label="r3 drops v6 (cluster-list loop)")

        step("Restore: clear r3 cluster-id; route is accepted again")
        # 清 cluster-id 同样自动触发 ROUTE-REFRESH，让对端重传后 r3 再次接受
        run_cmds(
            rt=rt, device="r3",
            commands=[
                "config", f"bgp {BGP_AS}",
                "af ipv4-unicast",
                "no reflector cluster-id",
                "exit",
                "af ipv6-unicast",
                "no reflector cluster-id",
                "exit",
                "end",
            ],
        )
        _wait_route_with_attrs(
            rt, device="r3", af="ipv4-unicast", prefix=V4_PREFIX,
            expect_originator=R1_ROUTER_ID, expect_cluster_id=R2_EXPLICIT_CLUSTER,
            timeout=30, label="r3 v4 re-accepted",
        )
        _wait_route_with_attrs(
            rt, device="r3", af="ipv6-unicast", prefix=V6_PREFIX,
            expect_originator=R1_ROUTER_ID, expect_cluster_id=R2_EXPLICIT_CLUSTER,
            timeout=30, label="r3 v6 re-accepted",
        )

        step("Un-configure reflect-client on r2; previously-reflected routes withdrawn from r3")
        # 去掉 r3 两个 transport peer 的 reflect-client 标记后，r2 应主动发 WITHDRAW，
        # r3 RIB 中之前反射来的路由应被撤销。
        run_cmds(
            rt=rt, device="r2",
            commands=[
                "config", f"bgp {BGP_AS}",
                "af ipv4-unicast",
                f"no neighbor {r2_to_r3_v4} reflect-client",
                f"no neighbor {r2_to_r3_v6} reflect-client",
                "exit",
                "af ipv6-unicast",
                f"no neighbor {r2_to_r3_v4} reflect-client",
                f"no neighbor {r2_to_r3_v6} reflect-client",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {"device": "r3", "command": "show bgp route af ipv4-unicast",
                 "not_contains": [V4_PREFIX], "label": "r3 v4 prefix withdrawn after un-reflect"},
                {"device": "r3", "command": "show bgp route af ipv6-unicast",
                 "not_contains": [V6_PREFIX], "label": "r3 v6 prefix withdrawn after un-reflect"},
            ],
            timeout=20,
        )

        print("BGP route-reflector check passed.")
    finally:
        _cleanup(rt, r1_nh4=r1_peer_v4, r1_nh6=r1_peer_v6)
