#!/usr/bin/env python3
"""
验证：接口 IP / 直连路由的 `process {reboot|stop|start} if` 生命周期。

覆盖：
  Phase A. 配置 r1 GE-1 IP（拓扑自动配 10.12.0.1/30），验证直连路由存在
  Phase B. 加配 loop1 + IP 192.168.99.1/32，验证 loopback 直连路由
  Phase C. process reboot if → 旧 IF pid 消失 → 新 IF pid 出现
  Phase D. IF 完成 db_restore + 重新下发，直连路由都恢复
  Phase E. process stop if → IF 进程消失 → OS+ROUTE 直连路由都消失（IF 优雅退出清理 runtime）
  Phase F. process start if → 新 IF 进程 → db_restore → 两条直连路由再次恢复
"""

from __future__ import annotations

import re
import subprocess
import time

from module_api import cmd, mark_step_failed, process_reboot, process_start, process_stop, require_devices, step  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE1_PREFIX = "10.12.0.0/30"        # r1.GE-1 直连段
GE1_IP = "10.12.0.1"
LOOP_IP = "192.168.99.1"
LOOP_PREFIX = "192.168.99.1/32"    # loopback /32
WAIT_RESPAWN_SEC = 10
WAIT_ROUTE_SEC = 8


def _list_if_pids(container: str) -> list[int]:
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", "netnexus-if"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(f"pgrep failed: {proc.stderr}")
    return [int(x) for x in proc.stdout.split() if x.strip().isdigit()]


def _wait_pid(container: str, *, predicate, timeout: float, what: str) -> list[int]:
    deadline = time.monotonic() + timeout
    pids: list[int] = []
    while time.monotonic() < deadline:
        pids = _list_if_pids(container)
        if predicate(pids):
            return pids
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting {what}; last if pids={pids}")


def _route_has(rt: TopologyRuntime, device: str, prefix: str, *, timeout: float) -> str:
    """
    轮询 `show route ipv4`，直到输出包含 prefix。返回最后一次完整输出。
    """
    deadline = time.monotonic() + timeout
    out = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, "show route ipv4", timeout=10)
        if prefix in out:
            return out
        time.sleep(0.5)
    raise AssertionError(f"timeout waiting prefix {prefix} in `show route ipv4`; last:\n{out}")


def _route_gone(rt: TopologyRuntime, device: str, prefix: str, *, timeout: float) -> str:
    """轮询直到 show route ipv4 输出不包含 prefix；超时则报错。"""
    deadline = time.monotonic() + timeout
    out = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, "show route ipv4", timeout=10)
        if prefix not in out:
            return out
        time.sleep(0.5)
    raise AssertionError(f"timeout waiting prefix {prefix} to disappear from `show route ipv4`; last:\n{out}")


def _fib_os_has(rt: TopologyRuntime, device: str, prefix: str) -> bool:
    """通过 `show fib os ipv4` 查 FIB 实际编入 OS 的 IPv4 路由是否包含 prefix。"""
    out = cmd(rt, device, "show fib os ipv4", timeout=10)
    return prefix in out


def _wait_fib_os_gone(rt: TopologyRuntime, device: str, prefix: str, *, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, "show fib os ipv4", timeout=10)
        if prefix not in out:
            return
        last = out
        time.sleep(0.5)
    raise AssertionError(f"timeout waiting FIB OS route {prefix} to disappear; last:\n{last}")


def _wait_fib_os(rt: TopologyRuntime, device: str, prefix: str, *, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, "show fib os ipv4", timeout=10)
        if prefix in out:
            return
        last = out
        time.sleep(0.5)
    raise AssertionError(f"timeout waiting FIB OS route {prefix} to appear; last:\n{last}")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))
    container = rt.container_name("r1")

    try:
        step("Phase A: 拓扑自动配的 GE-1 IP 已生效，直连路由存在")
        # 拓扑层 cli_configure_interfaces 已为 GE-1 配 IP；这里只验证路由
        _route_has(rt, "r1", GE1_PREFIX, timeout=WAIT_ROUTE_SEC)

        step("Phase B: 配置 loop1 + IP，验证 loopback 直连路由")
        cmd(rt, "r1", "config", timeout=5)
        cmd(rt, "r1", "if loop 1", timeout=5)
        cmd(rt, "r1", f"ip address {LOOP_IP} 32", timeout=5)
        cmd(rt, "r1", "exit", strict=False)
        cmd(rt, "r1", "end", strict=False)
        _route_has(rt, "r1", LOOP_PREFIX, timeout=WAIT_ROUTE_SEC)

        step("Phase C: process reboot if → 进程更替")
        before = _list_if_pids(container)
        if len(before) != 1:
            mark_step_failed()
            raise AssertionError(f"Phase C: expected exactly 1 if pid, got {before}")
        old_pid = before[0]

        out = process_reboot(rt, "r1", "if")
        if "reboot" not in out.lower():
            mark_step_failed()
            raise AssertionError(f"Phase C: unexpected reboot response:\n{out}")

        _wait_pid(container, predicate=lambda p: old_pid not in p, timeout=WAIT_RESPAWN_SEC,
                  what=f"old if pid {old_pid} to exit")
        new_pids = _wait_pid(container, predicate=lambda p: len(p) == 1 and p[0] != old_pid,
                              timeout=WAIT_RESPAWN_SEC, what="new if pid to appear")

        step("Phase D: 直连路由在 if 重启后自动恢复")
        _route_has(rt, "r1", GE1_PREFIX, timeout=15)
        _route_has(rt, "r1", LOOP_PREFIX, timeout=15)

        step("Phase E: process stop if → OS+ROUTE 直连路由都消失")
        running = _list_if_pids(container)
        if len(running) != 1:
            mark_step_failed()
            raise AssertionError(f"Phase E: expected 1 if pid, got {running}")
        pid_before_stop = running[0]

        out = process_stop(rt, "r1", "if")
        if "stop" not in out.lower():
            mark_step_failed()
            raise AssertionError(f"Phase E: unexpected stop response:\n{out}")

        _wait_pid(container, predicate=lambda p: not p, timeout=WAIT_RESPAWN_SEC,
                  what=f"if pid {pid_before_stop} to exit on stop")

        # IF 优雅退出会逐个 if_cfg_apply_ip(is_no=1)：移除 OS netlink IP（kernel 撤直连路由）
        # + 通知 ROUTE 撤路由。两条路由都验证 ROUTE RIB + FIB OS 编程。
        _route_gone(rt, "r1", GE1_PREFIX, timeout=10)
        _route_gone(rt, "r1", LOOP_PREFIX, timeout=10)
        _wait_fib_os_gone(rt, "r1", GE1_PREFIX, timeout=10)
        _wait_fib_os_gone(rt, "r1", LOOP_PREFIX, timeout=10)

        step("Phase F: process start if → 路由恢复")
        out = process_start(rt, "r1", "if")
        if "start" not in out.lower() and "ok" not in out.lower():
            mark_step_failed()
            raise AssertionError(f"Phase F: unexpected start response:\n{out}")
        started = _wait_pid(container, predicate=lambda p: len(p) == 1,
                             timeout=WAIT_RESPAWN_SEC, what="if pid after start")
        # db_restore 后路由再次恢复：ROUTE RIB + FIB OS 编程都回
        _route_has(rt, "r1", GE1_PREFIX, timeout=15)
        _route_has(rt, "r1", LOOP_PREFIX, timeout=15)
        _wait_fib_os(rt, "r1", GE1_PREFIX, timeout=10)
        _wait_fib_os(rt, "r1", LOOP_PREFIX, timeout=10)

        print(f"IF lifecycle check passed: pids {before[0]} → {new_pids[0]} (reboot) → 0 (stop) "
              f"→ {started[0]} (start); routes {GE1_PREFIX} and {LOOP_PREFIX} go through full cycle.")

    finally:
        # 清理：拆 loop1
        try:
            cmd(rt, "r1", "config", strict=False)
            cmd(rt, "r1", "if loop 1", strict=False)
            cmd(rt, "r1", f"no ip address {LOOP_IP} 32", strict=False)
            cmd(rt, "r1", "exit", strict=False)
            cmd(rt, "r1", "end", strict=False)
        except Exception as e:
            print(f"cleanup warn: {e}", flush=True)
