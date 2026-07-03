#!/usr/bin/env python3
"""VPNv4 ``policy vpn-target`` 入向过滤开关端到端回归。

验证 vpnv4 AF 视图下新增命令 ``policy vpn-target`` / ``no policy vpn-target`` 的语义：

* 默认（``policy vpn-target``）：收到的 vpnv4 路由必须 import-RT(IRT) 命中才接受，否则入向丢弃。
* ``no policy vpn-target``：vpnv4 路由一律接受进公网 vpnv4 RIB（供 RR 透传），
  **但导入私网 VRF 仍要求 IRT 命中**——不命中的路由留在 vpnv4 表里、不进任何 VRF。

拓扑：r1(GE-1 10.12.0.1/30) 直连 r2(GE-1 10.12.0.2/30)，GE-1 公网，eBGP 65001<->65002 跑 vpnv4。
私网源放在 r1 的 VRF red 内 loopback（connected → ``import-route connected`` 进 BGP VRF）。
r1 配 export RT，r2 **默认不配 import RT**（用于演示过滤/接受时序）。

覆盖场景：
A. 默认 policy vpn-target 开启 + r2 无 import-RT → r2 丢弃该 vpnv4 路由（vpnv4 表/VRF 均无）。
B. r2 ``no policy vpn-target`` → ROUTE-REFRESH → vpnv4 表出现该路由，但 VRF red 仍无（IRT 不命中）；
   running-config 出现 ``no policy vpn-target``。
C. r2 恢复 ``policy vpn-target``（仍无 import-RT）→ vpnv4 路由再次被丢弃；running-config 不再含该行。
D. r2 ``no policy vpn-target`` + 配匹配 import-RT → vpnv4 表有该路由且按 IRT 导入 VRF red
   （证明 VRF 导入始终受 IRT 约束、命中即生效）。

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

# r1 export RT / r2 import RT 取同一值（命中导入用）
RT = "65001:100"

# 私网 VRF 内 loopback（/32 主机 connected 路由，作为导出源）
LOOP1 = 10
LOOP1_ADDR = "100.1.1.1"
LOOP1_LEN = 32
PFX1 = "100.1.1.1/32"


def _established_check(device: str, peer_ip: str, label: str) -> dict:
    return {
        "device": device,
        "command": "show bgp neighbor af vpnv4",
        "regex": [rf"(?im)^\s*{re.escape(peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
        "label": label,
    }


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup vpnv4/VRF/loopback config on both sides")
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
                f"no vrf {VRF_NAME}",
                "end",
            ],
        )


def _setup_vrf(rt: TopologyRuntime, device: str, rd: str, *, rt_export: bool, rt_import: bool) -> None:
    commands = [
        "config",
        f"vrf {VRF_NAME}",
        "af ipv4",
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


def _set_r2_policy(rt: TopologyRuntime, *, enabled: bool) -> None:
    """在 r2 vpnv4 AF 视图下开/关 policy vpn-target。"""
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"bgp {R2_AS}",
            "af vpnv4",
            "policy vpn-target" if enabled else "no policy vpn-target",
            "exit",
            "end",
        ],
    )


def _set_r2_import_rt(rt: TopologyRuntime, *, enabled: bool) -> None:
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4",
            f"vpn-target {RT} import" if enabled else f"no vpn-target {RT} import",
            "exit",
            "exit",
            "end",
        ],
    )


def _vpnv4_check(present: bool, label: str) -> dict:
    return {
        "device": "r2",
        "command": "show bgp route af vpnv4",
        "contains": [PFX1] if present else [],
        "not_contains": [] if present else [PFX1],
        "label": label,
    }


def _vrf_check(present: bool, label: str) -> dict:
    return {
        "device": "r2",
        "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
        "contains": [PFX1] if present else [],
        "not_contains": [] if present else [PFX1],
        "label": label,
    }


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer = str(g_top.r1.GE_1.peer_ip)  # r2 的公网地址
    r2_peer = str(g_top.r2.GE_1.peer_ip)  # r1 的公网地址

    try:
        _cleanup(rt)

        step("两端创建 VRF red：r1 配 export RT，r2 不配 import RT")
        _setup_vrf(rt, "r1", R1_RD, rt_export=True, rt_import=False)
        _setup_vrf(rt, "r2", R2_RD, rt_export=False, rt_import=False)
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

        step("配置公网 eBGP + 两端 BGP VRF red af + 两端 vpnv4 邻居（route-refresh 默认开）")
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

        step("场景A：默认 policy vpn-target 开启 + r2 无 import-RT → r2 丢弃 vpnv4 路由")
        hold_check(
            rt,
            device="r2",
            command="show bgp route af vpnv4",
            duration=10,
            not_contains=[PFX1],
            label="A: r2 drops vpnv4 route (policy vpn-target default, no IRT match)",
        )
        hold_check(
            rt,
            device="r2",
            command=f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
            duration=2,
            not_contains=[PFX1],
            label="A: r2 VRF red has no route",
        )

        step("场景B：r2 no policy vpn-target → vpnv4 表接受该路由，但 VRF red 仍无（IRT 不命中）")
        _set_r2_policy(rt, enabled=False)
        wait_checks(
            rt,
            [_vpnv4_check(True, "B: r2 accepts vpnv4 route into vpnv4 RIB after no policy vpn-target")],
            timeout=40,
        )
        # vpnv4 路由虽接受，但无匹配 import-RT，故不应导入任何 VRF。给足传播时间后断言持续不出现。
        hold_check(
            rt,
            device="r2",
            command=f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
            duration=8,
            not_contains=[PFX1],
            label="B: VRF import still gated by IRT (route not in VRF red)",
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "contains": ["no policy vpn-target"],
                    "label": "B: no policy vpn-target shown in running-config",
                }
            ],
            timeout=10,
        )

        step("场景C：r2 恢复 policy vpn-target（仍无 import-RT）→ vpnv4 路由再次被丢弃")
        _set_r2_policy(rt, enabled=True)
        wait_checks(
            rt,
            [_vpnv4_check(False, "C: r2 drops vpnv4 route again after restoring policy vpn-target")],
            timeout=40,
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "not_contains": ["no policy vpn-target"],
                    "label": "C: default policy vpn-target not shown in running-config",
                }
            ],
            timeout=10,
        )

        step("场景D：r2 no policy vpn-target + 配匹配 import-RT → vpnv4 表有路由且按 IRT 导入 VRF red")
        _set_r2_policy(rt, enabled=False)
        _set_r2_import_rt(rt, enabled=True)
        wait_checks(
            rt,
            [
                _vpnv4_check(True, "D: r2 vpnv4 RIB has route"),
                _vrf_check(True, "D: r2 imports route into VRF red once IRT matches"),
            ],
            timeout=40,
        )

        print(
            "VPNv4 policy vpn-target check passed "
            "(default-filter / accept-all-but-VRF-still-IRT-gated / re-enable / IRT-match-imports)."
        )
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
