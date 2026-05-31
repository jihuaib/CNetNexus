#!/usr/bin/env python3
"""VPNv4（MPLS L3VPN, AFI=1/SAFI=128）基础功能回归。

覆盖：
1. `af vpnv4` 进入地址族视图，`neighbor <ip> enable` 仅使能 vpnv4 地址族。
2. 仅启用 vpnv4 即可把 eBGP 会话拉到 Established（`show bgp neighbor af vpnv4`），
   IPv4 邻居与 IPv6 邻居（vpnv4 over IPv6 传输）两条会话都验证。
3. `show bgp neighbor af vpnv4 <ip>` 详情能看到协商出来的 vpnv4 地址族
   （Negotiated Address Families 含 `afi=1 safi=128 (vpnv4)`），v4/v6 邻居均验证。
4. show current-configuration 回放 `af vpnv4` / `neighbor <ip> enable`（v4 + v6）。

本用例 case 脚本，由 scripts/ci/module_runner.py 加载；拓扑 r1(GE-1 10.12.0.1/30,
2001:db8:12::1/64) 直连 r2(GE-1 10.12.0.2/30, 2001:db8:12::2/64)，eBGP 65001 <-> 65002。
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def _cleanup_case_config(rt: TopologyRuntime) -> None:
    step("Cleanup BGP config on both sides")
    for dev in ("r1", "r2"):
        run_cmds(rt=rt, device=dev, strict=False, commands=["end", "config", "no bgp", "end"])


def _established_check(device: str, peer_ip: str, label: str) -> dict:
    return {
        "device": device,
        "command": "show bgp neighbor af vpnv4",
        "contains": [peer_ip],
        "regex": [rf"(?im)^\s*{re.escape(peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
        "label": label,
    }


def _negotiated_check(device: str, peer_ip: str, label: str) -> dict:
    # 详情按 Local / Remote / Negotiated 三段列出地址族；要求 Negotiated 段含 vpnv4，
    # 非贪婪匹配确保命中的是 "Negotiated Address Families" 之后的 vpnv4 条目。
    return {
        "device": device,
        "command": f"show bgp neighbor af vpnv4 {peer_ip}",
        "contains": ["Negotiated Address Families", "afi=1 safi=128 (vpnv4)"],
        "regex": [r"(?s)Negotiated Address Families.*?afi=1 safi=128 \(vpnv4\)"],
        "label": label,
    }


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    # 直连链路上对端地址（r1 的邻居 = r2 的接口地址，反之亦然）
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        step("Configure eBGP base + vpnv4-only neighbors (v4 & v6) on r1/r2")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                f"neighbor {r1_peer_ip} as 65002",
                f"neighbor {r1_peer_ip6} as 65002",
                "af vpnv4",
                f"neighbor {r1_peer_ip} enable",
                f"neighbor {r1_peer_ip6} enable",
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
                "router-id 2.2.2.2",
                f"neighbor {r2_peer_ip} as 65001",
                f"neighbor {r2_peer_ip6} as 65001",
                "af vpnv4",
                f"neighbor {r2_peer_ip} enable",
                f"neighbor {r2_peer_ip6} enable",
                "exit",
                "end",
            ],
        )

        step("Wait vpnv4 eBGP sessions established (v4 & v6, both sides)")
        wait_checks(
            rt,
            [
                _established_check("r1", r1_peer_ip, "r1->r2 vpnv4 v4 established"),
                _established_check("r1", r1_peer_ip6, "r1->r2 vpnv4 v6 established"),
                _established_check("r2", r2_peer_ip, "r2->r1 vpnv4 v4 established"),
                _established_check("r2", r2_peer_ip6, "r2->r1 vpnv4 v6 established"),
            ],
            timeout=60,
        )

        step("Verify negotiated vpnv4 address-family in neighbor detail (v4 & v6)")
        wait_checks(
            rt,
            [
                _negotiated_check("r1", r1_peer_ip, "r1 negotiated vpnv4 af (v4 peer)"),
                _negotiated_check("r1", r1_peer_ip6, "r1 negotiated vpnv4 af (v6 peer)"),
                _negotiated_check("r2", r2_peer_ip, "r2 negotiated vpnv4 af (v4 peer)"),
                _negotiated_check("r2", r2_peer_ip6, "r2 negotiated vpnv4 af (v6 peer)"),
            ],
            timeout=30,
        )

        step("Verify running-config round-trips vpnv4 block on r1 (v4 & v6)")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "contains": [
                        "bgp 65001",
                        "af vpnv4",
                        f"neighbor {r1_peer_ip} enable",
                        f"neighbor {r1_peer_ip6} enable",
                    ],
                    "label": "r1 running-config has vpnv4 block (v4+v6)",
                },
            ],
            timeout=10,
        )

        print("VPNv4 basic check passed.")
    finally:
        _cleanup_case_config(rt)
