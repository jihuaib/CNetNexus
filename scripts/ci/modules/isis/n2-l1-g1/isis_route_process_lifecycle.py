#!/usr/bin/env python3
"""
ISIS/ROUTE/IF process lifecycle check.

Coverage on r1 with r2 as peer:
- baseline ISIS loopback route convergence
- process reboot route: IF/ISIS replay restores ROUTE RIB, FIB, OS route, and traffic
- process stop route: FIB removes all checked routes and OS route for the ISIS prefix is removed
- process start route: IF/ISIS replay restores ROUTE RIB, FIB, OS route, and traffic
- process reboot isis: ISIS replay restores adjacency, ROUTE/FIB/OS and traffic
- process stop isis: ISIS routes withdraw from ROUTE/FIB/OS and traffic fails
- process start isis: adjacency and routes recover
- process reboot if: IF replay restores interface state, ISIS re-subscribes, adjacency
  and ROUTE/FIB/OS/traffic recover
- process stop if: ISIS loses hello tx/rx via IF, adjacency drops after hold-time,
  ISIS routes withdraw and traffic fails
- process start if: IF reappears, ISIS re-subscribes via on_if_ready_cb, adjacency
  and routes recover
"""

from __future__ import annotations

import ipaddress
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


TAG = 120
GE_IF = "GE-1"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"

R1_LOOP_ID = 121
R2_LOOP_ID = 122

R1_LOOP_V4 = "10.255.121.1"
R1_LOOP_V4_LEN = 32
R2_LOOP_V4 = "10.255.122.2"
R2_LOOP_V4_LEN = 32

R1_LOOP_V6 = "2001:db8:255:121::1"
R1_LOOP_V6_LEN = 128
R2_LOOP_V6 = "2001:db8:255:122::2"
R2_LOOP_V6_LEN = 128

R1_LOOP_V4_PREFIX = f"{R1_LOOP_V4}/{R1_LOOP_V4_LEN}"
R2_LOOP_V4_PREFIX = f"{R2_LOOP_V4}/{R2_LOOP_V4_LEN}"
R1_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R1_LOOP_V6}/{R1_LOOP_V6_LEN}", strict=False))
R2_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R2_LOOP_V6}/{R2_LOOP_V6_LEN}", strict=False))

PING_SUCCESS_RE = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
PING_FAIL_RE = (
    r"(?im)(?:\b100(?:\.0)?%\s+packet loss\b|network is unreachable|"
    r"destination host unreachable|no route to host|connect:|"
    r"module timed out or failed to respond|failed to start ping|unreachable)"
)


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
    process_stop(rt, device, module)
    _wait_module_pids(container, module, predicate=lambda p: not p, timeout=20, what="no pid after stop")


def _process_start(rt: TopologyRuntime, device: str, container: str, module: str) -> None:
    process_start(rt, device, module)
    _wait_module_pids(container, module, predicate=lambda p: len(p) == 1, timeout=20, what="single pid after start")


def _cleanup_case_config(rt: TopologyRuntime) -> None:
    step("Cleanup stale ISIS/loopback config")
    for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=[
                "end",
                "config",
                f"no isis {TAG}",
                f"no if loop {loop_id}",
                f"if {GE_IF}",
                "no shutdown",
                "exit",
                "end",
            ],
        )


def _configure_isis(rt: TopologyRuntime) -> None:
    step("Configure loopbacks and ISIS")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if loop {R1_LOOP_ID}",
            f"ip address {R1_LOOP_V4} {R1_LOOP_V4_LEN}",
            f"ipv6 address {R1_LOOP_V6} {R1_LOOP_V6_LEN}",
            "exit",
            f"isis {TAG}",
            f"net {R1_NET}",
            "is-type level-1-2",
            "cost-style wide",
            "af ipv4",
            "af ipv6",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"if loop {R2_LOOP_ID}",
            f"ip address {R2_LOOP_V4} {R2_LOOP_V4_LEN}",
            f"ipv6 address {R2_LOOP_V6} {R2_LOOP_V6_LEN}",
            "exit",
            f"isis {TAG}",
            f"net {R2_NET}",
            "is-type level-1-2",
            "cost-style wide",
            "af ipv4",
            "af ipv6",
            "end",
        ],
    )

    for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=device,
            commands=[
                "config",
                f"if {GE_IF}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                f"if loop {loop_id}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis passive {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )


def _wait_adjacency(rt: TopologyRuntime, timeout: int = 90) -> None:
    r1_peer_ip4 = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip4 = str(g_top.r2.GE_1.peer_ip)
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis neighbor {TAG}",
                "contains": ["ISIS Neighbors", GE_IF, r1_peer_ip4],
                "regex": [
                    rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\s+yes\s+yes\s+\d+\s+\d+\s+"
                    rf"{re.escape(r1_peer_ip4)}\s+fe80:[0-9a-f:]+\s*$"
                ],
                "label": "r1 ISIS neighbor up",
            },
            {
                "device": "r2",
                "command": f"show isis neighbor {TAG}",
                "contains": ["ISIS Neighbors", GE_IF, r2_peer_ip4],
                "regex": [
                    rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\s+yes\s+yes\s+\d+\s+\d+\s+"
                    rf"{re.escape(r2_peer_ip4)}\s+fe80:[0-9a-f:]+\s*$"
                ],
                "label": "r2 ISIS neighbor up",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_route_present(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    prefix: str,
    timeout: int = 90,
) -> None:
    prefix_len = prefix.split("/", 1)[1]
    wait_check(
        rt,
        device=device,
        command=f"show route {afi} {destination} {prefix_len}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {prefix}"],
        not_contains=["(no routes)", "(no matching routes)"],
        regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*isis\b"],
        label=f"{device} ROUTE {afi} {prefix} present via ISIS",
    )


def _wait_route_absent(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    prefix: str,
    timeout: int = 45,
) -> None:
    prefix_len = prefix.split("/", 1)[1]
    wait_check(
        rt,
        device=device,
        command=f"show route {afi} {destination} {prefix_len}",
        timeout=timeout,
        interval=2,
        regex=[r"(?im)(?:\(no routes\)|\(no matching routes\))"],
        label=f"{device} ROUTE {afi} {prefix} absent",
    )


def _wait_fib_present(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    prefix_len: int,
    timeout: int = 90,
) -> None:
    wait_fib_route(
        rt,
        device=device,
        afi=afi,
        prefix_addr=destination,
        prefix_len=prefix_len,
        expect_present=True,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} FIB {afi} {destination}/{prefix_len} present",
    )


def _wait_fib_absent(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    prefix_len: int,
    timeout: int = 45,
) -> None:
    wait_fib_route(
        rt,
        device=device,
        afi=afi,
        prefix_addr=destination,
        prefix_len=prefix_len,
        expect_present=False,
        timeout=timeout,
        interval=2,
        label=f"{device} FIB {afi} {destination}/{prefix_len} absent",
    )


def _wait_os_prefix(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    prefix: str,
    present: bool,
    timeout: int = 60,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show fib {afi} os",
        timeout=timeout,
        interval=2,
        contains=[prefix] if present else [],
        not_contains=[] if present else [prefix],
        label=f"{device} OS FIB {afi} {prefix} {'present' if present else 'absent'}",
    )


def _wait_r1_remote_routes_ready(rt: TopologyRuntime, timeout: int = 90) -> None:
    _wait_route_present(rt, device="r1", afi="ipv4", destination=R2_LOOP_V4, prefix=R2_LOOP_V4_PREFIX, timeout=timeout)
    _wait_route_present(rt, device="r1", afi="ipv6", destination=R2_LOOP_V6, prefix=R2_LOOP_V6_PREFIX, timeout=timeout)
    _wait_fib_present(rt, device="r1", afi="ipv4", destination=R2_LOOP_V4, prefix_len=R2_LOOP_V4_LEN, timeout=timeout)
    _wait_fib_present(rt, device="r1", afi="ipv6", destination=R2_LOOP_V6, prefix_len=R2_LOOP_V6_LEN, timeout=timeout)
    _wait_os_prefix(rt, device="r1", afi="ipv4", prefix=R2_LOOP_V4_PREFIX, present=True, timeout=timeout)
    _wait_os_prefix(rt, device="r1", afi="ipv6", prefix=R2_LOOP_V6_PREFIX, present=True, timeout=timeout)


def _wait_bidirectional_remote_routes_ready(rt: TopologyRuntime, timeout: int = 90) -> None:
    _wait_r1_remote_routes_ready(rt, timeout=timeout)
    _wait_route_present(rt, device="r2", afi="ipv4", destination=R1_LOOP_V4, prefix=R1_LOOP_V4_PREFIX, timeout=timeout)
    _wait_route_present(rt, device="r2", afi="ipv6", destination=R1_LOOP_V6, prefix=R1_LOOP_V6_PREFIX, timeout=timeout)
    _wait_fib_present(rt, device="r2", afi="ipv4", destination=R1_LOOP_V4, prefix_len=R1_LOOP_V4_LEN, timeout=timeout)
    _wait_fib_present(rt, device="r2", afi="ipv6", destination=R1_LOOP_V6, prefix_len=R1_LOOP_V6_LEN, timeout=timeout)
    _wait_os_prefix(rt, device="r2", afi="ipv4", prefix=R1_LOOP_V4_PREFIX, present=True, timeout=timeout)
    _wait_os_prefix(rt, device="r2", afi="ipv6", prefix=R1_LOOP_V6_PREFIX, present=True, timeout=timeout)


def _wait_r1_remote_routes_gone(rt: TopologyRuntime, timeout: int = 45) -> None:
    _wait_route_absent(rt, device="r1", afi="ipv4", destination=R2_LOOP_V4, prefix=R2_LOOP_V4_PREFIX, timeout=timeout)
    _wait_route_absent(rt, device="r1", afi="ipv6", destination=R2_LOOP_V6, prefix=R2_LOOP_V6_PREFIX, timeout=timeout)
    _wait_fib_absent(rt, device="r1", afi="ipv4", destination=R2_LOOP_V4, prefix_len=R2_LOOP_V4_LEN, timeout=timeout)
    _wait_fib_absent(rt, device="r1", afi="ipv6", destination=R2_LOOP_V6, prefix_len=R2_LOOP_V6_LEN, timeout=timeout)
    _wait_os_prefix(rt, device="r1", afi="ipv4", prefix=R2_LOOP_V4_PREFIX, present=False, timeout=timeout)
    _wait_os_prefix(rt, device="r1", afi="ipv6", prefix=R2_LOOP_V6_PREFIX, present=False, timeout=timeout)


def _wait_r1_fib_all_checked_gone_after_route_stop(rt: TopologyRuntime) -> None:
    r1_ge_net_v4 = str(ipaddress.ip_network(str(g_top.r1.GE_1.cidr), strict=False).network_address)
    r1_ge_len_v4 = int(g_top.r1.GE_1.prefix)
    r1_ge_net_v6 = str(ipaddress.ip_network(str(g_top.r1.GE_1.cidr6), strict=False).network_address)
    r1_ge_len_v6 = int(g_top.r1.GE_1.prefix6)

    for afi, destination, plen in (
        ("ipv4", r1_ge_net_v4, r1_ge_len_v4),
        ("ipv6", r1_ge_net_v6, r1_ge_len_v6),
        ("ipv4", R1_LOOP_V4, R1_LOOP_V4_LEN),
        ("ipv6", R1_LOOP_V6, R1_LOOP_V6_LEN),
        ("ipv4", R2_LOOP_V4, R2_LOOP_V4_LEN),
        ("ipv6", R2_LOOP_V6, R2_LOOP_V6_LEN),
    ):
        _wait_fib_absent(rt, device="r1", afi=afi, destination=destination, prefix_len=plen, timeout=45)

    _wait_os_prefix(rt, device="r1", afi="ipv4", prefix=R2_LOOP_V4_PREFIX, present=False, timeout=45)
    _wait_os_prefix(rt, device="r1", afi="ipv6", prefix=R2_LOOP_V6_PREFIX, present=False, timeout=45)


def _wait_ping_success(rt: TopologyRuntime) -> None:
    wait_check(
        rt,
        device="r1",
        command=f"ping {R2_LOOP_V4} -a {R1_LOOP_V4}",
        timeout=60,
        interval=2,
        regex=[PING_SUCCESS_RE],
        not_regex=[PING_FAIL_RE],
        normalize_whitespace=False,
        label="r1 loopback IPv4 ping succeeds",
    )
    wait_check(
        rt,
        device="r1",
        command=f"ping ipv6 {R2_LOOP_V6} -a {R1_LOOP_V6}",
        timeout=60,
        interval=2,
        regex=[PING_SUCCESS_RE],
        not_regex=[PING_FAIL_RE],
        normalize_whitespace=False,
        label="r1 loopback IPv6 ping succeeds",
    )


def _wait_ping_fail(rt: TopologyRuntime) -> None:
    wait_check(
        rt,
        device="r1",
        command=f"ping {R2_LOOP_V4} -a {R1_LOOP_V4}",
        timeout=30,
        interval=2,
        regex=[PING_FAIL_RE],
        not_regex=[PING_SUCCESS_RE],
        normalize_whitespace=False,
        label="r1 loopback IPv4 ping fails",
    )
    wait_check(
        rt,
        device="r1",
        command=f"ping ipv6 {R2_LOOP_V6} -a {R1_LOOP_V6}",
        timeout=30,
        interval=2,
        regex=[PING_FAIL_RE],
        not_regex=[PING_SUCCESS_RE],
        normalize_whitespace=False,
        label="r1 loopback IPv6 ping fails",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_container = rt.container_name("r1")

    try:
        _cleanup_case_config(rt)
        _configure_isis(rt)

        step("Wait baseline ISIS adjacency, ROUTE/FIB/OS FIB, and traffic")
        _wait_adjacency(rt)
        _wait_bidirectional_remote_routes_ready(rt)
        _wait_ping_success(rt)

        step("Reboot ROUTE on r1 and verify IF/ISIS replay restores data plane")
        _process_reboot(rt, "r1", r1_container, "route")
        _wait_r1_remote_routes_ready(rt, timeout=120)
        _wait_ping_success(rt)

        step("Stop ROUTE on r1 and verify FIB routes are deleted")
        _process_stop(rt, "r1", r1_container, "route")
        _wait_r1_fib_all_checked_gone_after_route_stop(rt)
        _wait_ping_fail(rt)

        step("Start ROUTE on r1 and verify ROUTE/FIB/OS FIB/traffic recover")
        _process_start(rt, "r1", r1_container, "route")
        _wait_r1_remote_routes_ready(rt, timeout=120)
        _wait_ping_success(rt)

        step("Reboot ISIS on r1 and verify ROUTE/FIB/OS FIB/traffic recover")
        _process_reboot(rt, "r1", r1_container, "isis")
        _wait_adjacency(rt, timeout=120)
        _wait_bidirectional_remote_routes_ready(rt, timeout=120)
        _wait_ping_success(rt)

        step("Stop ISIS on r1 and verify ISIS routes are withdrawn")
        _process_stop(rt, "r1", r1_container, "isis")
        _wait_r1_remote_routes_gone(rt)
        _wait_ping_fail(rt)

        step("Start ISIS on r1 and verify adjacency and data plane recover")
        _process_start(rt, "r1", r1_container, "isis")
        _wait_adjacency(rt, timeout=120)
        _wait_bidirectional_remote_routes_ready(rt, timeout=120)
        _wait_ping_success(rt)

        # IF 模块生命周期：IF 是 ISIS 的发包/收包/接口事件来源。
        # 这里依赖 ISIS 对 DEV_MODULE_EVENT_DOWN 的反应：
        #   isis_on_if_ready_cb -> ISIS_MSG_TYPE_INTERNAL_IF_DOWN
        #   -> isis_worker_post_if_down -> ISIS_WORKER_CMD_IF_DOWN handler
        #     1) if_api_cache_cleanup + init
        #     2) isis_neighbor_reconcile_all  (邻接立即判 should_remove → 撤 LSP/SPF)
        #     3) isis_route_sync_reconcile_all_instances  (路由批量 withdraw)
        # 因此 stop if 后路由撤销时延应远低于 hello hold-time（9s）；恢复路径
        # 由现有 isis_on_if_ready_cb -> if_api_subscribe_all 自动重建。
        step("Reboot IF on r1 and verify ISIS/ROUTE/FIB/OS/traffic recover")
        _process_reboot(rt, "r1", r1_container, "if")
        _wait_adjacency(rt, timeout=120)
        _wait_bidirectional_remote_routes_ready(rt, timeout=120)
        _wait_ping_success(rt)

        step("Stop IF on r1 and verify ISIS adjacency drops, routes withdrawn, ping fails")
        t0 = time.monotonic()
        _process_stop(rt, "r1", r1_container, "if")
        _wait_r1_remote_routes_gone(rt, timeout=30)
        stop_if_elapsed = time.monotonic() - t0
        print(f"[INFO] stop if -> r1 ISIS routes withdrawn in {stop_if_elapsed:.2f}s")
        # 阈值 8s：低于 ISIS hold-time（hello-interval=3 × hold-multiplier=3 = 9s），
        # 用以回归式守护 ISIS 对 IF DOWN 的反应路径——若退化为靠 hello 超时被动感知，
        # 这条会失败。本地 dev 测试稳定在 <3s 完成。
        assert stop_if_elapsed < 8.0, (
            f"ISIS reactive IF-DOWN handling regressed: route withdraw took {stop_if_elapsed:.2f}s "
            f"(>= 8s suggests falling back to hello hold-time)"
        )
        _wait_ping_fail(rt)

        step("Start IF on r1 and verify ISIS adjacency and data plane recover")
        _process_start(rt, "r1", r1_container, "if")
        _wait_adjacency(rt, timeout=120)
        _wait_bidirectional_remote_routes_ready(rt, timeout=120)
        _wait_ping_success(rt)

        print("ISIS/ROUTE/IF process lifecycle check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup_case_config(rt)
