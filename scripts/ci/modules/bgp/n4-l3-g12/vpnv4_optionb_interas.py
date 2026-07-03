#!/usr/bin/env python3
"""VPNv4 inter-AS Option B（ASBR-ASBR eBGP vpnv4，no policy vpn-target）端到端验证（4 设备）。

Option B 拓扑：
    AS1 = 65001               inter-AS                     AS2 = 65002
  r1(PE1) ─GE1/GE1─ r2(ASBR1) ═GE2/GE1═ r3(ASBR2) ─GE2/GE1─ r4(PE2)
   ISIS+LDP, iBGP vpnv4     eBGP vpnv4 (ASBR↔ASBR)        ISIS+LDP, iBGP vpnv4
   100.1.1.1@VRF red        ASBR 无 VRF / no policy vpn-target   100.4.4.4@VRF red

要点（对应用户对 Option B 的定义）：
- **ASBR 无 VRF**：r2/r3 不配客户 VRF，只是 vpnv4 中转。它们之间用 **eBGP vpnv4** 直连邻居
  （inter-AS 链路 r2-r3 公网地址，非 VRF）。
- **no policy vpn-target**：ASBR 没有任何 import-RT，默认 vpn-target 过滤会把收到的 vpnv4 路由
  全部丢弃；必须在 ``af vpnv4`` 下配 ``no policy vpn-target`` 才能接受并中转（本用例核心）。
- 域内 PE↔ASBR 跑 iBGP vpnv4，域内 ISIS+LDP 提供 IGP + 传输标签。

验证：
1. BGP 会话：域内 iBGP vpnv4 + inter-AS eBGP vpnv4 均 Established。
2. **控制面中转（no policy vpn-target 的价值）**：ASBR 接受并中转 vpnv4 路由，对端客户前缀
   一路传到对端 PE 的 VRF red。
3. 端到端数据面 ping：r1 客户 loopback ↔ r4 客户 loopback。

本用例由 scripts/ci/module_runner.py 加载（拓扑 n4-l3-g12）。
"""

from __future__ import annotations

import re
import time

from module_api import (  # noqa: E402
    g_top,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
TAG = 1
AS1 = 65001
AS2 = 65002

# LSR / BGP router-id loopback（每设备一个，参与 ISIS/LDP/iBGP）
R1_LOOP, R1_ID = 11, "1.1.1.1"
R2_LOOP, R2_ID = 22, "2.2.2.2"
R3_LOOP, R3_ID = 33, "3.3.3.3"
R4_LOOP, R4_ID = 44, "4.4.4.4"

# 客户私网 loopback（VRF red 内，作为端到端两端）
R1_CUST_LOOP, R1_CUST = 110, "100.1.1.1"
R4_CUST_LOOP, R4_CUST = 140, "100.4.4.4"
LEN = 32

# 每 AS 一套 RT（两端 PE 的 VRF red 必须 import 对端 export 的 RT 才能落 VRF；
# Option B 跨 AS 直传 vpnv4，RT 在两端 PE 间端到端一致即可命中导入）
RT = "65000:100"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"
R3_NET = "49.0002.0000.0000.0003.00"
R4_NET = "49.0002.0000.0000.0004.00"

# inter-AS 链路（r2 GE-2 <-> r3 GE-1）公网地址（top.yaml 自动配置，eBGP vpnv4 直连）
R2R3_PEER = "10.23.0.2"  # r3 GE-1（r2 的 eBGP vpnv4 邻居）
R3R2_PEER = "10.23.0.1"  # r2 GE-2（r3 的 eBGP vpnv4 邻居）


def _ping_ok(output: str) -> bool:
    if "bytes from" not in output:
        return False
    m = re.search(r"(\d+)%\s*packet loss", output)
    return not (m and int(m.group(1)) >= 100)


def _vrf_fib_tunnel_check(device: str, dst: str, label: str) -> dict[str, object]:
    """对端私网前缀已在本 PE 的 VRF red FIB 安装为 tunnel + VPN label。"""
    return {
        "device": device,
        "command": f"show fib ipv4 vrf {VRF_NAME} {dst} {LEN}",
        "contains": [f"Routing entry for {dst}/{LEN}"],
        "regex": [
            r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
            r"(?im)^\s*Out-Label\s*:\s*[1-9]\d*\s*$",
            r"(?im)^\s*Installed\s*:\s*yes\s*$",
        ],
        "label": label,
    }


def _ldp_ftn_check(device: str, endpoint: str, label: str) -> dict[str, object]:
    return {
        "device": device,
        "command": "show tunnel ftn",
        "regex": [
            rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+fec\s+{re.escape(endpoint)}/{LEN}\s+"
            r"->\s+nhlfe\s+[1-9]\d*\s+src\s+ldp\s+state\s+up\s*$"
        ],
        "label": label,
    }


def _bgp_adj_candidate_check(device: str, endpoint: str, label: str) -> dict[str, object]:
    return {
        "device": device,
        "command": "show tunnel candidate",
        "regex": [
            rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+endpoint\s+{re.escape(endpoint)}\s+nh\s+-\s+"
            rf"relay\s+{re.escape(endpoint)}\s+oif\s+\d+\s+src\s+bgp-adj\s+pref\s+10\s+labels\s+\[\]\s*$"
        ],
        "label": label,
    }


def _mpls_action_checks(device: str, action: str, label: str) -> list[dict[str, object]]:
    action_id = {"swap": 2, "pop": 3}[action]
    os_action = r"swap\[[1-9]\d*(?:,[1-9]\d*)*\]" if action == "swap" else "pop"
    return [
        {
            "device": device,
            "command": "show tunnel ilm",
            "regex": [
                rf"(?im)^\s*vrf\s+0\s+label\s+[1-9]\d+\s+->\s+(?:nhlfe\s+(?:0|[1-9]\d*)|direct)\s+"
                rf"action\s+{action}\({action_id}\)\s+state\s+up\s*$"
            ],
            "label": f"{label}: tunnel ILM {action.upper()} programmed",
        },
        {
            "device": device,
            "command": "show fib mpls",
            "regex": [
                rf"(?im)^\s*0\s+[1-9]\d*\s+{action}\s+(?:0|[1-9]\d*|-)\s+up\s+\d+\s+\S+\s+"
                r"(?:[1-9]\d*|-)\s+yes\s+yes\s*$"
            ],
            "label": f"{label}: FIB MPLS {action.upper()} installed",
        },
        {
            "device": device,
            "command": "show fib mpls os",
            "regex": [
                rf"(?im)^\s*main\s+unicast\s+[1-9]\d*\s+\S+\s+\S+\s+static\s+\d+\s+{os_action}\s*$"
            ],
            "label": f"{label}: Linux MPLS route {action.upper()} installed",
        },
    ]


def _dump_tunnel_state(rt: TopologyRuntime) -> None:
    for dev in ("r1", "r2", "r3", "r4"):
        for cmd in (
            "show tunnel candidate",
            "show tunnel nhlfe",
            "show tunnel ftn",
            "show tunnel ilm",
            "show tunnel label",
            "show fib mpls",
            "show fib mpls os",
        ):
            out = rt.exec_cmd(dev, cmd, timeout=15)
            print(f"\n===== {dev}: {cmd} =====\n{out}", flush=True)


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup Option B config on all four nodes")
    specs = (
        ("r1", ("GE-1",), (R1_LOOP, R1_CUST_LOOP)),
        ("r2", ("GE-1",), (R2_LOOP,)),
        ("r3", ("GE-2",), (R3_LOOP,)),
        ("r4", ("GE-1",), (R4_LOOP, R4_CUST_LOOP)),
    )
    for dev, ifaces, loops in specs:
        cmds = ["end", "config", "no bgp"]
        for ifname in ifaces:
            cmds += [f"if {ifname}", "no ldp enable", f"no isis enable {TAG}", "exit"]
        cmds += ["no ldp", f"no isis {TAG}"]
        for lp in loops:
            cmds.append(f"no if loop {lp}")
        cmds += [f"no vrf {VRF_NAME}", "end"]
        run_cmds(rt=rt, device=dev, commands=cmds, strict=False)


def _configure_core(rt: TopologyRuntime, device: str, *, net: str, lsr_id: str, loop_id: int, loop_v4: str,
                    core_iface: str) -> None:
    """域内 IGP+LDP：LSR loopback + ISIS（core 链路 + loopback passive）+ LDP（core 链路）。"""
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if loop {loop_id}",
            f"ip address {loop_v4} {LEN}",
            "exit",
            f"isis {TAG}",
            f"net {net}",
            "is-type level-1-2",
            "cost-style wide",
            "af ipv4",
            "exit",
            f"if {core_iface}",
            f"isis enable {TAG}",
            f"isis hello-interval {TAG} 3",
            f"isis hold-multiplier {TAG} 3",
            "exit",
            f"if loop {loop_id}",
            f"isis enable {TAG}",
            f"isis passive {TAG}",
            "exit",
            "ldp",
            f"lsr-id {lsr_id}",
            "hello-interval 1000",
            "hold-time 3000",
            "keepalive-interval 3000",
            "exit",
            f"if {core_iface}",
            "ldp enable",
            "exit",
            "end",
        ],
    )


def _configure_pe(rt: TopologyRuntime, device: str, *, local_as: int, rid: str, src_loop: int, ibgp_peer: str,
                  cust_loop: int, cust_addr: str, rd: str) -> None:
    """PE：VRF red(RD/RT 两端一致) + 客户 loopback 入 VRF + iBGP vpnv4(到本域 ASBR) + import-route connected。"""
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4",
            f"route-distinguisher {rd}",
            "apply-label per-vrf",
            f"vpn-target {RT} export",
            f"vpn-target {RT} import",
            "exit",
            "exit",
            f"if loop {cust_loop}",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {cust_addr} {LEN}",
            "exit",
            f"bgp {local_as}",
            f"router-id {rid}",
            f"neighbor {ibgp_peer} as {local_as}",
            f"neighbor {ibgp_peer} source-interface loop{src_loop}",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "import-route connected",
            "exit",
            "exit",
            "af vpnv4",
            f"neighbor {ibgp_peer} enable",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [{"device": device, "command": f"show vrf name {VRF_NAME}",
          "contains": ["VRF Detail:", f"Name           : {VRF_NAME}"], "label": f"{device} vrf {VRF_NAME} ready"}],
        timeout=15,
    )


def _configure_asbr(rt: TopologyRuntime, device: str, *, local_as: int, rid: str, src_loop: int, ibgp_peer: str,
                    ebgp_peer: str, peer_as: int) -> None:
    """ASBR：无 VRF；iBGP vpnv4(到本域 PE) + eBGP vpnv4(到对端 ASBR) + no policy vpn-target。"""
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"bgp {local_as}",
            f"router-id {rid}",
            # iBGP vpnv4 到本域 PE（源 loopback）
            f"neighbor {ibgp_peer} as {local_as}",
            f"neighbor {ibgp_peer} source-interface loop{src_loop}",
            # eBGP vpnv4 到对端 ASBR（inter-AS 直连公网地址）
            f"neighbor {ebgp_peer} as {peer_as}",
            "af vpnv4",
            "no policy vpn-target",
            f"neighbor {ibgp_peer} enable",
            f"neighbor {ebgp_peer} enable",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3", "r4"))

    try:
        _cleanup(rt)

        step("域内 IGP+LDP：r1/r2(AS1 core GE-1)、r3/r4(AS2 core: r3 GE-2 / r4 GE-1)")
        _configure_core(rt, "r1", net=R1_NET, lsr_id=R1_ID, loop_id=R1_LOOP, loop_v4=R1_ID, core_iface="GE-1")
        _configure_core(rt, "r2", net=R2_NET, lsr_id=R2_ID, loop_id=R2_LOOP, loop_v4=R2_ID, core_iface="GE-1")
        _configure_core(rt, "r3", net=R3_NET, lsr_id=R3_ID, loop_id=R3_LOOP, loop_v4=R3_ID, core_iface="GE-2")
        _configure_core(rt, "r4", net=R4_NET, lsr_id=R4_ID, loop_id=R4_LOOP, loop_v4=R4_ID, core_iface="GE-1")

        step("配置 PE(r1/r4，含 VRF red) 与 ASBR(r2/r3，无 VRF + no policy vpn-target)")
        _configure_pe(rt, "r1", local_as=AS1, rid=R1_ID, src_loop=R1_LOOP, ibgp_peer=R2_ID,
                      cust_loop=R1_CUST_LOOP, cust_addr=R1_CUST, rd="65001:1")
        _configure_asbr(rt, "r2", local_as=AS1, rid=R2_ID, src_loop=R2_LOOP, ibgp_peer=R1_ID,
                        ebgp_peer=R2R3_PEER, peer_as=AS2)
        _configure_asbr(rt, "r3", local_as=AS2, rid=R3_ID, src_loop=R3_LOOP, ibgp_peer=R4_ID,
                        ebgp_peer=R3R2_PEER, peer_as=AS1)
        _configure_pe(rt, "r4", local_as=AS2, rid=R4_ID, src_loop=R4_LOOP, ibgp_peer=R3_ID,
                      cust_loop=R4_CUST_LOOP, cust_addr=R4_CUST, rd="65002:4")

        step("等待域内 ISIS/LDP 收敛（PE 经 LDP 可达本域 ASBR loopback）")
        wait_checks(
            rt,
            [
                {"device": "r1", "command": f"show isis neighbor {TAG}", "contains": ["GE-1", "Up"],
                 "label": "r1 ISIS up"},
                {"device": "r4", "command": f"show isis neighbor {TAG}", "contains": ["GE-1", "Up"],
                 "label": "r4 ISIS up"},
                {"device": "r1", "command": "show ldp neighbor", "contains": [R2_ID, "OPERATIONAL"],
                 "label": "r1 LDP to r2 operational"},
                {"device": "r4", "command": "show ldp neighbor", "contains": [R3_ID, "OPERATIONAL"],
                 "label": "r4 LDP to r3 operational"},
            ],
            timeout=120,
            interval=3,
        )

        step("等待 BGP 会话：域内 iBGP vpnv4 + inter-AS eBGP vpnv4(ASBR↔ASBR)")
        wait_checks(
            rt,
            [
                {"device": "r1", "command": "show bgp neighbor af vpnv4",
                 "regex": [rf"(?im)^\s*{re.escape(R2_ID)}\s+\S+\s+\S+\s+Established\s*$"],
                 "label": "r1 iBGP vpnv4 to r2 established"},
                {"device": "r4", "command": "show bgp neighbor af vpnv4",
                 "regex": [rf"(?im)^\s*{re.escape(R3_ID)}\s+\S+\s+\S+\s+Established\s*$"],
                 "label": "r4 iBGP vpnv4 to r3 established"},
                {"device": "r2", "command": "show bgp neighbor af vpnv4",
                 "regex": [rf"(?im)^\s*{re.escape(R2R3_PEER)}\s+\S+\s+\S+\s+Established\s*$"],
                 "label": "r2 eBGP vpnv4 to r3 established"},
                {"device": "r3", "command": "show bgp neighbor af vpnv4",
                 "regex": [rf"(?im)^\s*{re.escape(R3R2_PEER)}\s+\S+\s+\S+\s+Established\s*$"],
                 "label": "r3 eBGP vpnv4 to r2 established"},
            ],
            timeout=90,
            interval=3,
        )

        step("控制面中转（no policy vpn-target 核心）：ASBR vpnv4 表收到对端客户前缀")
        wait_checks(
            rt,
            [
                # r2(ASBR1) 无 VRF，靠 no policy vpn-target 接受 r1 本域导出 + r3 跨 AS 传来的 vpnv4
                {"device": "r2", "command": "show bgp route af vpnv4",
                 "contains": [R1_CUST, R4_CUST], "label": "r2 ASBR relays both cust prefixes in vpnv4"},
                {"device": "r3", "command": "show bgp route af vpnv4",
                 "contains": [R1_CUST, R4_CUST], "label": "r3 ASBR relays both cust prefixes in vpnv4"},
            ],
            timeout=90,
            interval=3,
        )

        step("控制面端到端：对端客户前缀传到对端 PE 的 VRF red")
        wait_checks(
            rt,
            [
                {"device": "r4", "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                 "contains": [R1_CUST], "label": "r4 VRF red learned r1 cust 100.1.1.1 (via ASBR relay)"},
                {"device": "r1", "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                 "contains": [R4_CUST], "label": "r1 VRF red learned r4 cust 100.4.4.4 (via ASBR relay)"},
            ],
            timeout=90,
            interval=3,
        )

        step("Option B 隧道与 MPLS 转发表：PE tunnel FIB、ASBR BGP_ADJ/SWAP、FIB MPLS/OS")
        mpls_checks = [
            _vrf_fib_tunnel_check("r1", R4_CUST, "r1 VRF FIB to r4 cust via tunnel+VPN label"),
            _vrf_fib_tunnel_check("r4", R1_CUST, "r4 VRF FIB to r1 cust via tunnel+VPN label"),
            _ldp_ftn_check("r1", R2_ID, "r1 LDP tunnel to ASBR1 loopback"),
            _ldp_ftn_check("r4", R3_ID, "r4 LDP tunnel to ASBR2 loopback"),
            _bgp_adj_candidate_check("r2", R2R3_PEER, "r2 BGP_ADJ tunnel candidate to ASBR2"),
            _bgp_adj_candidate_check("r3", R3R2_PEER, "r3 BGP_ADJ tunnel candidate to ASBR1"),
        ]
        mpls_checks.extend(_mpls_action_checks("r1", "pop", "r1 PE local VPN label"))
        mpls_checks.extend(_mpls_action_checks("r2", "swap", "r2 ASBR Option B transit label"))
        mpls_checks.extend(_mpls_action_checks("r3", "swap", "r3 ASBR Option B transit label"))
        mpls_checks.extend(_mpls_action_checks("r4", "pop", "r4 PE local VPN label"))
        wait_checks(rt, mpls_checks, timeout=90, interval=3)

        step("数据面诊断 dump（vpnv4 / VRF route / FIB / tunnel / MPLS FIB / MPLS OS）")
        for dev, cmd in (
            ("r1", f"show bgp route af ipv4-unicast vrf {VRF_NAME} {R4_CUST} {LEN}"),
            ("r1", f"show route ipv4 vrf {VRF_NAME} {R4_CUST} {LEN}"),
            ("r1", f"show fib ipv4 vrf {VRF_NAME} {R4_CUST} {LEN}"),
            ("r2", "show bgp route af vpnv4"),
            ("r3", "show bgp route af vpnv4"),
            ("r4", f"show bgp route af ipv4-unicast vrf {VRF_NAME} {R1_CUST} {LEN}"),
            ("r4", f"show route ipv4 vrf {VRF_NAME} {R1_CUST} {LEN}"),
            ("r4", f"show fib ipv4 vrf {VRF_NAME} {R1_CUST} {LEN}"),
        ):
            out = rt.exec_cmd(dev, cmd, timeout=15)
            print(f"\n===== {dev}: {cmd} =====\n{out}", flush=True)
        _dump_tunnel_state(rt)

        step("数据面 ping：r1 客户 loopback <-> r4 客户 loopback（穿越 Option B 全程）")

        def _ping(src_dev: str, dst: str, src: str) -> bool:
            out = rt.exec_cmd(src_dev, f"ping {dst} -a {src} vrf {VRF_NAME}", timeout=25)
            print(out, flush=True)
            return _ping_ok(out)

        ok = False
        deadline = time.time() + 40
        while time.time() < deadline:
            if _ping("r1", R4_CUST, R1_CUST):
                ok = True
                break
            time.sleep(4)
        if not ok:
            raise RuntimeError("r1 -> r4 Option B L3VPN ping failed (no reply / 100% loss)")

        if not _ping("r4", R1_CUST, R4_CUST):
            raise RuntimeError("r4 -> r1 Option B L3VPN ping failed (no reply / 100% loss)")

        print("VPNv4 inter-AS Option B end-to-end data-plane ping (bidirectional) check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
