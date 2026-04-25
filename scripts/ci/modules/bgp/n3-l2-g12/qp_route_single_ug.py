#!/usr/bin/env python3
"""
BGP QP 自产生路由 + 常规 update-group 划分 + 保持 BID 下一跳 检查（IPv4 QP AF）

拓扑: r1 --- r2 --- r3

AS 安排:
- r1: AS 65001（eBGP 对 r2）
- r2: AS 65002（QP 路由源；同时承载 r1 为 eBGP 邻居、r3 为 iBGP 邻居）
- r3: AS 65002（iBGP 对 r2）

验证目标:
1. QP 地址族下，三台设备 BGP 会话 Established
2. route-select 未启用时，r2 自产生 QP 路由不向外发送
3. route-select 启用后，r1 (eBGP) 和 r3 (iBGP) 均收到 QP 路由
4. r2 的 QP 对外发布复用常规 update-group 划分，不再强制把 eBGP+iBGP 合入同一个 group
5. QP 对外发布保持 BID 下一跳，且 eBGP 路径仍携带本地 AS
6. 重复配置拒绝、前缀覆盖拒绝
7. `no route-select enable` 生效后 r1/r3 不再见到 QP 路由
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    cmd,
    g_top,
    hold_check,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


AS_EBGP = "65001"   # r1
AS_LOCAL = "65002"  # r2 + r3

# 自产生 QP 路由配置
QP_START_DQPN = 1000
QP_COUNT = 4
QP_PFX_ADDR = "10.99.99.0"
QP_MASK = 24
QP_BID = "2001:db8:aaaa::1"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup QP config")
    run_cmds(rt=rt, device="r1", strict=False, commands=["config", "no bgp", "end"])
    run_cmds(rt=rt, device="r2", strict=False, commands=["config", "no bgp", "end"])
    run_cmds(rt=rt, device="r3", strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    # r1 <-> r2 (GE-1 on both)
    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)       # r2 的 GE-1 IPv4
    r2_to_r1_v4 = str(g_top.r2.GE_1.peer_ip)      # r1 的 GE-1 IPv4
    # r2 <-> r3 (r2 GE-2 <-> r3 GE-1)
    r2_to_r3_v4 = str(g_top.r2.GE_2.peer_ip)      # r3 的 GE-1 IPv4
    r3_peer_v4 = str(g_top.r3.GE_1.peer_ip)       # r2 的 GE-2 IPv4

    try:
        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", commands=["config", f"bgp {AS_EBGP}", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", commands=["config", f"bgp {AS_LOCAL}", "router-id 2.2.2.2", "end"])
        run_cmds(rt=rt, device="r3", commands=["config", f"bgp {AS_LOCAL}", "router-id 3.3.3.3", "end"])

        step("Configure neighbors and enable ipv4-qp on all nodes")
        # r1: 与 r2 eBGP，仅启用 ipv4-qp
        run_cmds(
            rt=rt, device="r1",
            commands=[
                "config", f"bgp {AS_EBGP}",
                f"neighbor {r1_peer_v4} as {AS_LOCAL}",
                "af ipv4-qp", f"neighbor {r1_peer_v4} enable", "exit",
                "end",
            ],
        )
        # r2: 与 r1 eBGP，与 r3 iBGP，均启用 ipv4-qp
        run_cmds(
            rt=rt, device="r2",
            commands=[
                "config", f"bgp {AS_LOCAL}",
                f"neighbor {r2_to_r1_v4} as {AS_EBGP}",
                f"neighbor {r2_to_r3_v4} as {AS_LOCAL}",
                "af ipv4-qp",
                f"neighbor {r2_to_r1_v4} enable",
                f"neighbor {r2_to_r3_v4} enable",
                "exit",
                "end",
            ],
        )
        # r3: 与 r2 iBGP，启用 ipv4-qp
        run_cmds(
            rt=rt, device="r3",
            commands=[
                "config", f"bgp {AS_LOCAL}",
                f"neighbor {r3_peer_v4} as {AS_LOCAL}",
                "af ipv4-qp", f"neighbor {r3_peer_v4} enable", "exit",
                "end",
            ],
        )

        step("Wait QP BGP sessions established")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-qp",
                    "regex": [_established_regex(r2_to_r1_v4), _established_regex(r2_to_r3_v4)],
                    "label": "r2 QP both peers Established",
                },
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-qp",
                    "regex": [_established_regex(r1_peer_v4)],
                    "label": "r1 QP peer Established",
                },
                {
                    "device": "r3",
                    "command": "show bgp neighbor af ipv4-qp",
                    "regex": [_established_regex(r3_peer_v4)],
                    "label": "r3 QP peer Established",
                },
            ],
            timeout=40,
        )

        step("Verify QP neighbor detail shows local/remote AFs and sent counters")
        detail_out = cmd(rt, "r2", f"show bgp neighbor af ipv4-qp {r2_to_r1_v4}", strict=False)
        for text in (
            "Local Address Families",
            "Remote Address Families",
            "Negotiated Address Families",
            "Sent Messages:",
            "afi=1 safi=253 (ipv4-qp)",
        ):
            if text not in detail_out:
                raise RuntimeError(f"r2 QP neighbor detail missing '{text}':\n{detail_out}")
        if not re.search(r"(?ims)^\s*Sent Messages:\s*$.*?^\s*OPEN\s*:\s*[1-9]\d*\s*$", detail_out):
            raise RuntimeError(f"r2 QP neighbor detail missing non-zero Sent OPEN counter:\n{detail_out}")

        step("Configure QP self-originated route on r2 (route-select disabled by default)")
        run_cmds(
            rt=rt, device="r2",
            commands=[
                "config", f"bgp {AS_LOCAL}",
                "af ipv4-qp",
                f"route start-dqpn {QP_START_DQPN} ip {QP_PFX_ADDR} mask {QP_MASK} count {QP_COUNT} bid {QP_BID}",
                "exit", "end",
            ],
        )

        step("r2 sees own QP routes locally")
        wait_check(
            rt, device="r2", command="show bgp route af ipv4-qp", timeout=15,
            regex=[r"(?im)^Total:\s*[1-9]\d*\s+networks"],
            label="r2 local QP route count>=1",
        )

        step("Hold-check: peers must NOT receive QP routes while route-select disabled")
        hold_check(
            rt, device="r1", command="show bgp route af ipv4-qp",
            duration=10, interval=2,
            contains=["(no routes)"],
            label="r1 has no QP routes (route-select disabled)",
        )
        hold_check(
            rt, device="r3", command="show bgp route af ipv4-qp",
            duration=10, interval=2,
            contains=["(no routes)"],
            label="r3 has no QP routes (route-select disabled)",
        )

        step("Enable route-select on r2 ipv4-qp AF")
        run_cmds(
            rt=rt, device="r2",
            commands=["config", f"bgp {AS_LOCAL}", "af ipv4-qp", "route-select enable", "exit", "end"],
        )

        step("Peers receive QP routes after route-select enable")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-qp",
                    "regex": [r"(?im)^Total:\s*[1-9]\d*\s+networks"],
                    "label": "r1 has QP routes",
                },
                {
                    "device": "r3",
                    "command": "show bgp route af ipv4-qp",
                    "regex": [r"(?im)^Total:\s*[1-9]\d*\s+networks"],
                    "label": "r3 has QP routes",
                },
            ],
            timeout=30,
        )

        step("Verify nexthop stays as BID, and eBGP export still carries local AS")
        # r1 为 eBGP 邻居：正常 next-hop-self 会覆盖为本端地址，QP NH_UNCHANGED 应保留 BID
        # 同时 eBGP 仍必须 prepend 本地 AS，避免对端因缺少 AS_PATH 丢弃 UPDATE。
        # r3 为 iBGP 邻居：本地 IMPORT 路由通常替换为 local，QP 应保留 BID
        r1_out = cmd(rt, "r1", "show bgp route af ipv4-qp", strict=False)
        r3_out = cmd(rt, "r3", "show bgp route af ipv4-qp", strict=False)
        bid_compact = re.escape(QP_BID)
        if not re.search(bid_compact, r1_out, re.IGNORECASE):
            raise RuntimeError(f"r1 QP route output missing BID {QP_BID}:\n{r1_out}")
        if not re.search(bid_compact, r3_out, re.IGNORECASE):
            raise RuntimeError(f"r3 QP route output missing BID {QP_BID}:\n{r3_out}")
        if not re.search(rf"(?im)\b{re.escape(AS_LOCAL)}\b", r1_out):
            raise RuntimeError(f"r1 QP route output missing AS_PATH {AS_LOCAL}:\n{r1_out}")

        step("Verify QP detail lookup accepts only strict string-key format")
        qp_key = f"dqpn={QP_START_DQPN},ip={QP_PFX_ADDR}/{QP_MASK}"
        detail_out = cmd(rt, "r1", f"show bgp route af ipv4-qp {qp_key}", strict=False)
        if "BGP Route Detail:" not in detail_out or f"AS-Path  : {AS_LOCAL}" not in detail_out:
            raise RuntimeError(f"r1 strict QP detail query failed unexpectedly:\n{detail_out}")
        bad_detail_out = cmd(rt, "r1", f"show bgp route af ipv4-qp dqpn={QP_START_DQPN},ip={QP_PFX_ADDR},mask={QP_MASK}", strict=False)
        if "BGP Error: Invalid QP route query format." not in bad_detail_out:
            raise RuntimeError(f"r1 non-strict QP detail query was accepted unexpectedly:\n{bad_detail_out}")

        step("Verify r2 reuses normal update-group split for QP peers")
        ug_summary = cmd(rt, "r2", "show bgp update-group af ipv4-qp", strict=False)
        total_m = re.search(r"(?im)^Total:\s*(\d+)\s+groups", ug_summary)
        if not total_m or int(total_m.group(1)) != 2:
            raise RuntimeError(f"expected exactly 2 update-groups for QP (eBGP + iBGP), got:\n{ug_summary}")
        if len(re.findall(r"(?im)^\s*\d+\s+eBGP\s+", ug_summary)) != 1:
            raise RuntimeError(f"expected one eBGP update-group for QP, got:\n{ug_summary}")
        if len(re.findall(r"(?im)^\s*\d+\s+iBGP\s+", ug_summary)) != 1:
            raise RuntimeError(f"expected one iBGP update-group for QP, got:\n{ug_summary}")

        step("Reject duplicate QP route config")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"bgp {AS_LOCAL}",
                "af ipv4-qp",
                f"route start-dqpn {QP_START_DQPN} ip {QP_PFX_ADDR} mask {QP_MASK} count {QP_COUNT} bid {QP_BID}",
                "exit",
                "end",
            ],
        )
        dup_show = cmd(rt, "r2", "show bgp route af ipv4-qp", strict=False)
        dup_total = re.search(r"(?im)^Total:\s*(\d+)\s+networks", dup_show)
        if not dup_total or int(dup_total.group(1)) != QP_COUNT:
            raise RuntimeError(f"duplicate QP route changed route count unexpectedly:\n{dup_show}")

        step("Reject overlapping-prefix QP route config")
        ovl_dqpn = QP_START_DQPN + 100
        ovl_mask = QP_MASK - 1
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"bgp {AS_LOCAL}",
                "af ipv4-qp",
                f"route start-dqpn {ovl_dqpn} ip {QP_PFX_ADDR} mask {ovl_mask} count 1 bid {QP_BID}",
                "exit",
                "end",
            ],
        )
        ovl_show = cmd(rt, "r2", "show bgp route af ipv4-qp", strict=False)
        ovl_total = re.search(r"(?im)^Total:\s*(\d+)\s+networks", ovl_show)
        if not ovl_total or int(ovl_total.group(1)) != QP_COUNT:
            raise RuntimeError(f"overlapping QP route changed route count unexpectedly:\n{ovl_show}")
        if re.search(rf"(?im)\bdqpn={ovl_dqpn}\b", ovl_show) or re.search(rf"(?im)/{ovl_mask}\b", ovl_show):
            raise RuntimeError(f"overlapping QP route was accepted unexpectedly:\n{ovl_show}")

        step("Disable route-select; peers should withdraw QP routes")
        run_cmds(
            rt=rt, device="r2",
            commands=["config", f"bgp {AS_LOCAL}", "af ipv4-qp", "no route-select enable", "exit", "end"],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-qp",
                    "contains": ["(no routes)"],
                    "label": "r1 QP routes withdrawn",
                },
                {
                    "device": "r3",
                    "command": "show bgp route af ipv4-qp",
                    "contains": ["(no routes)"],
                    "label": "r3 QP routes withdrawn",
                },
            ],
            timeout=30,
        )

        print("QP route single-UG + BID nexthop + route-select toggle check passed.")
    finally:
        _cleanup(rt)
