#!/usr/bin/env python3
"""
BGP VRF neighbor 建立验证（IPv4 + IPv6）。

覆盖范围：
- r1/r2 各自创建 VRF "red"，给 VRF 配置 IPv4/IPv6 AF + RD，
  把 GE-1 绑入 VRF 并在 VRF 内配置直连 IPv4/IPv6。
- BGP 进入 ``vrf red`` 子视图，分别配置 IPv4 / IPv6 neighbor。
- 等待两侧 IPv4 + IPv6 BGP 会话进入 Established。
- 验证 ``show bgp neighbor af ipv4-unicast vrf red`` /
  ``show bgp neighbor af ipv6-unicast vrf red`` 能列出 VRF 内的邻居；
  公网视图中不应出现这些 VRF 邻居。
"""

from __future__ import annotations

import re
import time

from module_api import (  # noqa: E402
    cmd,
    g_top,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
GE_IF = "GE-1"

# 给 VRF 重新分配的接口子网（绑入 VRF 时原 IP 会被清空）
R1_V4 = "10.99.0.1"
R2_V4 = "10.99.0.2"
V4_LEN = 30

R1_V6 = "2001:db8:99::1"
R2_V6 = "2001:db8:99::2"
V6_LEN = 64


def _cleanup(rt: TopologyRuntime, base: dict[str, dict[str, str | int]]) -> None:
    """Best-effort 复位：拆 BGP / VRF 绑定，还原公网接口地址。"""
    for dev in ("r1", "r2"):
        b = base[dev]
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                "no bgp",
                f"if {GE_IF}",
                "no shutdown",
                f"no ip address {R1_V4} {V4_LEN}" if dev == "r1" else f"no ip address {R2_V4} {V4_LEN}",
                f"no ipv6 address {R1_V6} {V6_LEN}" if dev == "r1" else f"no ipv6 address {R2_V6} {V6_LEN}",
                "no vrf forwarding",
                f"ip address {b['v4']} {b['v4_len']}",
                f"ipv6 address {b['v6']} {b['v6_len']}",
                "exit",
                f"no vrf {VRF_NAME}",
                "end",
            ],
        )


def _setup_vrf_and_address(
    rt: TopologyRuntime,
    *,
    device: str,
    local_v4: str,
    local_v6: str,
    rd_v4: str,
    rd_v6: str,
) -> None:
    """建 VRF（含 IPv4/IPv6 AF + RD）、绑接口、配 IP，等待直连路由就绪。

    BGP 进入 ``vrf <name> / af ipv*-unicast`` 时会校验该 VRF 在 VRF 模块下
    对应 AF 是否已经配置 RD（route-distinguisher），未配置会直接报错。
    """
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"route-distinguisher {rd_v4}",
            "exit",
            "af ipv6-unicast",
            f"route-distinguisher {rd_v6}",
            "exit",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device=device,
        command=f"show vrf name {VRF_NAME}",
        timeout=10,
        interval=1,
        contains=["VRF Detail:", f"Name           : {VRF_NAME}"],
        label=f"{device} vrf {VRF_NAME} ready",
    )
    # VRF_ADD/AF_ENABLE/AF_RD_ADD 事件需要传播到 BGP/IF 的 VRF cache
    time.sleep(2)

    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {local_v4} {V4_LEN}",
            f"ipv6 address {local_v6} {V6_LEN}",
            "exit",
            "end",
        ],
    )


def _wait_dual_stack_session(
    rt: TopologyRuntime,
    *,
    device: str,
    peer_v4: str,
    peer_v6: str,
    timeout: int = 60,
) -> None:
    """等待 device 视角 VRF red 内 IPv4 + IPv6 会话进入 Established。"""
    checks = [
        {
            "device": device,
            "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
            "contains": [peer_v4, f"AF: ipv4-unicast"],
            "regex": [rf"(?im)^\s*{re.escape(peer_v4)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": f"{device} vrf={VRF_NAME} ipv4 session up",
        },
        {
            "device": device,
            "command": f"show bgp neighbor af ipv6-unicast vrf {VRF_NAME}",
            "contains": [peer_v6, f"AF: ipv6-unicast"],
            "regex": [rf"(?im)^\s*{re.escape(peer_v6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": f"{device} vrf={VRF_NAME} ipv6 session up",
        },
    ]
    wait_checks(rt, checks, timeout=timeout)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    base = {
        "r1": {
            "v4": str(g_top.r1.GE_1.ip),
            "v4_len": int(g_top.r1.GE_1.prefix),
            "v6": str(g_top.r1.GE_1.ip6),
            "v6_len": int(g_top.r1.GE_1.prefix6),
        },
        "r2": {
            "v4": str(g_top.r2.GE_1.ip),
            "v4_len": int(g_top.r2.GE_1.prefix),
            "v6": str(g_top.r2.GE_1.ip6),
            "v6_len": int(g_top.r2.GE_1.prefix6),
        },
    }

    try:
        _cleanup(rt, base)

        step("两端创建 VRF red（含 IPv4/IPv6 AF + RD）并把 GE-1 绑入 VRF + 配置 IPv4/IPv6 地址")
        # 用本端 AS 作 RD 区分，便于排错；两端可以相同也可以不同
        _setup_vrf_and_address(rt, device="r1", local_v4=R1_V4, local_v6=R1_V6, rd_v4="65001:1", rd_v6="65001:2")
        _setup_vrf_and_address(rt, device="r2", local_v4=R2_V4, local_v6=R2_V6, rd_v4="65002:1", rd_v6="65002:2")

        # 等直连路由出现在 VRF 内（IPv4 + IPv6 子网）
        for dev in ("r1", "r2"):
            wait_check(
                rt,
                device=dev,
                command=f"show route ipv4 10.99.0.0 30 vrf {VRF_NAME}",
                timeout=15,
                interval=2,
                regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                label=f"{dev} ipv4 connected in vrf {VRF_NAME}",
            )
            wait_check(
                rt,
                device=dev,
                command=f"show route ipv6 2001:db8:99:: 64 vrf {VRF_NAME}",
                timeout=15,
                interval=2,
                regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                label=f"{dev} ipv6 connected in vrf {VRF_NAME}",
            )

        step("配置 r1 BGP（vrf red 子视图，双栈邻居）")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                f"vrf {VRF_NAME}",
                "router-id 1.1.1.1",
                f"neighbor {R2_V4} as 65002",
                f"neighbor {R2_V6} as 65002",
                "af ipv4-unicast",
                f"neighbor {R2_V4} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {R2_V6} enable",
                "exit",
                "exit",
                "end",
            ],
        )

        step("配置 r2 BGP（vrf red 子视图，双栈邻居）")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                "bgp 65002",
                "router-id 2.2.2.2",
                f"vrf {VRF_NAME}",
                "router-id 2.2.2.2",
                f"neighbor {R1_V4} as 65001",
                f"neighbor {R1_V6} as 65001",
                "af ipv4-unicast",
                f"neighbor {R1_V4} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {R1_V6} enable",
                "exit",
                "exit",
                "end",
            ],
        )

        step("等待 VRF 内双栈 BGP 会话 Established")
        _wait_dual_stack_session(rt, device="r1", peer_v4=R2_V4, peer_v6=R2_V6)
        _wait_dual_stack_session(rt, device="r2", peer_v4=R1_V4, peer_v6=R1_V6)

        step("公网视图不应显示 VRF 邻居")
        for dev, peer_v4, peer_v6 in (("r1", R2_V4, R2_V6), ("r2", R1_V4, R1_V6)):
            out_v4 = cmd(rt, dev, "show bgp neighbor af ipv4-unicast")
            if peer_v4 in out_v4:
                raise RuntimeError(
                    f"{dev} public-VRF ipv4 neighbor view unexpectedly contains {peer_v4}:\n{out_v4}"
                )
            out_v6 = cmd(rt, dev, "show bgp neighbor af ipv6-unicast")
            if peer_v6 in out_v6:
                raise RuntimeError(
                    f"{dev} public-VRF ipv6 neighbor view unexpectedly contains {peer_v6}:\n{out_v6}"
                )

        print("BGP VRF dual-stack neighbor 验证通过。")
    finally:
        _cleanup(rt, base)
