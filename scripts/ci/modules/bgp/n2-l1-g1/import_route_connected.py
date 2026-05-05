#!/usr/bin/env python3
"""
BGP `import-route connected` 时序验证。

设计目标：
- ETH 直连路由打有 NO_ADV 标记，import-route connected 后 BGP 本地能引入，但不会向 peer 发布。
- loop 接口的直连路由不打 NO_ADV，BGP 引入后会正常发布到 peer。
- import-route static 与 import-route connected 互斥（覆盖式），切换时正确订阅/取消订阅、清理 RIB。
- 删除并重建 loop 后，原 loop 直连路由能再次被发布。
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


LOOP_ID = 1
LOOP_V4 = "1.1.1.1"
LOOP_V4_PREFIX = 32
LOOP_V6 = "2001:db8:1::1"
LOOP_V6_PREFIX = 128


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup BGP/loop config")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            "bgp 65002",
            "af ipv4-unicast",
            "no import-route connected",
            "no import-route static",
            "exit",
            "af ipv6-unicast",
            "no import-route connected",
            "no import-route static",
            "exit",
            "exit",
            "no bgp",
            f"no if loop {LOOP_ID}",
            "end",
        ],
    )
    run_cmds(rt=rt, device="r1", strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    """
    入口：通过 module_runner 调用。
    """
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r1_local_v4 = str(g_top.r1.GE_1.ip)
    r2_local_v4 = str(g_top.r2.GE_1.ip)
    r1_local_v6 = str(g_top.r1.GE_1.ip6)
    r2_local_v6 = str(g_top.r2.GE_1.ip6)
    r1_peer_v6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_v6 = str(g_top.r2.GE_1.peer_ip6)

    eth_v4_net = _net_prefix(r2_local_v4, 30)  # GE link 网段
    eth_v6_net = _net_prefix(r2_local_v6, 64)

    try:
        step("基础 BGP + dual-stack 邻居")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                f"neighbor {r1_peer_ip} as 65002",
                f"neighbor {r1_peer_v6} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r1_peer_v6} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"ip address {LOOP_V4} {LOOP_V4_PREFIX}",
                f"ipv6 address {LOOP_V6} {LOOP_V6_PREFIX}",
                "exit",
                "bgp 65002",
                "router-id 2.2.2.2",
                f"neighbor {r2_peer_ip} as 65001",
                f"neighbor {r2_peer_v6} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "import-route connected",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_peer_v6} enable",
                "import-route connected",
                "exit",
                "end",
            ],
        )

        step("等待邻居 Established（v4/v6）")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 ipv4-unicast Established",
                },
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "regex": [rf"(?im)^\s*{re.escape(r1_peer_v6)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 ipv6-unicast Established",
                },
            ],
            timeout=30,
        )

        step("loop 直连路由：r2 本地 BGP RIB 含有，r1 收到通告（v4 + v6）")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r2 本地 import loop v4",
                },
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r2 本地 import loop v6",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r1 收到 loop v4 通告",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r1 收到 loop v6 通告",
                },
            ],
            timeout=30,
        )

        step("ETH 直连路由：r2 本地 BGP RIB 含有，但 r1 不应收到")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [eth_v4_net],
                    "label": "r2 本地 import ETH v4",
                },
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [eth_v6_net],
                    "label": "r2 本地 import ETH v6",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [eth_v4_net],
                    "label": "r1 不应收到 ETH v4（NO_ADV）",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "not_contains": [eth_v6_net],
                    "label": "r1 不应收到 ETH v6（NO_ADV）",
                },
            ],
            timeout=30,
        )

        step("覆盖式互斥：切换到 import-route static，原 connected 引入应清空")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "af ipv4-unicast",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                "import-route static",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}", eth_v4_net],
                    "label": "r2 ipv4 connected 引入已清空",
                },
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "not_contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}", eth_v6_net],
                    "label": "r2 ipv6 connected 引入已清空",
                },
                {
                    "device": "r2",
                    "command": "show route ipv4 subscribe",
                    "regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv4\s*$"],
                    "not_regex": [r"(?im)^\s*bgp\s+connected\s+0\s+ipv4\s*$"],
                    "label": "r2 ipv4 订阅切到 static",
                },
                {
                    "device": "r2",
                    "command": "show route ipv6 subscribe",
                    "regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv6\s*$"],
                    "not_regex": [r"(?im)^\s*bgp\s+connected\s+0\s+ipv6\s*$"],
                    "label": "r2 ipv6 订阅切到 static",
                },
            ],
            timeout=15,
        )

        step("再次切回 import-route connected，loop 路由应重新被通告")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "af ipv4-unicast",
                "import-route connected",
                "exit",
                "af ipv6-unicast",
                "import-route connected",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r1 重新收到 loop v4",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r1 重新收到 loop v6",
                },
                {
                    "device": "r2",
                    "command": "show route ipv4 subscribe",
                    "regex": [r"(?im)^\s*bgp\s+connected\s+0\s+ipv4\s*$"],
                    "not_regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv4\s*$"],
                    "label": "r2 ipv4 订阅切回 connected",
                },
                {
                    "device": "r2",
                    "command": "show route ipv6 subscribe",
                    "regex": [r"(?im)^\s*bgp\s+connected\s+0\s+ipv6\s*$"],
                    "not_regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv6\s*$"],
                    "label": "r2 ipv6 订阅切回 connected",
                },
            ],
            timeout=30,
        )

        step("loop 删除 IP：peer 上的 loop 路由应被撤销")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"no ip address {LOOP_V4} {LOOP_V4_PREFIX}",
                f"no ipv6 address {LOOP_V6} {LOOP_V6_PREFIX}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r2 本地 loop v4 已撤回",
                },
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "not_contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r2 本地 loop v6 已撤回",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r1 loop v4 撤销（删 IP）",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "not_contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r1 loop v6 撤销（删 IP）",
                },
            ],
            timeout=30,
        )

        step("loop 重新加 IP：peer 应再次收到通告")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"ip address {LOOP_V4} {LOOP_V4_PREFIX}",
                f"ipv6 address {LOOP_V6} {LOOP_V6_PREFIX}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r2 本地 loop v4 重新引入",
                },
                {
                    "device": "r2",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r2 本地 loop v6 重新引入",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r1 loop v4 重新通告（加 IP）",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r1 loop v6 重新通告（加 IP）",
                },
            ],
            timeout=30,
        )

        step("删除 loop 接口：peer 上的 loop 路由应被撤销")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"no if loop {LOOP_ID}", "end"],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r1 loop v4 已撤销",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "not_contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r1 loop v6 已撤销",
                },
            ],
            timeout=30,
        )

        step("重建 loop 接口（同 ID 同地址）：r1 应再次收到通告")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"ip address {LOOP_V4} {LOOP_V4_PREFIX}",
                f"ipv6 address {LOOP_V6} {LOOP_V6_PREFIX}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r1 loop v4 重建后再次收到",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r1 loop v6 重建后再次收到",
                },
            ],
            timeout=30,
        )

        step("no import-route connected：所有引入清空")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "af ipv4-unicast",
                "no import-route connected",
                "exit",
                "af ipv6-unicast",
                "no import-route connected",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [f"{LOOP_V4}/{LOOP_V4_PREFIX}"],
                    "label": "r1 ipv4 connected 撤回",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "not_contains": [f"{LOOP_V6}/{LOOP_V6_PREFIX}"],
                    "label": "r1 ipv6 connected 撤回",
                },
                {
                    "device": "r2",
                    "command": "show route ipv4 subscribe",
                    "not_regex": [r"(?im)^\s*bgp\s+connected\s+0\s+ipv4\s*$"],
                    "label": "r2 ipv4 connected 订阅取消",
                },
            ],
            timeout=15,
        )

        print("BGP import-route connected (eth/loop NO_ADV) 时序检查通过。")
    finally:
        _cleanup(rt)


def _net_prefix(addr: str, prefix_len: int) -> str:
    """根据 IP 字符串和前缀长度返回二进制对齐的网段字符串（v4/v6 通用）。"""
    import ipaddress

    network = ipaddress.ip_network(f"{addr}/{prefix_len}", strict=False)
    return f"{network.network_address}/{prefix_len}"
