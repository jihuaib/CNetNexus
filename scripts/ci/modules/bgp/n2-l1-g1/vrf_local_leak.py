#!/usr/bin/env python3
"""VRF 本地交叉（local route leaking, nexthop-vrf 模型）端到端验证（独立于 vpnv4）。

本机 r1 两个 VRF red / blue 互相 export+import 同一 RT（1:1）。red 里有一条经真实网关
(10.0.12.2, r2) 的静态路由 100.2.2.2/32，按 RT 直接泄漏进 blue 的单播表——**不使能 vpnv4、无邻居**。
关键验证只做 OS FIB 覆盖：泄漏进 blue/red 的目标前缀必须在 OS 路由表中可见，同时验证 red 下的
`null0` 静态路由配置是否落地。
"""

from __future__ import annotations

import re
import time

from module_api import (  # noqa: E402
    require_devices,
    mark_step_failed,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


RT = "1:1"
GE_IF = "GE-1"
RED = "red"
BLUE = "blue"
RED_RD = "65001:1"
BLUE_RD = "65001:2"
BLUE_EXPORT_RT = f"vpn-target {RT} export"
BLUE_EXPORT_RT_DEL = f"no vpn-target {RT} export"

LINK_NET = "10.0.12.0"
R1_GE = "10.0.12.1"
R2_GE = "10.0.12.2"
LINK_LEN = 24

R2_LOOP_IDX = 10
R2_LOOP = "100.2.2.2"          # 经 red 静态路由 + 泄漏，blue 要能到
BLUE_LOOP_IDX = 11
BLUE_LOOP = "100.1.1.1"        # blue 本地源（回程目标）
LOOP_LEN = 32

NULL0_PREFIX = "198.51.100.0"
NULL0_PREFIX_LEN = 32
NULL0_ROUTE = f"route static ipv4 vrf {RED} {NULL0_PREFIX} {NULL0_PREFIX_LEN} interface null0"
NULL0_ROUTE_DEL = f"no route static ipv4 vrf {RED} {NULL0_PREFIX} {NULL0_PREFIX_LEN} interface null0"


def _ping_ok(out: str) -> bool:
    if "bytes from" not in out:
        return False
    m = re.search(r"(\d+)%\s*packet loss", out)
    return not (m and int(m.group(1)) >= 100)


def _collect_ping_diag(rt: TopologyRuntime) -> None:
    # 诊断用：确认控制面到数据面各层是否一致，避免把“ping 参数问题”和“可达性问题”混淆。
    diag_cmds = [
        ("r1", "show if GE-1"),
        ("r2", "show if GE-1"),
        ("r1", "show if loop 11"),
        ("r2", "show if loop 10"),
        ("r1", "show route ipv4 vrf blue 100.2.2.2 32"),
        ("r1", "show route ipv4 vrf red 100.1.1.1 32"),
        ("r2", "show route ipv4 100.1.1.1 32"),
        ("r2", "show route ipv4 100.2.2.2 32"),
        ("r1", f"show bgp route af ipv4-unicast vrf {BLUE}"),
        ("r1", f"show bgp route af ipv4-unicast vrf {RED}"),
        ("r1", f"show fib os ipv4 vrf {BLUE}"),
        ("r1", f"show fib os ipv4 vrf {RED}"),
        ("r2", "show fib os ipv4"),
        ("r1", f"ping {R2_GE}", 12),
        ("r1", f"ping {R2_GE} -a {BLUE_LOOP} vrf {BLUE}", 12),
        ("r2", f"ping {R1_GE} -a {R2_LOOP}", 12),
        ("r1", f"ping {R2_LOOP} vrf {BLUE}", 12),
        ("r1", f"ping {R2_LOOP} -a {BLUE_LOOP}", 12),
        ("r1", f"ping {R2_LOOP} -a {BLUE_LOOP} vrf {BLUE}", 12),
        ("r2", f"ping {BLUE_LOOP} -a {R2_LOOP}", 12),
    ]
    print("=== 数据面诊断开始 ===", flush=True)
    for item in diag_cmds:
        if len(item) == 2:
            dev, cmd = item
            timeout = 20
        else:
            dev, cmd, timeout = item
        try:
            out = rt.exec_cmd(dev, cmd, timeout=timeout)
            print(f"[{dev}] {cmd}", flush=True)
            print(out, flush=True)
        except Exception as exc:
            print(f"[{dev}] {cmd} -> execute failed: {exc}", flush=True)
    print("=== 数据面诊断结束 ===", flush=True)


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup")
    run_cmds(rt=rt, device="r1", strict=False, commands=[
        "end",
        "config",
        "no bgp",
        f"no route static ipv4 vrf {RED} {R2_LOOP} {LOOP_LEN} {R2_GE}",
        NULL0_ROUTE_DEL,
        f"if {GE_IF}",
        "no vrf forwarding",
        "exit",
        f"no if loop {BLUE_LOOP_IDX}",
        f"no vrf {RED}",
        f"no vrf {BLUE}",
        "end",
    ])
    run_cmds(rt=rt, device="r2", strict=False, commands=[
        "config",
        f"no route static ipv4 {BLUE_LOOP} {LOOP_LEN} {R1_GE}",
        f"no if loop {R2_LOOP_IDX}",
        f"if {GE_IF}",
        f"no ip address {R2_GE} {LINK_LEN}",
        "exit",
        "end",
    ])


def _setup(rt: TopologyRuntime) -> None:
    # r1: VRFs + RT + GE-1 in red + blue loopback + static to r2 loop + null0 static + bgp import
    run_cmds(rt=rt, device="r1", commands=[
        "config",
        f"vrf {RED}", "af ipv4-unicast", f"route-distinguisher {RED_RD}",
        f"vpn-target {RT} export", f"vpn-target {RT} import", "exit", "exit",
        f"vrf {BLUE}", "af ipv4-unicast", f"route-distinguisher {BLUE_RD}",
        f"vpn-target {RT} export", f"vpn-target {RT} import", "exit", "exit",
        f"if {GE_IF}", "no shutdown", f"vrf forwarding {RED}", f"ip address {R1_GE} {LINK_LEN}", "exit",
        f"if loop {BLUE_LOOP_IDX}", f"vrf forwarding {BLUE}", f"ip address {BLUE_LOOP} {LOOP_LEN}", "exit",
        f"route static ipv4 vrf {RED} {R2_LOOP} {LOOP_LEN} {R2_GE}",
        NULL0_ROUTE,
        "end",
    ])
    # r2: GE-1 + loopback + return static
    run_cmds(rt=rt, device="r2", commands=[
        "config",
        f"if {GE_IF}", "no shutdown", f"ip address {R2_GE} {LINK_LEN}", "exit",
        f"if loop {R2_LOOP_IDX}", f"ip address {R2_LOOP} {LOOP_LEN}", "exit",
        f"route static ipv4 {BLUE_LOOP} {LOOP_LEN} {R1_GE}",
        "end",
    ])
    # 注意：同一 AF 内 import-route static/connected 互斥（覆盖式），故 red 只引入 static、blue 只引入 connected
    run_cmds(rt=rt, device="r1", commands=[
        "config",
        "bgp 65001",
        "router-id 1.1.1.1",
        f"vrf {RED}", "af ipv4-unicast", "import-route static", "exit", "exit",
        f"vrf {BLUE}", "af ipv4-unicast", "import-route connected", "exit", "exit",
        "end",
    ])
    # wait for control-plane propagation before os-check
    time.sleep(2)


def _set_blue_export_rt(rt: TopologyRuntime, *, enabled: bool) -> None:
    run_cmds(rt=rt, device="r1", commands=[
        "config",
        f"vrf {BLUE}",
        "af ipv4-unicast",
        BLUE_EXPORT_RT if enabled else BLUE_EXPORT_RT_DEL,
        "exit",
        "exit",
        "end",
    ])


def _wait_red_blue_loop_leak(rt: TopologyRuntime, *, present: bool, label: str) -> None:
    wait_checks(rt, [{
        "device": "r1",
        "command": f"show fib os ipv4 vrf {RED}",
        "contains": [f"{BLUE_LOOP}/{LOOP_LEN}"] if present else [],
        "not_contains": [] if present else [f"{BLUE_LOOP}/{LOOP_LEN}"],
        "regex": [
            rf"(?im)^\s*\S+\s+(?:unicast|local)\s+{re.escape(BLUE_LOOP)}/{LOOP_LEN}\s+",
        ] if present else [],
        "label": label,
    }], timeout=40)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    try:
        _cleanup(rt)
        step("配置 r1 双 VRF 互导 + GE-1(red) + blue loopback + 静态到 r2；r2 GE-1+loopback+回程")
        _setup(rt)

        step("配置校验：red 下存在静态 null0 路由")
        wait_checks(rt, [{
            "device": "r1",
            "command": "show current-configuration",
            "regex": [rf"(?im)^{re.escape(NULL0_ROUTE)}$"],
            "label": "r1 red 配置中存在 null0 静态路由",
        }], timeout=20)

        step("OS FIB 校验：blue 可见泄漏条目 100.2.2.2")
        wait_checks(rt, [{
            "device": "r1",
            "command": f"show fib os ipv4 vrf {BLUE}",
            "contains": [f"{R2_LOOP}/{LOOP_LEN}"],
            "regex": [
                rf"(?im)^\s*\S+\s+unicast\s+{re.escape(R2_LOOP)}/{LOOP_LEN}\s+",
            ],
            "label": "blue OS FIB has leaked route",
        }], timeout=40)

        step("OS FIB 校验：blue 可见泄漏的 red null0 静态路由")
        wait_checks(rt, [{
            "device": "r1",
            "command": f"show fib os ipv4 vrf {BLUE}",
            "contains": [f"{NULL0_PREFIX}/{NULL0_PREFIX_LEN}"],
            "regex": [
                rf"(?im)^\s*\S+\s+blackhole\s+{re.escape(NULL0_PREFIX)}/{NULL0_PREFIX_LEN}\s+",
            ],
            "label": "blue OS FIB has leaked red null0 static route",
        }], timeout=40)

        step("OS FIB 校验：red 可见回送 blue loopback 泄漏条目")
        wait_checks(rt, [{
            "device": "r1",
            "command": f"show fib os ipv4 vrf {RED}",
            "contains": [f"{BLUE_LOOP}/{LOOP_LEN}"],
            "regex": [
                rf"(?im)^\s*\S+\s+(?:unicast|local)\s+{re.escape(BLUE_LOOP)}/{LOOP_LEN}\s+",
            ],
            "label": "red OS FIB has reverse leaked blue loopback",
        }], timeout=40)

        step("删除 blue export RT：red 中 blue loopback 泄漏路由应撤销")
        _set_blue_export_rt(rt, enabled=False)
        _wait_red_blue_loop_leak(
            rt,
            present=False,
            label="red OS FIB withdrew leaked blue loopback after blue export RT delete",
        )

        step("恢复 blue export RT：red 中 blue loopback 泄漏路由重新下发")
        _set_blue_export_rt(rt, enabled=True)
        _wait_red_blue_loop_leak(
            rt,
            present=True,
            label="red OS FIB has leaked blue loopback after blue export RT restore",
        )

        step("数据面 ping：blue loopback -> r2 loopback（经 red 出接口转发）")
        ok = False
        deadline = time.time() + 30
        while time.time() < deadline:
            out = rt.exec_cmd("r1", f"ping {R2_LOOP} -a {BLUE_LOOP} vrf {BLUE}", timeout=25)
            print(out, flush=True)
            if _ping_ok(out):
                ok = True
                break
            time.sleep(3)
        if not ok:
            _collect_ping_diag(rt)
            mark_step_failed("数据面 ping：blue loopback -> r2 loopback（经 red 出接口转发）")
            raise RuntimeError("blue -> r2 (via red leak) ping failed")

        print("VRF local-cross check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
