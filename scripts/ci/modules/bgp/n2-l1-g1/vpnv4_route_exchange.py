#!/usr/bin/env python3
"""VPNv4（MPLS L3VPN）路由收发 + 导出/导入(import-RT 过滤)/显示功能端到端回归。

与 ``vpnv4_basic.py``（只验证 vpnv4 会话建立/协商）互补，本用例同时验证 **数据面导出**
（bgp_vrf_export：私网 VRF 路由按 RD 导出到公网 vpnv4）与 **数据面导入**
（bgp_vrf_import：收到 vpnv4 路由按 import-RT 过滤，命中才入表并导入对应 VRF）。

拓扑：r1(GE-1 10.12.0.1/30) 直连 r2(GE-1 10.12.0.2/30)，GE-1 保持公网，
eBGP 65001 <-> 65002 跑 vpnv4。私网路由放在 r1 的 VRF red 内 loopback 上
（loop 在 VRF 内 → connected 路由 → ``import-route connected`` 进 BGP VRF RIB）。
r1 的 VRF red 配 export RT，r2 的 VRF red 配 import RT，二者取同一 RT 值。

覆盖时序（对应 bgp_vrf_import + bgp_vrf_export 实现）：
1. vpnv4 会话建立。
2. r1 使能 vpnv4 后全量扫描导出：r1 本地 vpnv4 表按 RD 出现私网路由（带 export RT）。
3a. **r2 尚无 import-RT → 收到的 vpnv4 路由因 IRT 不匹配被丢弃**：r2 vpnv4 表 / VRF red 均无该路由。
3b. **r2 配置 vpn-target import → 自动触发 ROUTE-REFRESH → r2 重新收到**：vpnv4 表出现该 RD 下的路由，
    且按 import-RT 导入 r2 的 VRF red ipv4-unicast RIB。
3c. **数据面转发**：导入私网的路由经 eBGP-vpnv4 邻接假隧道（BGP_ADJ）以 NH-Type=tunnel 下刷 r2 的
    VRF FIB，并压入对端 PE 通告的私网 VPN 标签（Out-Label）——r1 据此标签 demux 到正确 VRF。
4. ``show bgp route af vpnv4 rd <rd>`` RD 过滤：命中/不命中。
4b. 前缀详情（跨 RD / 单 RD）+ 接收标签语义。
5. 增量：vpnv4 已起来后 r1 新增私网路由 → r2 vpnv4 + VRF red 自动出现。
6. 撤销：删除私网路由 → r2 vpnv4 + VRF red 自动撤销。
7. apply-label per-route 配置回放（默认 per-vrf 不显示；per-route 进 running-config）。

本用例由 scripts/ci/module_runner.py 加载。
"""

from __future__ import annotations

import re
import time

from module_api import (  # noqa: E402
    g_top,
    hold_check,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
R1_AS = 65001
R2_AS = 65002
R1_RD = "65001:1"
R2_RD = "65002:1"
RD_NOMATCH = "65009:9"

# r1 export RT / r2 import RT 取同一值（eBGP 下 RT 值任意，但两端必须一致才能命中导入）
RT = "65001:100"

# 私网 VRF 内 loopback（loopback 地址在 ROUTE 侧是 /32 主机 connected 路由，作为导出源）
LOOP1 = 10
LOOP1_ADDR = "100.1.1.1"
LOOP1_LEN = 32
PFX1 = "100.1.1.1/32"

# 增量场景用的第二个私网前缀
LOOP2 = 11
LOOP2_ADDR = "100.2.2.2"
LOOP2_LEN = 32
PFX2 = "100.2.2.2/32"


def _rd_header_check(device: str, rd: str, prefix: str, label: str) -> dict:
    """vpnv4 表按 RD 分段显示，且该 RD 段下含指定前缀。"""
    return {
        "device": device,
        "command": "show bgp route af vpnv4",
        "contains": [f"RD: {rd}", prefix],
        "regex": [rf"(?im)^\s*RD:\s*{re.escape(rd)}\s*$"],
        "label": label,
    }


def _vrf_route_check(device: str, prefix: str, label: str) -> dict:
    """导入路由出现在该设备 VRF red 的 ipv4-unicast BGP RIB。"""
    return {
        "device": device,
        "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
        "contains": [prefix],
        "label": label,
    }


def _established_check(device: str, peer_ip: str, label: str) -> dict:
    return {
        "device": device,
        "command": "show bgp neighbor af vpnv4",
        "regex": [rf"(?im)^\s*{re.escape(peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
        "label": label,
    }


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup vpnv4/VRF/loopback/static config on both sides")
    for dev in ("r1", "r2"):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                "no bgp",
                f"no if loop {LOOP1}",
                f"no if loop {LOOP2}",
                f"no vrf {VRF_NAME}",
                "end",
            ],
        )


def _setup_vrf(rt: TopologyRuntime, device: str, rd: str, *, rt_export: bool, rt_import: bool) -> None:
    """创建 VRF red：RD + apply-label per-vrf，按需配 export/import RT。"""
    commands = [
        "config",
        f"vrf {VRF_NAME}",
        "af ipv4-unicast",
        f"route-distinguisher {rd}",
        "apply-label per-vrf",
    ]
    if rt_export:
        commands.append(f"vpn-target {RT} export")
    if rt_import:
        commands.append(f"vpn-target {RT} import")
    commands += ["exit", "exit", "end"]
    run_cmds(rt=rt, device=device, commands=commands)
    wait_checks(
        rt,
        [
            {
                "device": device,
                "command": f"show vrf name {VRF_NAME}",
                "contains": ["VRF Detail:", f"Name           : {VRF_NAME}"],
                "label": f"{device} vrf {VRF_NAME} ready",
            }
        ],
        timeout=15,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer = str(g_top.r1.GE_1.peer_ip)  # r2 的公网地址
    r2_peer = str(g_top.r2.GE_1.peer_ip)  # r1 的公网地址

    try:
        _cleanup(rt)

        step("两端创建 VRF red：r1 配 export RT，r2 先不配 import RT（演示过滤时序）")
        _setup_vrf(rt, "r1", R1_RD, rt_export=True, rt_import=False)
        _setup_vrf(rt, "r2", R2_RD, rt_export=False, rt_import=False)
        # VRF_ADD/AF_ENABLE/AF_RD_ADD/IMPORT_RT 事件需传播到 BGP 的 VRF cache。
        time.sleep(2)

        step("r1 在 VRF red 内建 loopback（connected 路由作为导出源）")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {LOOP1}",
                f"vrf forwarding {VRF_NAME}",
                f"ip address {LOOP1_ADDR} {LOOP1_LEN}",
                "exit",
                "end",
            ],
        )

        step("配置公网 eBGP + 两端 BGP VRF red af + 两端 vpnv4 邻居")
        # r1：私网 import-route connected 提供导出源；r2：建 VRF red ipv4-unicast 实例以承接导入。
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {R1_AS}",
                "router-id 1.1.1.1",
                f"neighbor {r1_peer} as {R2_AS}",
                f"vrf {VRF_NAME}",
                "af ipv4-unicast",
                "import-route connected",
                "exit",
                "exit",
                "af vpnv4",
                f"neighbor {r1_peer} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {R2_AS}",
                "router-id 2.2.2.2",
                f"neighbor {r2_peer} as {R1_AS}",
                f"vrf {VRF_NAME}",
                "af ipv4-unicast",
                "import-route connected",
                "exit",
                "exit",
                "af vpnv4",
                f"neighbor {r2_peer} enable",
                "exit",
                "end",
            ],
        )

        step("等待 vpnv4 eBGP 会话 Established")
        wait_checks(
            rt,
            [
                _established_check("r1", r1_peer, "r1->r2 vpnv4 established"),
                _established_check("r2", r2_peer, "r2->r1 vpnv4 established"),
            ],
            timeout=60,
        )

        step("场景2：r1 本地 vpnv4 表按 RD 出现导出的私网路由（带 export RT）")
        wait_checks(
            rt,
            [_rd_header_check("r1", R1_RD, PFX1, "r1 local vpnv4 export under RD")],
            timeout=30,
        )

        step("场景3a：r2 无 import-RT → 收到的 vpnv4 路由因 IRT 不匹配被丢弃（vpnv4 表/VRF 均无）")
        # 给足传播时间后，断言该路由在一段时间内始终不出现（被入向过滤丢弃）。
        hold_check(
            rt,
            device="r2",
            command="show bgp route af vpnv4",
            duration=10,
            not_contains=[PFX1],
            label="r2 drops vpnv4 route without matching import-RT",
        )
        hold_check(
            rt,
            device="r2",
            command=f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
            duration=2,
            not_contains=[PFX1],
            label="r2 VRF red has no imported route yet",
        )

        step("场景3b：r2 配置 vpn-target import → 自动 ROUTE-REFRESH → 重新收到并导入 VRF")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"vrf {VRF_NAME}",
                "af ipv4-unicast",
                f"vpn-target {RT} import",
                "exit",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                _rd_header_check("r2", R1_RD, PFX1, "r2 received vpnv4 route after import-RT (refresh)"),
                _vrf_route_check("r2", PFX1, "r2 imported route into VRF red ipv4-unicast"),
            ],
            timeout=40,
        )

        step("场景3c：数据面——r2 导入私网路由经 eBGP-vpnv4 假隧道 + 压私网 VPN 标签下刷 VRF FIB")
        # 导入私网的路由（REMOTE_CROSS）下一跳是远端 PE（r1=r2_peer），在公网表做隧道迭代命中
        # 「eBGP-vpnv4 邻接假隧道」(BGP_ADJ)，以 NH-Type=tunnel 进 r2 的 VRF red 转发表。
        # 关键：转发时必须压入对端 PE 通告的私网 VPN 标签（Out-Label，非 0），r1 收到后才能据此
        # 标签 demux 到正确 VRF——共享 GE-1 无 per-VRF 子接口，没有标签无法区分 VRF。
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show route ipv4 vrf {VRF_NAME} {LOOP1_ADDR} {LOOP1_LEN}",
                    "contains": ["Path [1]: bgp"],
                    "regex": [
                        # NH-Type=tunnel + Tunnel-ID 非 0 + Out-Label（VPN 私网标签）非 0
                        r"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?NH-Type\s*:\s*tunnel\b.*?Tunnel-ID\s*:\s*[1-9]\d*"
                        r".*?Out-Label\s*:\s*[1-9]\d*",
                    ],
                    "label": "r2 imported VRF route iterates eBGP-vpnv4 tunnel with VPN label",
                },
                {
                    "device": "r2",
                    "command": f"show fib ipv4 vrf {VRF_NAME} {LOOP1_ADDR} {LOOP1_LEN}",
                    "contains": [f"Routing entry for {PFX1}"],
                    "regex": [
                        r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
                        r"(?im)^\s*Out-Label\s*:\s*[1-9]\d*\s*$",
                        r"(?im)^\s*Installed\s*:\s*yes\s*$",
                    ],
                    "label": "r2 imported VRF route installed to FIB via tunnel with VPN label",
                },
            ],
            timeout=15,
        )

        step("场景4：show bgp route af vpnv4 rd <rd> 过滤（命中 + 不命中）")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show bgp route af vpnv4 rd {R1_RD}",
                    "contains": [f"RD: {R1_RD}", PFX1],
                    "label": "r2 rd filter match shows route",
                },
                {
                    "device": "r2",
                    "command": f"show bgp route af vpnv4 rd {RD_NOMATCH}",
                    "not_contains": [PFX1],
                    "label": "r2 rd filter no-match hides route",
                },
            ],
            timeout=20,
        )

        step("场景4b：前缀详情（不带 rd 跨 RD 列出 / 带 rd 缩到单 RD）+ 标签语义校验")
        # 标签不在 path attribute 里（在 NLRI）；同一前缀两份状态：
        #   - 发送端 r1 的 loc-rib 导出路由：发送时才申请标签 → 详情【无标签行】
        #   - 接收端 r2 的 loc-rib：收报文时存下收到的标签 → 详情显示 RecvLabel（证明 rib-out 带标签）
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show bgp route af vpnv4 {LOOP1_ADDR} {LOOP1_LEN}",
                    "contains": ["BGP Route Detail", f"RD: {R1_RD}", "RecvLabel"],
                    "label": "r2 prefix detail (no rd): by-RD + received label present",
                },
                {
                    "device": "r2",
                    "command": f"show bgp route af vpnv4 {LOOP1_ADDR} {LOOP1_LEN} rd {R1_RD}",
                    "contains": ["BGP Route Detail", f"RD: {R1_RD}", "RecvLabel"],
                    "label": "r2 prefix detail with rd",
                },
                {
                    "device": "r1",
                    "command": f"show bgp route af vpnv4 {LOOP1_ADDR} {LOOP1_LEN}",
                    "contains": ["BGP Route Detail", f"RD: {R1_RD}"],
                    "not_contains": ["RecvLabel", "LocalLabel"],
                    "label": "r1 loc-rib export has NO label (allocated at send time)",
                },
            ],
            timeout=20,
        )

        step("场景5：增量——vpnv4 已起来后 r1 新增私网路由 → r2 vpnv4 + VRF red 自动收到")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {LOOP2}",
                f"vrf forwarding {VRF_NAME}",
                f"ip address {LOOP2_ADDR} {LOOP2_LEN}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                _rd_header_check("r1", R1_RD, PFX2, "r1 incremental export PFX2"),
                _rd_header_check("r2", R1_RD, PFX2, "r2 received incremental PFX2"),
                _vrf_route_check("r2", PFX2, "r2 imported incremental PFX2 into VRF red"),
            ],
            timeout=30,
        )

        step("场景6：撤销——删除私网路由 → r2 vpnv4 + VRF red 自动撤销")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", f"no if loop {LOOP2}", "end"],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af vpnv4",
                    "contains": [PFX1],  # 第一条仍在
                    "not_contains": [PFX2],  # 第二条已撤销
                    "label": "r2 vpnv4 withdrew PFX2 keeps PFX1",
                },
                {
                    "device": "r2",
                    "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                    "contains": [PFX1],
                    "not_contains": [PFX2],
                    "label": "r2 VRF red withdrew imported PFX2 keeps PFX1",
                },
            ],
            timeout=30,
        )

        step("场景7：apply-label per-route 配置回放（默认 per-vrf 不显示）")
        # 默认 per-vrf 不应出现在 running-config
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "not_contains": ["apply-label per-route"],
                    "label": "r1 default per-vrf not shown",
                }
            ],
            timeout=10,
        )
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"vrf {VRF_NAME}",
                "af ipv4-unicast",
                "apply-label per-route",
                "exit",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "contains": ["apply-label per-route"],
                    "label": "r1 per-route round-trips in running-config",
                }
            ],
            timeout=10,
        )

        print(
            "VPNv4 exchange (export/import-RT filter+refresh/RD-show/label/incremental/withdraw/apply-label) "
            "check passed."
        )
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
