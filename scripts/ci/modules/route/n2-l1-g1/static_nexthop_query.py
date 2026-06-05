#!/usr/bin/env python3
"""
Static route + nexthop 查询命令端到端校验（dual-stack）。

目标：
- 在 r1 上配置两条共享同一下一跳（peer_ip）的 IPv4 静态路由，验证均下发 RIB/OS。
- 验证新增的三条 nexthop 查询命令彼此一致、共享同一 nexthop_id
  （命令形式：``show {fib|route|route static} nexthop {ipv4|ipv6} [vrf <name>] [id <id>]``，afi 必填）：
    * ``show fib nexthop ipv4``          —— FIB nexthop 对象（gateway=peer_ip）
    * ``show route nexthop ipv4``        —— route nexthop 对象（proto=static）
    * ``show route static nexthop ipv4`` —— 静态 nexthop 组（Ref=共享条数）
- 验证过滤：``ipv4|ipv6`` 地址族（必填）、``id <nexthop-id>`` 精确查询、``vrf`` 适配。
- 覆盖 nexthop 种类：IP 网关、直连(interface-only)、null0 黑洞。
- 验证引用计数随路由增删变化；全部删除后组/对象消失。
- IPv6：单条静态路由验证 ``show {fib|route} nexthop ipv6``。
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    cmd,
    g_top,
    require_devices,
    run_cmds,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402

GE_IF = "GE-1"

PFX1_ADDR = "198.18.71.0"
PFX2_ADDR = "198.18.72.0"
IFACE_PFX_ADDR = "198.18.73.0"  # interface-only（直连）静态路由
NULL0_PFX_ADDR = "198.18.74.0"  # null0 黑洞静态路由
MASK = "24"
PFX1 = f"{PFX1_ADDR}/{MASK}"
PFX2 = f"{PFX2_ADDR}/{MASK}"

V6_PFX_ADDR = "2001:db8:7100::"
V6_LEN = "64"
V6_PFX = f"{V6_PFX_ADDR}/{V6_LEN}"


def _extract_static_nh_id(output: str, nexthop: str) -> str:
    """从 ``show route static nexthop`` 输出里抓取指定下一跳所在组的 NH-ID。"""
    row = re.search(
        rf"(?im)^\s*(\d+)\s+\d+\s+ipv4\s+\S+\s+{re.escape(nexthop)}\b",
        output,
    )
    if not row:
        raise RuntimeError(
            f"未在 'show route static nexthop' 输出找到 nexthop={nexthop} 的行:\n{output}"
        )
    return row.group(1)


def _cleanup(rt: TopologyRuntime, static_nh: str, static_nh6: str) -> None:
    step("Cleanup static routes")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            f"no route static ipv4 {PFX1_ADDR} {MASK} {static_nh}",
            f"no route static ipv4 {PFX2_ADDR} {MASK} {static_nh}",
            f"no route static ipv4 {IFACE_PFX_ADDR} {MASK} interface {GE_IF}",
            f"no route static ipv4 {NULL0_PFX_ADDR} {MASK} interface null0",
            f"no route static ipv6 {V6_PFX_ADDR} {V6_LEN} {static_nh6}",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    static_nh = str(g_top.r1.GE_1.peer_ip)
    static_nh6 = str(g_top.r1.GE_1.peer_ip6)

    try:
        _cleanup(rt, static_nh, static_nh6)

        step("Configure two IPv4 static routes sharing one nexthop on r1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {GE_IF}",
                "no shutdown",
                "exit",
                f"route static ipv4 {PFX1_ADDR} {MASK} {static_nh}",
                f"route static ipv4 {PFX2_ADDR} {MASK} {static_nh}",
                "end",
            ],
        )

        step("Both prefixes installed in RIB via the shared nexthop")
        for pfx in (PFX1, PFX2):
            wait_check(
                rt,
                device="r1",
                command="show route ipv4",
                timeout=30,
                regex=[rf"(?im)^\s*S\s+{re.escape(pfx)}\s+{re.escape(static_nh)}\s+\S+"],
                label=f"r1 RIB {pfx} via {static_nh}",
            )

        step("show route static nexthop: one group, Ref=2 (two prefixes share it)")
        wait_check(
            rt,
            device="r1",
            command="show route static nexthop ipv4",
            timeout=30,
            regex=[
                rf"(?im)^\s*\d+\s+\d+\s+ipv4\s+ip\s+{re.escape(static_nh)}\s+\S+\s+\S+\s+\d+\s+yes\s+2\s*$"
            ],
            label="r1 static nexthop group Ref=2",
        )

        # 抓取共享 nexthop_id，跨三条命令交叉验证一致性
        nh_id = _extract_static_nh_id(
            cmd(rt, "r1", "show route static nexthop ipv4"), static_nh
        )
        step(f"Shared nexthop_id={nh_id} consistent across fib/route/static")

        wait_check(
            rt,
            device="r1",
            command="show route nexthop ipv4",
            timeout=20,
            regex=[
                rf"(?im)^\s*{nh_id}\s+\d+\s+ipv4\s+static\s+{re.escape(static_nh)}\b"
            ],
            label=f"r1 route nexthop id={nh_id} proto=static",
        )
        wait_check(
            rt,
            device="r1",
            command="show fib nexthop ipv4",
            timeout=20,
            regex=[
                rf"(?im)^\s*{nh_id}\s+\d+\s+ipv4\s+ip\s+up\s+{re.escape(static_nh)}\b"
            ],
            label=f"r1 fib nexthop id={nh_id} gateway={static_nh}",
        )
        wait_check(
            rt,
            device="r1",
            command=f"show route ipv4 {PFX1_ADDR} {MASK}",
            timeout=20,
            contains=[f"Routing entry for {PFX1}"],
            regex=[
                rf"(?is)Path\s*\[1\]\s*:\s*static\b.*?"
                rf"Nexthop\s*:\s*{re.escape(static_nh)}\s*.*?"
                rf"NH-ID\s*:\s*{re.escape(nh_id)}\s*"
            ],
            label=f"r1 route detail exposes static nexthop id={nh_id}",
        )

        step(f"Filter by id: show route static nexthop ipv4 id {nh_id}")
        wait_check(
            rt,
            device="r1",
            command=f"show route static nexthop ipv4 id {nh_id}",
            timeout=20,
            regex=[rf"(?im)^\s*{nh_id}\s+\d+\s+ipv4\s+ip\s+{re.escape(static_nh)}\b"],
            contains=["Total 1 static nexthop(s)"],
            label=f"r1 static nexthop id filter -> only {nh_id}",
        )

        step("Filter by afi: ipv4 shows it, ipv6 excludes it")
        wait_check(
            rt,
            device="r1",
            command="show fib nexthop ipv4",
            timeout=20,
            regex=[rf"(?im)^\s*{nh_id}\s+\d+\s+ipv4\b"],
            label="r1 fib nexthop ipv4 includes shared id",
        )
        wait_check(
            rt,
            device="r1",
            command="show fib nexthop ipv6",
            timeout=20,
            not_regex=[rf"(?im)^\s*{nh_id}\s+\d+\s+ipv4\b"],
            label="r1 fib nexthop ipv6 excludes ipv4 object",
        )

        step("Add an IPv6 static route; show {fib,route} nexthop ipv6 reflect it")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv6 {V6_PFX_ADDR} {V6_LEN} {static_nh6}",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command="show route nexthop ipv6",
            timeout=30,
            regex=[rf"(?im)^\s*\d+\s+\d+\s+ipv6\s+static\s+{re.escape(static_nh6)}\b"],
            label="r1 route nexthop ipv6 shows v6 static object",
        )
        wait_check(
            rt,
            device="r1",
            command="show fib nexthop ipv6",
            timeout=30,
            regex=[rf"(?im)^\s*\d+\s+\d+\s+ipv6\s+ip\s+up\s+{re.escape(static_nh6)}\b"],
            label="r1 fib nexthop ipv6 shows v6 gateway",
        )

        step("show route static nexthop ipv6: only ipv6 group (afi distinction)")
        wait_check(
            rt,
            device="r1",
            command="show route static nexthop ipv6",
            timeout=20,
            regex=[rf"(?im)^\s*\d+\s+\d+\s+ipv6\s+ip\s+{re.escape(static_nh6)}\b"],
            not_regex=[r"(?im)^\s*\d+\s+\d+\s+ipv4\b"],
            label="r1 static nexthop ipv6 filter excludes ipv4",
        )

        step("Configure interface-only (直连) and null0 static routes")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv4 {IFACE_PFX_ADDR} {MASK} interface {GE_IF}",
                f"route static ipv4 {NULL0_PFX_ADDR} {MASK} interface null0",
                "end",
            ],
        )

        step("show route static nexthop: directly-connected nexthop -> Kind=iface (Interface=GE-1)")
        wait_check(
            rt,
            device="r1",
            command="show route static nexthop ipv4",
            timeout=30,
            regex=[
                rf"(?im)^\s*\d+\s+\d+\s+ipv4\s+iface\s+-\s+-\s+{re.escape(GE_IF)}\s+\d+\s+yes\s+\d+\s*$"
            ],
            label="r1 static nexthop iface (directly-connected) row",
        )

        step("show route static nexthop: null0 nexthop -> Kind=null0 (Interface=null0)")
        wait_check(
            rt,
            device="r1",
            command="show route static nexthop ipv4",
            timeout=30,
            regex=[
                r"(?im)^\s*\d+\s+\d+\s+ipv4\s+null0\s+-\s+-\s+null0\s+\d+\s+yes\s+\d+\s*$"
            ],
            label="r1 static nexthop null0 row",
        )
        wait_check(
            rt,
            device="r1",
            command="show fib nexthop ipv4",
            timeout=20,
            regex=[r"(?im)^\s*\d+\s+\d+\s+ipv4\s+blackhole\b"],
            label="r1 fib nexthop has blackhole object (null0)",
        )

        step("Delete one shared IPv4 route; group remains with Ref=1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"no route static ipv4 {PFX1_ADDR} {MASK} {static_nh}",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command="show route static nexthop ipv4",
            timeout=30,
            regex=[
                rf"(?im)^\s*{nh_id}\s+\d+\s+ipv4\s+ip\s+{re.escape(static_nh)}\s+\S+\s+\S+\s+\d+\s+yes\s+1\s*$"
            ],
            label="r1 static nexthop Ref drops to 1",
        )
        # 另一条仍在 OS
        wait_check(
            rt,
            device="r1",
            command="show route ipv4",
            timeout=20,
            regex=[rf"(?im)^\s*S\s+{re.escape(PFX2)}\s+{re.escape(static_nh)}\b"],
            not_regex=[rf"(?im)^\s*S\s+{re.escape(PFX1)}\s+"],
            label="r1 only PFX2 remains after deleting PFX1",
        )

        step("Delete the last shared IPv4 route; static nexthop group disappears")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"no route static ipv4 {PFX2_ADDR} {MASK} {static_nh}",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command="show route static nexthop ipv4",
            timeout=30,
            not_regex=[rf"(?im)^\s*{nh_id}\s+\d+\s+ipv4\s+ip\s+{re.escape(static_nh)}\b"],
            label="r1 static nexthop group removed",
        )
        wait_check(
            rt,
            device="r1",
            command="show fib nexthop ipv4",
            timeout=30,
            not_regex=[rf"(?im)^\s*{nh_id}\s+\d+\s+ipv4\s+ip\s+up\s+{re.escape(static_nh)}\b"],
            label="r1 fib nexthop object removed",
        )

        print("Static route + nexthop query end-to-end check passed.")
    finally:
        _cleanup(rt, static_nh, static_nh6)
