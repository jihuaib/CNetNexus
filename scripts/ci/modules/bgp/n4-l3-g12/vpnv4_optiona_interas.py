#!/usr/bin/env python3
"""VPNv4 inter-AS Option A（back-to-back VRF）端到端数据面验证（4 设备）。

Option A 拓扑：
    AS1 = 65001                inter-AS (VRF red)            AS2 = 65002
  r1(PE1) ─GE1/GE1─ r2(ASBR1) ─GE2/GE1─ r3(ASBR2) ─GE2/GE1─ r4(PE2)
   ISIS+LDP, iBGP vpnv4        私网 eBGP ipv4-unicast(纯 IP)   ISIS+LDP, iBGP vpnv4
   100.1.1.1@VRF red                                              100.4.4.4@VRF red

要点（对应用户对 Option A 的定义）：
- **域内 iBGP vpnv4**：PE↔ASBR 跑 iBGP vpnv4，nexthop=对端 loopback。iBGP 没有 eBGP 邻接
  假隧道(BGP_ADJ)，REMOTE_CROSS 路由的 nexthop 必须靠 **LDP** 候选解析（本用例核心）。
- **ASBR back-to-back VRF**：ASBR 把收到的 vpnv4 路由按 import-RT 插入私网 VRF red，再通过
  **私网 eBGP(ipv4-unicast)邻居**把该路由（纯 IP，无标签）发给对端 ASBR；对端 ASBR 把它当
  CE→PE 学习，再 re-originate 进自己域的 vpnv4。inter-AS 链路(r2-r3)接口属于 VRF red。
- 端到端：r1 的 VRF red loopback 100.1.1.1 ↔ r4 的 VRF red loopback 100.4.4.4 互通。

转发(以 r1→r4 为例，r1-r2 直连 1 跳，LDP implicit-null 即栈只含 VPN 标签)：
  r1: VRF red 查 100.4.4.4 → push [VPN标签(r2分配)] → r2
  r2: POP VPN标签 → VRF red 查 100.4.4.4 → 私网 eBGP 学到、nexthop=r3(直连) → 纯 IP → r3
  r3: VRF red 查 100.4.4.4 → vpnv4 学到、nexthop=r4 loopback(LDP) → push [VPN标签(r4分配)] → r4
  r4: POP VPN标签 → VRF red 查 100.4.4.4 → 本地 loopback 交付

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

# 每 AS 一套 RD/RT（Option A 下 RD 域内本地、RT 域内一致）
RT1 = "65001:100"
RT2 = "65002:100"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"
R3_NET = "49.0002.0000.0000.0003.00"
R4_NET = "49.0002.0000.0000.0004.00"


def _ping_ok(output: str) -> bool:
    if "bytes from" not in output:
        return False
    m = re.search(r"(\d+)%\s*packet loss", output)
    return not (m and int(m.group(1)) >= 100)


def _topology_if_addresses(device: str, ifname: str) -> list[str]:
    iface = g_top.devices[device].ifs[ifname]
    commands = [f"ip address {iface.ip} {iface.prefix}"]
    if iface.ip6:
        commands.append(f"ipv6 address {iface.ip6} {iface.prefix6}")
    return commands


def _restore_topology_if(device: str, ifname: str) -> list[str]:
    return [
        f"if {ifname}",
        "no ldp enable",
        f"no isis enable {TAG}",
        "no vrf forwarding",
        *_topology_if_addresses(device, ifname),
        "no shutdown",
        "exit",
    ]


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup Option A config on all four nodes")
    specs = (
        ("r1", ("GE-1",), (R1_LOOP, R1_CUST_LOOP)),
        ("r2", ("GE-1", "GE-2"), (R2_LOOP,)),
        ("r3", ("GE-1", "GE-2"), (R3_LOOP,)),
        ("r4", ("GE-1",), (R4_LOOP, R4_CUST_LOOP)),
    )
    for dev, ifaces, loops in specs:
        cmds = ["end", "config", "no bgp"]
        for ifname in ifaces:
            cmds += _restore_topology_if(dev, ifname)
        cmds += ["no ldp", f"no isis {TAG}"]
        for lp in loops:
            cmds.append(f"no if loop {lp}")
        cmds += [f"no vrf {VRF_NAME}", "end"]
        run_cmds(rt=rt, device=dev, commands=cmds, strict=False)


def _vrf_fib_tunnel_check(device: str, dst: str, label: str) -> dict:
    """对端私网前缀已在本 PE 的 VRF red FIB 安装为隧道路由 + 压 VPN 标签。"""
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


def _configure_vrf(rt: TopologyRuntime, device: str, *, rd: str, rt_val: str) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"route-distinguisher {rd}",
            "apply-label per-vrf",
            f"vpn-target {rt_val} export",
            f"vpn-target {rt_val} import",
            "exit",
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


def _configure_pe(rt: TopologyRuntime, device: str, *, local_as: int, rid: str, src_loop: int,
                  ibgp_peer: str, cust_loop: int, cust_addr: str) -> None:
    """PE：客户 loopback 入 VRF red + iBGP vpnv4(到 ASBR，loopback 源) + import-route connected。"""
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
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


def _configure_asbr(rt: TopologyRuntime, device: str, *, local_as: int, rid: str, src_loop: int,
                    ibgp_peer: str, interas_iface: str, interas_local: str, interas_prefix: int,
                    interas_peer: str, peer_as: int) -> None:
    """ASBR：inter-AS 接口入 VRF red + iBGP vpnv4(到本域 PE) + 私网 eBGP(到对端 ASBR)。"""
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if {interas_iface}",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {interas_local} {interas_prefix}",
            "exit",
            f"bgp {local_as}",
            f"router-id {rid}",
            f"neighbor {ibgp_peer} as {local_as}",
            f"neighbor {ibgp_peer} source-interface loop{src_loop}",
            "af vpnv4",
            f"neighbor {ibgp_peer} enable",
            "exit",
            f"vrf {VRF_NAME}",
            f"router-id {rid}",
            f"neighbor {interas_peer} as {peer_as}",
            "af ipv4-unicast",
            f"neighbor {interas_peer} enable",
            "exit",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3", "r4"))

    # inter-AS 链路（r2 GE-2 <-> r3 GE-1）专用 VRF red 子网（绑入 VRF，不复用公网自动地址）
    r2r3_local = "10.23.0.1"   # r2 GE-2 in VRF red
    r2r3_peer = "10.23.0.2"    # r3 GE-1 in VRF red（r2 的私网 eBGP 邻居）
    r3r2_local = "10.23.0.2"   # r3 GE-1 in VRF red
    r3r2_peer = "10.23.0.1"    # r2 GE-2 in VRF red（r3 的私网 eBGP 邻居）
    r2r3_prefix = 30

    try:
        _cleanup(rt)

        step("域内 IGP+LDP：r1/r2(AS1 core GE-1)、r3/r4(AS2 core: r3 GE-2 / r4 GE-1)")
        _configure_core(rt, "r1", net=R1_NET, lsr_id=R1_ID, loop_id=R1_LOOP, loop_v4=R1_ID, core_iface="GE-1")
        _configure_core(rt, "r2", net=R2_NET, lsr_id=R2_ID, loop_id=R2_LOOP, loop_v4=R2_ID, core_iface="GE-1")
        _configure_core(rt, "r3", net=R3_NET, lsr_id=R3_ID, loop_id=R3_LOOP, loop_v4=R3_ID, core_iface="GE-2")
        _configure_core(rt, "r4", net=R4_NET, lsr_id=R4_ID, loop_id=R4_LOOP, loop_v4=R4_ID, core_iface="GE-1")

        step("四节点创建 VRF red（RD/RT；AS1 用 RT1，AS2 用 RT2）")
        _configure_vrf(rt, "r1", rd="65001:1", rt_val=RT1)
        _configure_vrf(rt, "r2", rd="65001:2", rt_val=RT1)
        _configure_vrf(rt, "r3", rd="65002:3", rt_val=RT2)
        _configure_vrf(rt, "r4", rd="65002:4", rt_val=RT2)

        step("配置 PE(r1/r4) 与 ASBR(r2/r3)")
        _configure_pe(rt, "r1", local_as=AS1, rid=R1_ID, src_loop=R1_LOOP, ibgp_peer=R2_ID,
                      cust_loop=R1_CUST_LOOP, cust_addr=R1_CUST)
        _configure_asbr(rt, "r2", local_as=AS1, rid=R2_ID, src_loop=R2_LOOP, ibgp_peer=R1_ID,
                        interas_iface="GE-2", interas_local=r2r3_local, interas_prefix=r2r3_prefix,
                        interas_peer=r2r3_peer, peer_as=AS2)
        _configure_asbr(rt, "r3", local_as=AS2, rid=R3_ID, src_loop=R3_LOOP, ibgp_peer=R4_ID,
                        interas_iface="GE-1", interas_local=r3r2_local, interas_prefix=r2r3_prefix,
                        interas_peer=r3r2_peer, peer_as=AS1)
        _configure_pe(rt, "r4", local_as=AS2, rid=R4_ID, src_loop=R4_LOOP, ibgp_peer=R3_ID,
                      cust_loop=R4_CUST_LOOP, cust_addr=R4_CUST)

        step("等待域内 ISIS/LDP 收敛（PE 经 LDP 可达 ASBR loopback）")
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

        step("等待 BGP 会话：域内 iBGP vpnv4 + inter-AS 私网 eBGP")
        wait_checks(
            rt,
            [
                {"device": "r1", "command": "show bgp neighbor af vpnv4",
                 "regex": [rf"(?im)^\s*{re.escape(R2_ID)}\s+\S+\s+\S+\s+Established\s*$"],
                 "label": "r1 iBGP vpnv4 to r2 established"},
                {"device": "r4", "command": "show bgp neighbor af vpnv4",
                 "regex": [rf"(?im)^\s*{re.escape(R3_ID)}\s+\S+\s+\S+\s+Established\s*$"],
                 "label": "r4 iBGP vpnv4 to r3 established"},
                {"device": "r2", "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                 "regex": [rf"(?im)^\s*{re.escape(r2r3_peer)}\s+\S+\s+\S+\s+Established\s*$"],
                 "label": "r2 inter-AS eBGP(VRF) to r3 established"},
                {"device": "r3", "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                 "regex": [rf"(?im)^\s*{re.escape(r3r2_peer)}\s+\S+\s+\S+\s+Established\s*$"],
                 "label": "r3 inter-AS eBGP(VRF) to r2 established"},
            ],
            timeout=90,
            interval=3,
        )

        step("端到端路由传播：两端客户前缀均到达对端 PE 的 VRF red")
        wait_checks(
            rt,
            [
                # r1 的 100.1.1.1 一路传到 r4；r4 的 100.4.4.4 一路传到 r1
                {"device": "r4", "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                 "contains": [R1_CUST], "label": "r4 VRF red learned r1 cust 100.1.1.1"},
                {"device": "r1", "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                 "contains": [R4_CUST], "label": "r1 VRF red learned r4 cust 100.4.4.4"},
                # ASBR 私网中转：r2/r3 VRF red 都应有对端客户前缀
                {"device": "r2", "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                 "contains": [R1_CUST, R4_CUST], "label": "r2 ASBR VRF has both cust prefixes"},
                {"device": "r3", "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                 "contains": [R1_CUST, R4_CUST], "label": "r3 ASBR VRF has both cust prefixes"},
            ],
            timeout=90,
            interval=3,
        )

        step("PE 转发表：对端客户前缀经隧道(LDP)+VPN 标签装入 VRF red FIB")
        wait_checks(
            rt,
            [
                _vrf_fib_tunnel_check("r4", R1_CUST, "r4 FIB 100.1.1.1 via tunnel+label"),
                _vrf_fib_tunnel_check("r1", R4_CUST, "r1 FIB 100.4.4.4 via tunnel+label"),
            ],
            timeout=60,
            interval=3,
        )

        step("数据面 ping：r1 客户 loopback <-> r4 客户 loopback（穿越 Option A 全程）")

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
            raise RuntimeError("r1 -> r4 Option A L3VPN ping failed (no reply / 100% loss)")

        if not _ping("r4", R1_CUST, R4_CUST):
            raise RuntimeError("r4 -> r1 Option A L3VPN ping failed (no reply / 100% loss)")

        print("VPNv4 inter-AS Option A end-to-end data-plane ping (bidirectional) check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
