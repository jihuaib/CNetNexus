#!/usr/bin/env python3
"""
BGP/ROUTE process lifecycle 端到端校验。

拓扑沿用本 case 目录的 top.yaml：r1 (AS 65001) <-> r2 (AS 65002)，单 GE-1 直连。
r1 通过 import-route static 把 ROUTE_COUNT 条 IPv4 静态路由灌给 BGP，r2 作为 eBGP
邻居学习这批前缀并下刷到 ROUTE/FIB/OS FIB。

覆盖的生命周期场景（动作均作用于 r2）：
1. 基线：r1 注入 ROUTE_COUNT 条前缀 → r2 的 BGP/ROUTE/FIB/OS FIB 全部到位
2. reboot bgp：r2 BGP 重启后,会话重建,所有前缀重新到达 BGP/ROUTE/FIB/OS
3. stop bgp：r2 BGP 退出,前缀从 BGP/ROUTE/FIB/OS 全部撤销
   （依赖 bgp_worker_runtime_cleanup → bgp_route_flush_withdraw_all_for_shutdown
    把所有 FLUSHED 路由主动 route_rpc_del 给 ROUTE）
4. start bgp：r2 重新拉起 BGP,会话恢复,前缀再次到位
5. stop route：r2 ROUTE 退出,FIB/OS 全部清空,BGP RIB 仍持有前缀但 nexthop 不再
   可解析 → 这批前缀不应被选为 best（show bgp route 行首无 '>'）。
   依赖 route_worker_shutdown 退出前主动:
     - route_relay_publish_unreachable_for_shutdown(): 遍历 nh_watch_table,给
       每个外部 owner 的 watcher 发 ROUTE_MSG_TYPE_NH_NOTIFY(resolved=0)
       → BGP 侧 bgp_relay_handle_nh_notify 清 iter_resolved → bgp_calc 重算
       → BGP_ROUTE_FLAG_BEST 清零 → show bgp route 行首没有 '>' 标记。
     - route_pub_withdraw_all_for_shutdown(): 对 OS-installed best path 给订阅者
       发 ROUTE_MSG_TYPE_UPDATE(is_withdraw=1),清各方镜像。
     - route_calc_cleanup(): 遍历 RIB 调 fib_rpc_route_delete,FIB 删 OS 路由。
6. start route：r2 ROUTE 恢复,BGP 重刷 ROUTE,FIB/OS 重建
"""

from __future__ import annotations

import re
import subprocess
import time

from module_api import (  # noqa: E402
    cmd,
    g_top,
    process_reboot,
    process_start,
    process_stop,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
    wait_fib_route,
)
from top_runner import TopologyRuntime  # noqa: E402


ROUTE_COUNT = 200


def _prefix_octets(i: int) -> tuple[int, int]:
    """第 i (0-based) 条测试 /24 前缀映射到 10.<second>.<third>.0/24。

    避开 10.12.0.0/30（r1-r2 直连），从 10.90.0.0/24 起步，每行 200 条满了
    再递增 second 字节,200 条够用一个 second 字段。
    """
    base_second = 90
    second = base_second + (i // 200)
    third = i % 200
    return second, third


def _prefix_str(i: int) -> str:
    second, third = _prefix_octets(i)
    return f"10.{second}.{third}.0/24"


def _prefix_addr(i: int) -> str:
    second, third = _prefix_octets(i)
    return f"10.{second}.{third}.0"


PREFIX_LEN = 24


def _sampled_indices() -> list[int]:
    """挑头/中/尾 + 1/3 + 2/3 五个样本做细粒度断言,避免 200 条全量 wait。"""
    return sorted(
        {
            0,
            ROUTE_COUNT // 3,
            ROUTE_COUNT // 2,
            (2 * ROUTE_COUNT) // 3,
            ROUTE_COUNT - 1,
        }
    )


def _sampled_prefixes() -> list[str]:
    return [_prefix_str(i) for i in _sampled_indices()]


# ============================================================================
# 进程生命周期 helpers（复用 isis lifecycle 套路：pgrep + 等 pid 变化）
# ============================================================================


def _module_pids(container: str, module: str) -> list[int]:
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", f"netnexus-{module}"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(f"pgrep netnexus-{module} failed: {proc.stderr}")
    return [int(x) for x in proc.stdout.split() if x.strip().isdigit()]


def _wait_module_pids(container: str, module: str, *, predicate, timeout: float, what: str) -> list[int]:
    deadline = time.monotonic() + timeout
    pids: list[int] = []
    while time.monotonic() < deadline:
        pids = _module_pids(container, module)
        if predicate(pids):
            return pids
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting {module} {what}; last pids={pids}")


def _process_reboot(rt: TopologyRuntime, device: str, container: str, module: str) -> None:
    before = _wait_module_pids(
        container,
        module,
        predicate=lambda p: len(p) == 1,
        timeout=10,
        what="single pid before reboot",
    )
    old_pid = before[0]
    process_reboot(rt, device, module)
    _wait_module_pids(
        container,
        module,
        predicate=lambda p: old_pid not in p,
        timeout=15,
        what=f"old pid {old_pid} to exit",
    )
    _wait_module_pids(
        container,
        module,
        predicate=lambda p: len(p) == 1 and p[0] != old_pid,
        timeout=20,
        what="new pid after reboot",
    )


def _process_stop(rt: TopologyRuntime, device: str, container: str, module: str) -> None:
    _wait_module_pids(
        container,
        module,
        predicate=lambda p: len(p) == 1,
        timeout=10,
        what="single pid before stop",
    )
    # module_api.process_stop 默认已经等 DEV SIGCHLD handler 把 child_pid 清零，
    # 这里不需要再做 pgrep / show dev modules 的二次轮询。
    process_stop(rt, device, module)


def _process_start(rt: TopologyRuntime, device: str, container: str, module: str) -> None:
    process_start(rt, device, module)
    _wait_module_pids(container, module, predicate=lambda p: len(p) == 1, timeout=20, what="single pid after start")


# ============================================================================
# 数据面校验 helpers
# ============================================================================


def _wait_bgp_session(rt: TopologyRuntime, *, r1_peer_ip: str, r2_peer_ip: str, timeout: int = 60) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1->r2 ipv4-unicast established",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2->r1 ipv4-unicast established",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_bgp_rib_full(rt: TopologyRuntime, *, device: str, timeout: int = 90) -> None:
    """断言 device 的 BGP RIB 含全部 ROUTE_COUNT 条前缀（按 Total 行 + 抽样）。"""
    wait_check(
        rt,
        device=device,
        command="show bgp route af ipv4-unicast",
        timeout=timeout,
        interval=2,
        contains=_sampled_prefixes(),
        regex=[rf"(?im)^\s*Total\s*:\s*{ROUTE_COUNT}\s+networks\b"],
        label=f"{device} BGP RIB has all {ROUTE_COUNT} prefixes",
    )


def _wait_bgp_rib_empty(rt: TopologyRuntime, *, device: str, timeout: int = 60) -> None:
    """断言 device 的 BGP RIB 完全清空（Total: 0 networks）。"""
    wait_check(
        rt,
        device=device,
        command="show bgp route af ipv4-unicast",
        timeout=timeout,
        interval=2,
        regex=[r"(?im)^\s*Total\s*:\s*0\s+networks\b"],
        label=f"{device} BGP RIB empty",
    )


def _wait_route_prefix(
    rt: TopologyRuntime, *, device: str, prefix: str, present: bool, timeout: int
) -> None:
    addr = prefix.split("/", 1)[0]
    if present:
        wait_check(
            rt,
            device=device,
            command=f"show route ipv4 {addr} {PREFIX_LEN}",
            timeout=timeout,
            interval=2,
            contains=[f"Routing entry for {prefix}"],
            not_contains=["(no routes)", "(no matching routes)"],
            regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*bgp\b"],
            label=f"{device} ROUTE {prefix} present via BGP",
        )
    else:
        wait_check(
            rt,
            device=device,
            command=f"show route ipv4 {addr} {PREFIX_LEN}",
            timeout=timeout,
            interval=2,
            regex=[r"(?im)(?:\(no routes\)|\(no matching routes\))"],
            label=f"{device} ROUTE {prefix} absent",
        )


def _wait_fib_prefix(
    rt: TopologyRuntime, *, device: str, prefix: str, present: bool, timeout: int
) -> None:
    addr = prefix.split("/", 1)[0]
    wait_fib_route(
        rt,
        device=device,
        afi="ipv4",
        prefix_addr=addr,
        prefix_len=PREFIX_LEN,
        expect_present=present,
        installed=True if present else None,
        skip_os=False if present else None,
        timeout=timeout,
        interval=2,
        label=f"{device} FIB {prefix} {'present' if present else 'absent'}",
    )


def _wait_os_prefix(
    rt: TopologyRuntime, *, device: str, prefix: str, present: bool, timeout: int
) -> None:
    wait_check(
        rt,
        device=device,
        command="show fib ipv4 os",
        timeout=timeout,
        interval=2,
        contains=[prefix] if present else [],
        not_contains=[] if present else [prefix],
        label=f"{device} OS FIB {prefix} {'present' if present else 'absent'}",
    )


def _wait_r2_full_convergence(rt: TopologyRuntime, *, timeout: int = 120) -> None:
    """r2 上 200 条前缀全部到位:BGP RIB + ROUTE + FIB + OS（采样断言）。"""
    _wait_bgp_rib_full(rt, device="r2", timeout=timeout)
    for pfx in _sampled_prefixes():
        _wait_route_prefix(rt, device="r2", prefix=pfx, present=True, timeout=timeout)
        _wait_fib_prefix(rt, device="r2", prefix=pfx, present=True, timeout=timeout)
        _wait_os_prefix(rt, device="r2", prefix=pfx, present=True, timeout=timeout)


def _wait_r2_data_plane_gone(rt: TopologyRuntime, *, timeout: int = 60) -> None:
    """r2 的 ROUTE/FIB/OS 都看不到这批前缀;不强约束 BGP RIB（不同场景表现不同）。"""
    for pfx in _sampled_prefixes():
        _wait_route_prefix(rt, device="r2", prefix=pfx, present=False, timeout=timeout)
        _wait_fib_prefix(rt, device="r2", prefix=pfx, present=False, timeout=timeout)
        _wait_os_prefix(rt, device="r2", prefix=pfx, present=False, timeout=timeout)


def _wait_r2_fib_os_gone(rt: TopologyRuntime, *, timeout: int = 60) -> None:
    """仅断言 FIB + OS FIB 撤销（用于 stop route 场景:BGP 进程未死,RIB 仍存）。"""
    for pfx in _sampled_prefixes():
        _wait_fib_prefix(rt, device="r2", prefix=pfx, present=False, timeout=timeout)
        _wait_os_prefix(rt, device="r2", prefix=pfx, present=False, timeout=timeout)


def _assert_r2_bgp_routes_not_preferred(rt: TopologyRuntime) -> None:
    """r2 ROUTE 死后,BGP 自身仍持有前缀但 nexthop 不可达 → 无 best 标记。

    show bgp route af ipv4-unicast 每行以 '<best-flag><valid-flag>' 起头,
    best-flag 为 '>' 表示 BEST,空格表示非 best。这里抽样断言样本前缀的所有行
    都不带 '>' 前缀。
    """
    output = cmd(rt, "r2", "show bgp route af ipv4-unicast", strict=False)
    for pfx in _sampled_prefixes():
        # 该前缀对应的所有路径行:行内必含 pfx
        for line in output.splitlines():
            if pfx not in line:
                continue
            # 行首 2 个字符是 flag,正常 best 路由是 '>v ',
            # 不可达后应该退化成 ' v ' 或 '   '。
            if line.startswith(">"):
                raise AssertionError(
                    f"r2 BGP prefix {pfx} still marked BEST after route stop:\n{line}\n"
                    f"--- full show bgp route ---\n{output}"
                )


# ============================================================================
# 配置 / 清理
# ============================================================================


def _configure_bgp(rt: TopologyRuntime, *, r1_peer_ip: str, r2_peer_ip: str) -> None:
    step("Configure eBGP base on r1/r2 and import-route static on r1")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            "bgp 65001",
            "router-id 1.1.1.1",
            f"neighbor {r1_peer_ip} as 65002",
            "af ipv4-unicast",
            "import-route static",
            f"neighbor {r1_peer_ip} enable",
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
            "af ipv4-unicast",
            f"neighbor {r2_peer_ip} enable",
            "exit",
            "end",
        ],
    )


def _inject_static_routes(rt: TopologyRuntime, *, r1_nh: str) -> None:
    step(f"Inject {ROUTE_COUNT} static IPv4 /24 routes on r1 (single config block)")
    inject_cmds: list[str] = ["config"]
    for i in range(ROUTE_COUNT):
        inject_cmds.append(f"route static ipv4 {_prefix_addr(i)} {PREFIX_LEN} {r1_nh}")
    inject_cmds.append("end")
    run_cmds(rt=rt, device="r1", commands=inject_cmds)


def _cleanup_case_config(rt: TopologyRuntime, *, r1_nh: str) -> None:
    step("Cleanup BGP + static config on both sides")
    r1_cmds: list[str] = ["end", "config"]
    for i in range(ROUTE_COUNT):
        r1_cmds.append(f"no route static ipv4 {_prefix_addr(i)} {PREFIX_LEN} {r1_nh}")
    r1_cmds.extend(["no bgp", "end"])
    run_cmds(rt=rt, device="r1", strict=False, commands=r1_cmds)
    run_cmds(rt=rt, device="r2", strict=False, commands=["end", "config", "no bgp", "end"])


# ============================================================================
# Entry
# ============================================================================


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)  # r2 在 r1 GE-1 视角的对端地址
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)  # r1 在 r2 GE-1 视角的对端地址
    r2_container = rt.container_name("r2")

    try:
        _cleanup_case_config(rt, r1_nh=r1_peer_ip)
        _configure_bgp(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip)

        step("Wait baseline eBGP session established")
        _wait_bgp_session(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip)

        _inject_static_routes(rt, r1_nh=r1_peer_ip)

        step(f"Wait r2 to learn all {ROUTE_COUNT} prefixes in BGP/ROUTE/FIB/OS")
        _wait_r2_full_convergence(rt)

        step("Reboot BGP on r2 and verify all routes recover in BGP/ROUTE/FIB/OS")
        _process_reboot(rt, "r2", r2_container, "bgp")
        _wait_bgp_session(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip, timeout=120)
        _wait_r2_full_convergence(rt, timeout=180)

        step("Stop BGP on r2 and verify all routes are withdrawn from BGP/ROUTE/FIB/OS")
        _process_stop(rt, "r2", r2_container, "bgp")
        _wait_r2_data_plane_gone(rt, timeout=60)

        step("Start BGP on r2 and verify all routes recover")
        _process_start(rt, "r2", r2_container, "bgp")
        _wait_bgp_session(rt, r1_peer_ip=r1_peer_ip, r2_peer_ip=r2_peer_ip, timeout=120)
        _wait_r2_full_convergence(rt, timeout=180)

        step("Stop ROUTE on r2 — BGP keeps RIB, but routes unreachable: FIB/OS withdrawn, BGP not preferred")
        _process_stop(rt, "r2", r2_container, "route")
        # FIB/OS 必撤;BGP 自己进程没死,RIB 仍持有前缀(只是 best 标志因 nh 不可达被清)。
        _wait_r2_fib_os_gone(rt, timeout=60)
        _assert_r2_bgp_routes_not_preferred(rt)

        step("Start ROUTE on r2 and verify ROUTE/FIB/OS recover")
        _process_start(rt, "r2", r2_container, "route")
        _wait_r2_full_convergence(rt, timeout=180)

        print(f"BGP/ROUTE process lifecycle check passed ({ROUTE_COUNT} prefixes).")
    finally:
        if not should_skip_cleanup():
            _cleanup_case_config(rt, r1_nh=r1_peer_ip)
