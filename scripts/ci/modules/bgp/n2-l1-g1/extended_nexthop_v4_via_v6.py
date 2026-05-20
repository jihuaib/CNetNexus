#!/usr/bin/env python3
"""
RFC 8950 (Extended-Nexthop) 验证：仅 IPv6 BGP 邻居承载 IPv4 NLRI。

场景目标：
- r1/r2 之间只配置 IPv6 BGP 邻居（不配置任何 IPv4 邻居）
- 在 IPv6 邻居下使能 af ipv4-unicast，协商 Extended-Nexthop capability
- r2 注入 IPv4 静态路由并 import-route static 进 BGP，AF=ipv4-unicast
- r1 在 ipv4-unicast 收到该路由，nexthop 必须为 IPv6（only one path，无 v4 备选）
- r1 Route RIB best path 走 IPv6 nexthop
- r1 OS 路由表通过 netlink RTA_VIA AF_INET6 安装到内核：
  Gateway 列显示 IPv6 地址，证明跨族 nexthop 已正确下发
- 反向同样验证：r1 注入 IPv4 路由 → r2 接收并装到内核
- r1 reboot 后所有校验仍然成立
"""

from __future__ import annotations

import re

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_ipv4_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


# r2 注入的 IPv4 前缀
R2_IPV4_PREFIX_ADDR = "10.66.66.0"
R2_IPV4_MASK = "24"
R2_IPV4_PREFIX = f"{R2_IPV4_PREFIX_ADDR}/{R2_IPV4_MASK}"

# r1 注入的 IPv4 前缀（反向校验）
R1_IPV4_PREFIX_ADDR = "10.77.77.0"
R1_IPV4_MASK = "24"
R1_IPV4_PREFIX = f"{R1_IPV4_PREFIX_ADDR}/{R1_IPV4_MASK}"


def _peer_row(peer: str, state: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+{re.escape(state)}\s*$"


def _session_checks(r1_peer_v6: str, r2_peer_v6: str) -> list[dict[str, object]]:
    return [
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r1_peer_v6],
            "regex": [_peer_row(r1_peer_v6, "Established")],
            "label": "r1 ipv4-unicast v6-peer established",
        },
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [r1_peer_v6],
            "regex": [_peer_row(r1_peer_v6, "Established")],
            "label": "r1 ipv6-unicast v6-peer established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r2_peer_v6],
            "regex": [_peer_row(r2_peer_v6, "Established")],
            "label": "r2 ipv4-unicast v6-peer established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [r2_peer_v6],
            "regex": [_peer_row(r2_peer_v6, "Established")],
            "label": "r2 ipv6-unicast v6-peer established",
        },
    ]


def _ext_nexthop_capability_checks(r1_peer_v6: str, r2_peer_v6: str) -> list[dict[str, object]]:
    """Extended-Nexthop 必须在 v6 peer 上协商成功（Yes/Yes/Yes）。"""
    ext_nh_yes_row = r"(?im)^\s*Extended-Nexthop\s+Yes\s+Yes\s+Yes\s*$"
    return [
        {
            "device": "r1",
            "command": f"show bgp neighbor af ipv4-unicast {r1_peer_v6}",
            "contains": [r1_peer_v6, "Capabilities:", "Extended-Nexthop"],
            "regex": [ext_nh_yes_row],
            "label": "r1 ipv4-unicast v6-peer ext-nexthop negotiated",
        },
        {
            "device": "r2",
            "command": f"show bgp neighbor af ipv4-unicast {r2_peer_v6}",
            "contains": [r2_peer_v6, "Capabilities:", "Extended-Nexthop"],
            "regex": [ext_nh_yes_row],
            "label": "r2 ipv4-unicast v6-peer ext-nexthop negotiated",
        },
    ]


def _wait_ipv4_bgp_v6_nexthop_only(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    nh6: str,
    timeout: int,
) -> None:
    """
    ipv4-unicast 表中该前缀必须唯一一条 path，nexthop 是 IPv6。

    断言策略：
    - 正向：存在带前缀且 nexthop=nh6 的行（带 best/valid 标记或续行格式）
    - 负向：不存在同前缀但 nexthop 形如 IPv4 点分十进制的行
    （`Paths: N` 头部表示整个 AF 的总路径数，会被其它前缀干扰，故不用于单前缀断言）
    """
    row_v6 = (
        rf"(?im)(?:^[>v ]*{re.escape(prefix)}\s+{re.escape(nh6)}\s+|^\s*[>v ]+\s+{re.escape(nh6)}\s+)"
    )
    # 同前缀但 nexthop 是 IPv4 点分十进制 —— 出现就说明 RFC 8950 行为不符合预期
    not_row_v4 = rf"(?im)^[>v ]*{re.escape(prefix)}\s+\d+\.\d+\.\d+\.\d+\s+"
    wait_check(
        rt,
        device=device,
        command="show bgp route af ipv4-unicast",
        timeout=timeout,
        interval=2,
        contains=[prefix],
        regex=[row_v6],
        not_regex=[not_row_v4],
        label=f"{device} ipv4-unicast {prefix} only v6-nexthop path ({nh6})",
    )


def _wait_route_rib_ipv4_via_v6(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    mask: str,
    nh6: str,
    timeout: int,
) -> None:
    """Route 模块的 RIB：bgp best path 的 Nexthop 字段是 IPv6。"""
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {mask}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for"],
        regex=[
            rf"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Nexthop\s*:\s*{re.escape(nh6)}\b.*?Preference\s*:\s*200\b"
        ],
        label=f"{device} route RIB v4 prefix {destination}/{mask} nexthop is v6 {nh6}",
    )


def _wait_route_iter_ipv6_oif(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    mask: str,
    iter_nh6: str,
    iter_oif: str,
    timeout: int,
) -> None:
    """递归解析后的 Iter NH 是直连 v6 链路地址，Iter OIF 是出接口名。"""
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {mask}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for"],
        regex=[
            rf"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Iter\s+NH\s*:\s*{re.escape(iter_nh6)}\b.*?Iter\s+OIF\s*:\s*{re.escape(iter_oif)}\b"
        ],
        label=f"{device} route iter v4 {destination}/{mask} via v6 {iter_nh6}/{iter_oif}",
    )
    wait_fib_ipv4_route(
        rt,
        device=device,
        prefix_addr=destination,
        prefix_len=mask,
        expect_present=True,
        nexthop=iter_nh6,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} fib v4 {destination}/{mask} via v6 {iter_nh6}",
    )


def _wait_os_ipv4_via_v6(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    nh6: str,
    timeout: int,
) -> None:
    """
    OS 路由表（show fib ipv4 os）从 netlink RTM_GETROUTE 解析；
    Gateway 列若显示 IPv6 地址，意味着内核以 RTA_VIA AF_INET6 安装了该 IPv4 路由，
    即 RFC 8950 跨族 nexthop 数据面已生效。
    """
    best_row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(nh6)}\s+\S+\s+bgp\s+\d+(?:\s+\S+)?\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show fib ipv4 os",
        timeout=timeout,
        interval=2,
        regex=[best_row_regex],
        label=f"{device} OS table {prefix} via v6 gateway {nh6} (RTA_VIA AF_INET6)",
    )


def _verify_one_direction(
    rt: TopologyRuntime,
    *,
    sender: str,
    receiver: str,
    prefix_addr: str,
    mask: str,
    sender_nh4: str,
    receiver_iter_nh6: str,
    receiver_oif: str,
    timeout: int,
) -> None:
    """sender 注入 IPv4 静态路由 → receiver 通过 v6 邻居收到，验证 RIB+OS。"""
    full_prefix = f"{prefix_addr}/{mask}"
    step(f"Inject IPv4 static {full_prefix} on {sender}")
    run_cmds(
        rt=rt,
        device=sender,
        strict=False,
        commands=[
            "config",
            f"route static ipv4 {prefix_addr} {mask} {sender_nh4}",
            "end",
        ],
    )

    step(f"Verify {receiver} af-ipv4 sees {full_prefix} with v6 nexthop only")
    _wait_ipv4_bgp_v6_nexthop_only(
        rt, device=receiver, prefix=full_prefix, nh6=receiver_iter_nh6, timeout=timeout
    )

    step(f"Verify {receiver} Route RIB best path uses v6 nexthop")
    _wait_route_rib_ipv4_via_v6(
        rt, device=receiver, destination=prefix_addr, mask=mask, nh6=receiver_iter_nh6, timeout=timeout
    )
    _wait_route_iter_ipv6_oif(
        rt,
        device=receiver,
        destination=prefix_addr,
        mask=mask,
        iter_nh6=receiver_iter_nh6,
        iter_oif=receiver_oif,
        timeout=timeout,
    )

    step(f"Verify {receiver} OS table installs {full_prefix} via v6 gateway (RTA_VIA AF_INET6)")
    _wait_os_ipv4_via_v6(
        rt, device=receiver, prefix=full_prefix, nh6=receiver_iter_nh6, timeout=timeout
    )


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {R1_IPV4_PREFIX_ADDR} {R1_IPV4_MASK} {g_top.r1.GE_1.peer_ip}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {R2_IPV4_PREFIX_ADDR} {R2_IPV4_MASK} {g_top.r2.GE_1.peer_ip}",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)
    r2_peer_v4 = str(g_top.r2.GE_1.peer_ip)
    r1_peer_v6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_v6 = str(g_top.r2.GE_1.peer_ip6)

    session_checks = _session_checks(r1_peer_v6, r2_peer_v6)
    ext_nh_checks = _ext_nexthop_capability_checks(r1_peer_v6, r2_peer_v6)

    try:
        step("Cleanup stale config")
        _cleanup(rt)

        step("Configure BGP base (no IPv4 peer, only IPv6 peer)")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"],
        )

        step("Enable IPv6 neighbor with both AFs (ipv4-unicast + ipv6-unicast)")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_v6} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_v6} enable",
                "import-route static",
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
                "bgp 65002",
                f"neighbor {r2_peer_v6} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_v6} enable",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_peer_v6} enable",
                "exit",
                "end",
            ],
        )

        step("Wait BGP IPv6-peer Established under both AFs")
        wait_checks(rt, session_checks, timeout=60)

        step("Verify Extended-Nexthop capability negotiated on both ends")
        wait_checks(rt, ext_nh_checks, timeout=40)

        # ---------- r2 -> r1：IPv4 NLRI via v6 nexthop ----------
        _verify_one_direction(
            rt,
            sender="r2",
            receiver="r1",
            prefix_addr=R2_IPV4_PREFIX_ADDR,
            mask=R2_IPV4_MASK,
            sender_nh4=r2_peer_v4,
            receiver_iter_nh6=r1_peer_v6,
            receiver_oif="GE-1",
            timeout=40,
        )

        # ---------- r1 -> r2：反向验证 ----------
        _verify_one_direction(
            rt,
            sender="r1",
            receiver="r2",
            prefix_addr=R1_IPV4_PREFIX_ADDR,
            mask=R1_IPV4_MASK,
            sender_nh4=r1_peer_v4,
            receiver_iter_nh6=r2_peer_v6,
            receiver_oif="GE-1",
            timeout=40,
        )

        step("Reboot r1 and re-verify all RFC 8950 forwarding state")
        reboot_device(rt, "r1", timeout=120)

        step("Wait sessions after reboot")
        wait_checks(rt, session_checks, timeout=60)
        step("Re-verify Extended-Nexthop capability after reboot")
        wait_checks(rt, ext_nh_checks, timeout=40)

        step("Re-verify r2 -> r1 direction after reboot")
        _wait_ipv4_bgp_v6_nexthop_only(
            rt, device="r1", prefix=R2_IPV4_PREFIX, nh6=r1_peer_v6, timeout=40
        )
        _wait_route_rib_ipv4_via_v6(
            rt, device="r1", destination=R2_IPV4_PREFIX_ADDR, mask=R2_IPV4_MASK, nh6=r1_peer_v6, timeout=40
        )
        _wait_route_iter_ipv6_oif(
            rt,
            device="r1",
            destination=R2_IPV4_PREFIX_ADDR,
            mask=R2_IPV4_MASK,
            iter_nh6=r1_peer_v6,
            iter_oif="GE-1",
            timeout=40,
        )
        _wait_os_ipv4_via_v6(
            rt, device="r1", prefix=R2_IPV4_PREFIX, nh6=r1_peer_v6, timeout=40
        )

        step("Re-verify r1 -> r2 direction after reboot")
        _wait_ipv4_bgp_v6_nexthop_only(
            rt, device="r2", prefix=R1_IPV4_PREFIX, nh6=r2_peer_v6, timeout=40
        )
        _wait_route_rib_ipv4_via_v6(
            rt, device="r2", destination=R1_IPV4_PREFIX_ADDR, mask=R1_IPV4_MASK, nh6=r2_peer_v6, timeout=40
        )
        _wait_os_ipv4_via_v6(
            rt, device="r2", prefix=R1_IPV4_PREFIX, nh6=r2_peer_v6, timeout=40
        )

        print("BGP RFC 8950 (Extended-Nexthop) IPv4-via-IPv6 forwarding check passed.")
    finally:
        _cleanup(rt)


def main(rt: TopologyRuntime, top: dict[str, object]) -> None:
    run(rt, top)
