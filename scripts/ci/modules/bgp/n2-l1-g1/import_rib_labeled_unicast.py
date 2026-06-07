#!/usr/bin/env python3
"""
BGP import-rib labeled-unicast 全流程测试（IPv4）。

拓扑：r1 --- r2

设计验证：
  labeled AF 不下刷 ROUTE；unicast AF 通过 import-rib labeled-unicast 将 labeled
  best 镜像到 unicast RIB，由 unicast 走标准优选/下刷（携带 LU 隧道下一跳）。

阶段：
  1. labeled BGP 建立完成，r2 未配置 import-rib：route/FIB/OS 不应有该前缀
  2. r2 配置 import-rib labeled-unicast：route/FIB/OS 出现该前缀（tunnel 下一跳），
     转发面 ping 通
  3. r2 no import-rib labeled-unicast：route/FIB/OS 撤销
  4. r2 重新 import-rib labeled-unicast：route/FIB/OS 恢复，ping 再次通
  5. 保持 r2 的 import-rib：r1 撤销路由（no import-route connected）→ r2 mirror 应被撤销
  6. r1 重新 import-route connected：r2 mirror 恢复，ping 通
"""

from __future__ import annotations

import re

from module_api import (
    g_top,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
    wait_fib_ipv4_route,
)
from top_runner import TopologyRuntime


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


def _wait_r2_labeled_route_present(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    """labeled RIB 中存在 TEST_PREFIX 且有效、隧道已解析"""
    wait_check(
        rt,
        device="r2",
        command=f"show bgp route af ipv4-labeled {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        contains=[f"BGP Route Detail: {TEST_PREFIX}", f"NextHop  : {lu_nexthop}"],
        regex=[
            r"(?im)^\s*Valid\s*:\s*Yes\s*$",
            r"(?im)^\s*IterState:\s*Resolved\s*$",
            r"(?im)^\s*Tunnel-ID:\s*[1-9]\d*\s*$",
        ],
        label="r2 labeled RIB has the LU prefix (valid/resolved)",
    )


def _wait_r2_labeled_route_absent(rt: TopologyRuntime, *, timeout: int) -> None:
    """labeled RIB 中 TEST_PREFIX 已撤销"""
    # show bgp route 即使没路由也会打印 "BGP Route Detail" 头 + "(no RIB)"/"(no matching)"，
    # 需要用 "Paths" 行是否出现来判定
    wait_check(
        rt,
        device="r2",
        command=f"show bgp route af ipv4-labeled {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        not_regex=[r"(?im)^\s*Paths\s*:\s*[1-9]"],
        label="r2 labeled RIB no longer has any path for the LU prefix",
    )


def _assert_r2_route_table_absent(rt: TopologyRuntime, *, timeout: int) -> None:
    """route/FIB/OS 均不应包含 TEST_PREFIX 的 BGP 路由"""
    # show route 即使无路由也会打印 "Routing entry for ..." 头部 + "(no matching routes)"，
    # 需要用 "(no matching routes)" 或检查 Path[N]: bgp 是否出现来判别
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show route ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
                "contains": ["(no matching routes)"],
                "not_regex": [r"Path\s*\[\d+\]\s*:\s*bgp"],
                "label": "r2 Route RIB has NO BGP path for the LU prefix",
            },
            {
                "device": "r2",
                "command": "show fib os ipv4",
                "not_regex": [
                    rf"(?im)^\s*main\s+unicast\s+{re.escape(TEST_PREFIX)}\s+\S+\s+\S+\s+bgp\b",
                ],
                "label": "r2 OS route table has NO BGP entry for the LU prefix",
            },
        ],
        timeout=timeout,
        interval=2,
    )
    wait_fib_ipv4_route(
        rt,
        device="r2",
        prefix_addr=TEST_PREFIX_ADDR,
        prefix_len=TEST_PREFIX_LEN,
        expect_present=False,
        timeout=timeout,
        interval=2,
        label="r2 FIB has NO LU prefix",
    )


def _assert_r2_route_table_present_tunnel(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    """route/FIB/OS 已通过 import-rib 镜像下刷，且为 tunnel 下一跳"""
    wait_check(
        rt,
        device="r2",
        command=f"show route ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {TEST_PREFIX}", f"Nexthop   : {lu_nexthop}"],
        regex=[
            r"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Iter NH\s*:\s*"
            + re.escape(lu_nexthop)
            + r"\b.*?NH-Type\s*:\s*tunnel\b.*?Tunnel-ID\s*:\s*[1-9]\d*",
        ],
        label="r2 Route RIB has LU prefix as tunnel nexthop",
    )
    # FIB 详情：tunnel 路由 Nexthop 字段显示 "tunnel:N"，relay IP 在 "Tunnel :" 行里
    wait_check(
        rt,
        device="r2",
        command=f"show fib ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {TEST_PREFIX}"],
        regex=[
            r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
            r"(?im)^\s*Tunnel-ID\s*:\s*[1-9]\d*\s*$",
            r"(?im)^\s*Installed\s*:\s*yes\s*$",
            r"(?im)^\s*Skip OS\s*:\s*no\s*$",
            rf"(?im)^\s*Tunnel\s*:\s*state=up\s+relay={re.escape(lu_nexthop)}\s+oif=\d+\s+labels=\[[0-9,]+\]\s*$",
        ],
        label="r2 FIB has LU prefix as installed tunnel route",
    )
    row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(TEST_PREFIX)}\s+{re.escape(lu_nexthop)}\s+"
        r"\S+\s+bgp\s+\d+\s+mpls\[[0-9,]+\]\s*$"
    )
    wait_check(
        rt,
        device="r2",
        command="show fib os ipv4",
        timeout=timeout,
        interval=2,
        regex=[row_regex],
        label="r2 OS route table has LU tunnel route",
    )


def _assert_r2_mirror_present(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    """unicast RIB 中存在 import-rib 镜像（IMPORT_RIB 标记）"""
    wait_check(
        rt,
        device="r2",
        command=f"show bgp route af ipv4-unicast {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        contains=[f"BGP Route Detail: {TEST_PREFIX}", f"NextHop  : {lu_nexthop}"],
        regex=[r"(?im)^\s*Valid\s*:\s*Yes\s*$"],
        label="r2 unicast RIB has the import-rib mirror (valid)",
    )


def _assert_r2_mirror_absent(rt: TopologyRuntime, *, timeout: int) -> None:
    wait_check(
        rt,
        device="r2",
        command=f"show bgp route af ipv4-unicast {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        not_regex=[r"(?im)^\s*Paths\s*:\s*[1-9]"],
        label="r2 unicast RIB has NO mirror path",
    )


def _verify_ping_ok(rt: TopologyRuntime, label_suffix: str) -> None:
    wait_check(
        rt,
        device="r2",
        command=f"ping {R1_LOOP_V4} -a {R2_LOOP_V4}",
        timeout=20,
        interval=2,
        regex=[
            rf"(?im)^PING\s+{re.escape(R1_LOOP_V4)}\s+from\s+{re.escape(R2_LOOP_V4)}\s*:\s*\d+\s+data\s+bytes\s*$",
            rf"(?im)^\s*\d+\s+bytes\s+from\s+{re.escape(R1_LOOP_V4)}\s*:\s*icmp_seq=\d+\s+time=",
            r"(?im)^\s*\d+\s+packets\s+transmitted,\s+[1-9]\d*\s+received,",
        ],
        not_regex=[r"(?im)^\s*\d+\s+packets\s+transmitted,\s+0\s+received,"],
        label=f"r2 ping r1 loop1 via LU tunnel succeeds ({label_suffix})",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_to_r2_peer = str(g_top.r1.GE_1.peer_ip)  # r2 GE-1
    r2_to_r1_peer = str(g_top.r2.GE_1.peer_ip)  # r1 GE-1

    try:
        _cleanup(rt)

        # ------------------------------------------------------------
        # 准备：环回 + labeled BGP（暂不配置 import-rib）
        # ------------------------------------------------------------
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

        step("Configure BGP labeled-unicast peering (no import-rib yet)")
        # r1 端预先配好 import-rib labeled-unicast，这样后续 r2 ping 1.1.1.1 时
        # r1 也能通过 LU 隧道把回包导回 r2 loop2。本测试关注的是 r2 端 lifecycle。
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
                "af ipv4-unicast",
                "import-rib public ipv4-labeled-unicast",
                "exit",
                "end",
            ],
        )
        # r2 也宣告 loop2，使 r1 端有 2.2.2.2/32 LU 路由，配合 r1 import-rib 提供回程隧道
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

        step("Wait BGP LU session established")
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

        _wait_r2_labeled_route_present(rt, lu_nexthop=r2_to_r1_peer, timeout=50)

        # ------------------------------------------------------------
        # 阶段 1：无 import-rib，labeled AF 不下刷 route/FIB/OS
        # ------------------------------------------------------------
        step("[Phase 1] Verify labeled AF does NOT install to route/FIB/OS without import-rib")
        _assert_r2_route_table_absent(rt, timeout=15)
        _assert_r2_mirror_absent(rt, timeout=5)

        # ------------------------------------------------------------
        # 阶段 2：r2 启用 import-rib，验证下刷 + ping
        # ------------------------------------------------------------
        step("[Phase 2] Enable import-rib labeled-unicast on r2; expect route/FIB/OS install + ping OK")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {AS_R2}",
                "af ipv4-unicast",
                "import-rib public ipv4-labeled-unicast",
                "exit",
                "end",
            ],
        )
        _assert_r2_mirror_present(rt, lu_nexthop=r2_to_r1_peer, timeout=30)
        _assert_r2_route_table_present_tunnel(rt, lu_nexthop=r2_to_r1_peer, timeout=30)
        _verify_ping_ok(rt, "phase2 enable")

        # ------------------------------------------------------------
        # 阶段 3：撤销 import-rib，验证撤销下刷
        # ------------------------------------------------------------
        step("[Phase 3] Disable import-rib on r2; expect route/FIB/OS withdrawn")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {AS_R2}",
                "af ipv4-unicast",
                "no import-rib public ipv4-labeled-unicast",
                "exit",
                "end",
            ],
        )
        _assert_r2_mirror_absent(rt, timeout=30)
        _assert_r2_route_table_absent(rt, timeout=30)

        # ------------------------------------------------------------
        # 阶段 4：重新启用 import-rib，验证恢复 + ping
        # ------------------------------------------------------------
        step("[Phase 4] Re-enable import-rib on r2; expect route/FIB/OS install + ping OK")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {AS_R2}",
                "af ipv4-unicast",
                "import-rib public ipv4-labeled-unicast",
                "exit",
                "end",
            ],
        )
        _assert_r2_mirror_present(rt, lu_nexthop=r2_to_r1_peer, timeout=30)
        _assert_r2_route_table_present_tunnel(rt, lu_nexthop=r2_to_r1_peer, timeout=30)
        _verify_ping_ok(rt, "phase4 re-enable")

        # ------------------------------------------------------------
        # 阶段 5：保留 r2 配置，r1 撤销路由 → mirror/route 应撤销
        # ------------------------------------------------------------
        step("[Phase 5] r1 withdraws routes (no import-route connected); expect r2 mirror withdrawn")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {AS_R1}",
                "af ipv4-labeled",
                "no import-route connected",
                "exit",
                "end",
            ],
        )
        _wait_r2_labeled_route_absent(rt, timeout=30)
        _assert_r2_mirror_absent(rt, timeout=30)
        _assert_r2_route_table_absent(rt, timeout=30)

        # ------------------------------------------------------------
        # 阶段 6：r1 重新发布 → r2 mirror/route 恢复 + ping
        # ------------------------------------------------------------
        step("[Phase 6] r1 re-publishes routes; expect r2 mirror restored + ping OK")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {AS_R1}",
                "af ipv4-labeled",
                "import-route connected",
                "exit",
                "end",
            ],
        )
        _wait_r2_labeled_route_present(rt, lu_nexthop=r2_to_r1_peer, timeout=30)
        _assert_r2_mirror_present(rt, lu_nexthop=r2_to_r1_peer, timeout=30)
        _assert_r2_route_table_present_tunnel(rt, lu_nexthop=r2_to_r1_peer, timeout=30)
        _verify_ping_ok(rt, "phase6 re-publish")

        print("BGP import-rib labeled-unicast full lifecycle check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
