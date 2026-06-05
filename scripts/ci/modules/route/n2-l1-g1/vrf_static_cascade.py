#!/usr/bin/env python3
"""
VRF 静态路由级联删除验证（含 RIB / FIB / FIB-OS 数据面校验，IPv4 + IPv6）。

覆盖：
- 绑定 GE-1 到 VRF "red"，在 VRF 内配置 IPv4/IPv6 直连地址。
- `route static ipv4 vrf red` ... / `route static ipv6 vrf red` ... 配置后：
    * VRF RIB（show route）中能看到 S 路径，nexthop 解析正确；
    * VRF FIB（show fib）中有详情条目，Installed=yes，Skip OS=no；
    * VRF OS FIB（show fib ipv{4,6} os vrf red）有内核条目。
- `show current-configuration` 包含带 ``vrf red`` 后缀的 ``route static`` 行。
- ``no vrf red`` 删除 VRF 后：
    * VRF 视图 RIB/FIB/FIB-OS 全部消失（命令直接报 VRF not found）；
    * ``show current-configuration`` 中对应 ``route static ... vrf red`` 行已清除；
    * 公网内未受影响的静态路由仍然存在并安装到 OS。
"""

from __future__ import annotations

import ipaddress
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

# 在 VRF 内重新分配的接口地址（GE-1 绑入 VRF 时原有 IP 会被清空）
VRF_V4 = "10.99.0.1"
VRF_V4_LEN = 30
VRF_V4_NH = "10.99.0.2"   # 与 VRF_V4 同子网，作为静态路由的 nexthop

VRF_V6 = "2001:db8:99::1"
VRF_V6_LEN = 64
VRF_V6_NH = "2001:db8:99::2"

# VRF 内的静态路由目标前缀
VRF_PREFIX_ADDR = "203.0.113.0"
VRF_PREFIX_LEN = 24
VRF_PREFIX = f"{VRF_PREFIX_ADDR}/{VRF_PREFIX_LEN}"

VRF_PREFIX6_ADDR = "2001:db8:200::"
VRF_PREFIX6_LEN = 64
VRF_PREFIX6 = f"{VRF_PREFIX6_ADDR}/{VRF_PREFIX6_LEN}"

# 公网测试静态路由（验证删除 VRF 不会影响公网）
PUB_PREFIX_ADDR = "198.51.100.0"
PUB_PREFIX_LEN = 24
PUB_PREFIX = f"{PUB_PREFIX_ADDR}/{PUB_PREFIX_LEN}"


def _route_detail_cmd(afi: str, prefix_addr: str, prefix_len: int, vrf: str) -> str:
    suffix = "" if vrf == "public" else f" vrf {vrf}"
    return f"show route {afi} {prefix_addr} {prefix_len}{suffix}"


def _fib_detail_cmd(afi: str, prefix_addr: str, prefix_len: int, vrf: str) -> str:
    suffix = "" if vrf == "public" else f" vrf {vrf}"
    return f"show fib {afi} {prefix_addr} {prefix_len}{suffix}"


def _fib_os_cmd(afi: str, vrf: str) -> str:
    suffix = "" if vrf == "public" else f" vrf {vrf}"
    return f"show fib {afi} os{suffix}"


def _network(prefix_addr: str, prefix_len: int) -> str:
    net = ipaddress.ip_network(f"{prefix_addr}/{prefix_len}", strict=False)
    return f"{net.network_address}/{prefix_len}"


def _wait_static_installed(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix_addr: str,
    prefix_len: int,
    nexthop: str,
    vrf: str,
    timeout: int = 30,
) -> None:
    """断言：route 表 / FIB / FIB-OS 三层均已装载该静态路由。"""
    prefix = _network(prefix_addr, prefix_len)
    route_cmd = _route_detail_cmd(afi, prefix_addr, prefix_len, vrf)
    fib_cmd = _fib_detail_cmd(afi, prefix_addr, prefix_len, vrf)
    fib_os_cmd = _fib_os_cmd(afi, vrf)

    route_header = rf"(?im)^\s*Routing entry for {re.escape(prefix)} \(VRF: {re.escape(vrf)}\)\s*$"
    route_static_path = r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"
    route_nh = rf"(?im)^\s*Nexthop\s*:\s*{re.escape(nexthop)}\s*$"

    fib_header = rf"(?im)^\s*Routing entry for\s+{re.escape(prefix)}\b"
    fib_afi = rf"(?im)^\s*AFI\s*:\s*{re.escape(afi)}\s*$"
    fib_installed = r"(?im)^\s*Installed\s*:\s*yes\s*$"
    fib_skip_os = r"(?im)^\s*Skip OS\s*:\s*no\s*$"

    # OS FIB dump 中的一行（kernel 来源），允许 nexthop 为 nh 或不显示具体细节
    os_row = rf"(?im)^\s*\S+\s+unicast\s+{re.escape(prefix)}\s+"

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": route_cmd,
                "regex": [route_header, route_static_path, route_nh],
                "not_contains": ["(no routes)", "(no matching routes)"],
                "label": f"r1 route {afi} {prefix} vrf={vrf} installed",
            },
            {
                "device": "r1",
                "command": fib_cmd,
                "regex": [fib_header, fib_afi, fib_installed, fib_skip_os],
                "not_contains": ["(no routes)"],
                "label": f"r1 fib {afi} {prefix} vrf={vrf} installed",
            },
            {
                "device": "r1",
                "command": fib_os_cmd,
                "regex": [os_row],
                "label": f"r1 fib-os {afi} {prefix} vrf={vrf} installed",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_static_absent_in_public(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix_addr: str,
    prefix_len: int,
    timeout: int = 30,
) -> None:
    """VRF 删除后，公网视图的 route / fib / fib-os 中也不应出现 VRF 内的前缀。"""
    prefix = _network(prefix_addr, prefix_len)
    route_cmd = _route_detail_cmd(afi, prefix_addr, prefix_len, "public")
    fib_cmd = _fib_detail_cmd(afi, prefix_addr, prefix_len, "public")
    fib_os_cmd = _fib_os_cmd(afi, "public")

    route_header = rf"(?im)^\s*Routing entry for {re.escape(prefix)} \(VRF: public\)\s*$"
    route_static_path = r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"
    fib_header = rf"(?im)^\s*Routing entry for\s+{re.escape(prefix)}\b"
    os_row = rf"(?im)^\s*\S+\s+unicast\s+{re.escape(prefix)}\s+"

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": route_cmd,
                "regex": [route_header],
                "not_regex": [route_static_path],
                "label": f"r1 route {afi} {prefix} public has no static path",
            },
            {
                "device": "r1",
                "command": fib_cmd,
                "not_regex": [fib_header],
                "label": f"r1 fib {afi} {prefix} public absent",
            },
            {
                "device": "r1",
                "command": fib_os_cmd,
                "not_regex": [os_row],
                "label": f"r1 fib-os {afi} {prefix} public absent",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_vrf_absent_for_show(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix_addr: str,
    prefix_len: int,
    vrf: str,
    timeout: int = 30,
) -> None:
    """VRF 删除后，按 ``vrf <name>`` 查询应直接报 ``VRF <name> not found``。"""
    err_re = rf"(?im)VRF\s+{re.escape(vrf)}\s+not\s+found"
    route_cmd = _route_detail_cmd(afi, prefix_addr, prefix_len, vrf)
    fib_cmd = _fib_detail_cmd(afi, prefix_addr, prefix_len, vrf)
    fib_os_cmd = _fib_os_cmd(afi, vrf)
    wait_checks(
        rt,
        [
            {"device": "r1", "command": route_cmd, "regex": [err_re], "strict": False,
             "label": f"r1 route {afi} vrf={vrf} reports not found"},
            {"device": "r1", "command": fib_cmd, "regex": [err_re], "strict": False,
             "label": f"r1 fib {afi} vrf={vrf} reports not found"},
            {"device": "r1", "command": fib_os_cmd, "regex": [err_re], "strict": False,
             "label": f"r1 fib-os {afi} vrf={vrf} reports not found"},
        ],
        timeout=timeout,
        interval=2,
    )


def _cleanup(
    rt: TopologyRuntime,
    *,
    base_v4: str,
    base_v4_len: int,
    base_v6: str,
    base_v6_len: int,
) -> None:
    """Best-effort 复位：移除测试静态路由 + VRF 绑定 + 还原公网接口地址。"""
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no route static ipv4 {PUB_PREFIX_ADDR} {PUB_PREFIX_LEN}",
            f"no route static ipv4 vrf {VRF_NAME} {VRF_PREFIX_ADDR} {VRF_PREFIX_LEN}",
            f"no route static ipv6 vrf {VRF_NAME} {VRF_PREFIX6_ADDR} {VRF_PREFIX6_LEN}",
            f"if {GE_IF}",
            "no shutdown",
            f"no ip address {VRF_V4} {VRF_V4_LEN}",
            f"no ipv6 address {VRF_V6} {VRF_V6_LEN}",
            "no vrf forwarding",
            f"ip address {base_v4} {base_v4_len}",
            f"ipv6 address {base_v6} {base_v6_len}",
            "exit",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    base_v4 = str(g_top.r1.GE_1.ip)
    base_v4_len = int(g_top.r1.GE_1.prefix)
    base_v6 = str(g_top.r1.GE_1.ip6)
    base_v6_len = int(g_top.r1.GE_1.prefix6)
    pub_nh = str(g_top.r1.GE_1.peer_ip)

    # VRF 内 GE-1 子网（用于等待连接路由出现）
    vrf_v4_net = str(ipaddress.ip_network(f"{VRF_V4}/{VRF_V4_LEN}", strict=False).network_address)
    vrf_v6_net = str(ipaddress.ip_network(f"{VRF_V6}/{VRF_V6_LEN}", strict=False).network_address)

    try:
        _cleanup(rt, base_v4=base_v4, base_v4_len=base_v4_len, base_v6=base_v6, base_v6_len=base_v6_len)

        step("创建 VRF 'red' 并把 GE-1 绑入该 VRF + 配置 IPv4/IPv6 地址")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"vrf {VRF_NAME}",
                "exit",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show vrf name {VRF_NAME}",
            timeout=10,
            interval=1,
            contains=["VRF Detail:", f"Name           : {VRF_NAME}"],
            label=f"r1 vrf {VRF_NAME} ready",
        )
        # 让 VRF_ADD 事件传播到 route 模块的 VRF cache
        time.sleep(2)

        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {GE_IF}",
                "no shutdown",
                f"vrf forwarding {VRF_NAME}",
                f"ip address {VRF_V4} {VRF_V4_LEN}",
                f"ipv6 address {VRF_V6} {VRF_V6_LEN}",
                "exit",
                "end",
            ],
        )
        # 等连接路由出现：subnet 必须在 VRF RIB 中
        wait_check(
            rt,
            device="r1",
            command=f"show route ipv4 vrf {VRF_NAME} {vrf_v4_net} {VRF_V4_LEN}",
            timeout=15,
            interval=2,
            regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
            label=f"r1 ipv4 connected subnet in vrf {VRF_NAME}",
        )
        wait_check(
            rt,
            device="r1",
            command=f"show route ipv6 vrf {VRF_NAME} {vrf_v6_net} {VRF_V6_LEN}",
            timeout=15,
            interval=2,
            regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
            label=f"r1 ipv6 connected subnet in vrf {VRF_NAME}",
        )

        step("在 VRF 中配置 IPv4 + IPv6 静态路由（nexthop 直连可达）")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv4 vrf {VRF_NAME} {VRF_PREFIX_ADDR} {VRF_PREFIX_LEN} {VRF_V4_NH}",
                f"route static ipv6 vrf {VRF_NAME} {VRF_PREFIX6_ADDR} {VRF_PREFIX6_LEN} {VRF_V6_NH}",
                "end",
            ],
        )

        step("公网配置一条静态路由（验证 VRF 删除不会牵连公网）")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv4 {PUB_PREFIX_ADDR} {PUB_PREFIX_LEN} {pub_nh}",
                "end",
            ],
        )

        step("校验 VRF IPv4 静态路由 RIB/FIB/FIB-OS 都已下发")
        _wait_static_installed(
            rt,
            afi="ipv4",
            prefix_addr=VRF_PREFIX_ADDR,
            prefix_len=VRF_PREFIX_LEN,
            nexthop=VRF_V4_NH,
            vrf=VRF_NAME,
        )

        step("校验 VRF IPv6 静态路由 RIB/FIB/FIB-OS 都已下发")
        _wait_static_installed(
            rt,
            afi="ipv6",
            prefix_addr=VRF_PREFIX6_ADDR,
            prefix_len=VRF_PREFIX6_LEN,
            nexthop=VRF_V6_NH,
            vrf=VRF_NAME,
        )

        step("校验 show current-configuration 中带 vrf 后缀的 route static 行")
        cfg_out = cmd(rt, "r1", "show current-configuration")
        vrf_v4_pattern = (
            rf"route static ipv4 {re.escape(VRF_PREFIX_ADDR)} {re.escape(str(VRF_PREFIX_LEN))} "
            rf"{re.escape(VRF_V4_NH)} vrf {re.escape(VRF_NAME)}"
        )
        vrf_v6_pattern = (
            rf"route static ipv6 {re.escape(VRF_PREFIX6_ADDR)} {re.escape(str(VRF_PREFIX6_LEN))} "
            rf"{re.escape(VRF_V6_NH)} vrf {re.escape(VRF_NAME)}"
        )
        pub_pattern = (
            rf"route static ipv4 {re.escape(PUB_PREFIX_ADDR)} {re.escape(str(PUB_PREFIX_LEN))} "
            rf"{re.escape(pub_nh)}\b(?! vrf)"
        )
        if not re.search(vrf_v4_pattern, cfg_out):
            raise RuntimeError(f"vrf {VRF_NAME} ipv4 static missing from running config:\n{cfg_out}")
        if not re.search(vrf_v6_pattern, cfg_out):
            raise RuntimeError(f"vrf {VRF_NAME} ipv6 static missing from running config:\n{cfg_out}")
        if not re.search(pub_pattern, cfg_out):
            raise RuntimeError(f"public ipv4 static missing or wrongly carries vrf suffix:\n{cfg_out}")

        step("删除 VRF (no vrf red) → 触发静态路由级联清理")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"no vrf {VRF_NAME}",
                "end",
            ],
        )

        step("校验 VRF 视图下查询 RIB/FIB/FIB-OS 都返回 'VRF red not found'")
        _wait_vrf_absent_for_show(
            rt,
            afi="ipv4",
            prefix_addr=VRF_PREFIX_ADDR,
            prefix_len=VRF_PREFIX_LEN,
            vrf=VRF_NAME,
        )
        _wait_vrf_absent_for_show(
            rt,
            afi="ipv6",
            prefix_addr=VRF_PREFIX6_ADDR,
            prefix_len=VRF_PREFIX6_LEN,
            vrf=VRF_NAME,
        )

        step("校验公网视图也找不到 VRF 内的前缀（OS 路由表已撤）")
        _wait_static_absent_in_public(
            rt,
            afi="ipv4",
            prefix_addr=VRF_PREFIX_ADDR,
            prefix_len=VRF_PREFIX_LEN,
        )
        _wait_static_absent_in_public(
            rt,
            afi="ipv6",
            prefix_addr=VRF_PREFIX6_ADDR,
            prefix_len=VRF_PREFIX6_LEN,
        )

        step("校验 show current-configuration 不再包含 vrf 静态路由，且公网静态路由仍在")
        cfg_after = cmd(rt, "r1", "show current-configuration")
        if re.search(vrf_v4_pattern, cfg_after):
            raise RuntimeError(f"vrf {VRF_NAME} ipv4 static still present after vrf delete:\n{cfg_after}")
        if re.search(vrf_v6_pattern, cfg_after):
            raise RuntimeError(f"vrf {VRF_NAME} ipv6 static still present after vrf delete:\n{cfg_after}")
        if not re.search(pub_pattern, cfg_after):
            raise RuntimeError(f"public static unexpectedly missing after vrf delete:\n{cfg_after}")

        print("VRF 静态路由级联删除（含 RIB/FIB/FIB-OS 校验）通过。")
    finally:
        _cleanup(rt, base_v4=base_v4, base_v4_len=base_v4_len, base_v6=base_v6, base_v6_len=base_v6_len)
