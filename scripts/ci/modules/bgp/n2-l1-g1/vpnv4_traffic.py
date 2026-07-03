#!/usr/bin/env python3
"""VPNv4（MPLS L3VPN）数据面端到端 ping 流量验证。

与 ``vpnv4_route_exchange.py``（控制面：导出/导入/过滤/显示）互补，本用例搭建 **对称** L3VPN：
r1、r2 各在 VRF red 内放一个 loopback，互相 export+import（同一 RT），双向都形成「经 eBGP-vpnv4
邻接假隧道 + 压私网 VPN 标签」的转发表项，然后从一端 loopback ping 另一端 loopback，验证真实
数据面转发（入口压标签 / 出口 POP 后按标签 demux 进对端 VRF）。

拓扑：r1(GE-1 10.12.0.1/30) 直连 r2(GE-1 10.12.0.2/30)，公网 eBGP 65001<->65002 跑 vpnv4。
私网：r1 VRF red loop 100.1.1.1/32，r2 VRF red loop 100.2.2.2/32，二者 RT 相同（互相导入）。

转发路径（以 r1 ping r2 为例）：
- echo request：r1 VRF red 查 100.2.2.2 → push r2 通告的 VPN 标签 → GE-1 送 r2；
  r2 收到 MPLS 标签 → POP → VRF red 查 100.2.2.2 → 本地 loopback 收。
- echo reply：r2 VRF red 查 100.1.1.1 → push r1 通告的 VPN 标签 → 送 r1；r1 POP → VRF red → 本地收。

本用例由 scripts/ci/module_runner.py 加载。
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
R1_AS = 65001
R2_AS = 65002
R1_RD = "65001:1"
R2_RD = "65002:1"
RT = "65001:100"  # 两端一致才能互相导入

R1_LOOP_IDX = 10
R1_LOOP_ADDR = "100.1.1.1"
R2_LOOP_IDX = 10
R2_LOOP_ADDR = "100.2.2.2"
LOOP_LEN = 32


def _fib_installed_check(device: str, dst: str, label: str) -> dict:
    """对端私网前缀已在本设备 VRF red FIB 安装为隧道路由且压 VPN 标签。"""
    return {
        "device": device,
        "command": f"show fib ipv4 vrf {VRF_NAME} {dst} {LOOP_LEN}",
        "contains": [f"Routing entry for {dst}/{LOOP_LEN}"],
        "regex": [
            r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
            r"(?im)^\s*Out-Label\s*:\s*[1-9]\d*\s*$",
            r"(?im)^\s*Installed\s*:\s*yes\s*$",
        ],
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
                f"no if loop {R1_LOOP_IDX}",
                f"no vrf {VRF_NAME}",
                "end",
            ],
        )


def _setup_pe(rt: TopologyRuntime, device: str, *, local_as: int, rd: str, peer_ip: str, remote_as: int,
              loop_idx: int, loop_addr: str) -> None:
    """配置一个 PE：VRF red(RD+RT export/import+per-vrf 标签) + loopback + 公网 eBGP + vpnv4 邻居。"""
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
            f"if loop {loop_idx}",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {loop_addr} {LOOP_LEN}",
            "exit",
            f"bgp {local_as}",
            f"router-id {loop_addr}",
            f"neighbor {peer_ip} as {remote_as}",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "import-route connected",
            "exit",
            "exit",
            "af vpnv4",
            f"neighbor {peer_ip} enable",
            "exit",
            "end",
        ],
    )


def _ping_ok(output: str) -> bool:
    """ping 输出是否表示成功（有回应 + 非 100% 丢包）。"""
    if "bytes from" not in output:
        return False
    m = re.search(r"(\d+)%\s*packet loss", output)
    if m and int(m.group(1)) >= 100:
        return False
    return True


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_to_r2 = str(g_top.r1.GE_1.peer_ip)  # r2 公网地址（r1 的邻居）
    r2_to_r1 = str(g_top.r2.GE_1.peer_ip)  # r1 公网地址（r2 的邻居）

    try:
        _cleanup(rt)

        step("对称配置两端 PE（VRF red + loopback + 公网 eBGP + vpnv4 邻居）")
        _setup_pe(rt, "r1", local_as=R1_AS, rd=R1_RD, peer_ip=r1_to_r2, remote_as=R2_AS,
                  loop_idx=R1_LOOP_IDX, loop_addr=R1_LOOP_ADDR)
        _setup_pe(rt, "r2", local_as=R2_AS, rd=R2_RD, peer_ip=r2_to_r1, remote_as=R1_AS,
                  loop_idx=R2_LOOP_IDX, loop_addr=R2_LOOP_ADDR)
        time.sleep(2)

        step("等待 vpnv4 eBGP 会话 Established")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af vpnv4",
                    "regex": [rf"(?im)^\s*{re.escape(r1_to_r2)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 vpnv4 established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af vpnv4",
                    "regex": [rf"(?im)^\s*{re.escape(r2_to_r1)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2->r1 vpnv4 established",
                },
            ],
            timeout=60,
        )

        step("双向：对端私网 loopback 已在本端 VRF red FIB 安装为隧道路由（压 VPN 标签）")
        wait_checks(
            rt,
            [
                _fib_installed_check("r1", R2_LOOP_ADDR, "r1 installed r2 loop via tunnel+label"),
                _fib_installed_check("r2", R1_LOOP_ADDR, "r2 installed r1 loop via tunnel+label"),
            ],
            timeout=40,
        )

        step("数据面 ping：r1 VRF red loopback -> r2 VRF red loopback（经 MPLS L3VPN 转发）")

        def _ping_check() -> bool:
            out = rt.exec_cmd("r1", f"ping {R2_LOOP_ADDR} -a {R1_LOOP_ADDR} vrf {VRF_NAME}", timeout=25)
            print(out, flush=True)
            return _ping_ok(out)

        ok = False
        deadline = time.time() + 30
        while time.time() < deadline:
            if _ping_check():
                ok = True
                break
            time.sleep(3)
        if not ok:
            raise RuntimeError("r1 -> r2 VRF red L3VPN ping failed (no reply / 100% loss)")

        step("反向 ping：r2 VRF red loopback -> r1 VRF red loopback")
        out = rt.exec_cmd("r2", f"ping {R1_LOOP_ADDR} -a {R2_LOOP_ADDR} vrf {VRF_NAME}", timeout=25)
        print(out, flush=True)
        if not _ping_ok(out):
            raise RuntimeError("r2 -> r1 VRF red L3VPN ping failed (no reply / 100% loss)")

        print("VPNv4 L3VPN data-plane ping (bidirectional) check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
