#!/usr/bin/env python3
"""
LDP/IF/ROUTE process lifecycle check on the n2-l1-g1 topology.

Coverage on r1 with r2 as peer:
- baseline LDP OPERATIONAL session + remote LIB has peer loopback FEC
- process reboot ldp: ldp 进程更替，邻居重新 OPERATIONAL，remote LIB 重新恢复
- process stop  ldp: 进程不在，邻居/LIB 消失
- process start ldp: 进程恢复后会话/LIB 重新恢复
- process reboot if: IF replay → LDP 重新订阅 IF + 收到 SMOOTHEND 后重新建邻
- process stop  if : LDP 邻居因 hello/keepalive 超时拆除
- process start if : LDP 邻居自动恢复
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
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"

R1_LSR_ID = "1.1.1.1"
R2_LSR_ID = "2.2.2.2"

R1_LOOP_ID = 11
R2_LOOP_ID = 22
R1_LOOP_V4 = "10.255.1.1"
R2_LOOP_V4 = "10.255.2.2"
R1_LOOP_V4_LEN = 32
R2_LOOP_V4_LEN = 32

HELLO_INTERVAL_MS = 1000
HOLD_TIME_MS = 3000
KEEPALIVE_INTERVAL_MS = 3000

WAIT_PID_SEC = 20
WAIT_GONE_SEC = 20
WAIT_SESSION_SEC = 60
WAIT_LIB_SEC = 60


# ---------------------------------------------------------------------------
# 进程探针
# ---------------------------------------------------------------------------


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
    before = _wait_module_pids(container, module, predicate=lambda p: len(p) == 1,
                                timeout=10, what="single pid before reboot")
    old_pid = before[0]
    process_reboot(rt, device, module)
    _wait_module_pids(container, module, predicate=lambda p: old_pid not in p,
                      timeout=WAIT_PID_SEC, what=f"old pid {old_pid} to exit")
    _wait_module_pids(container, module, predicate=lambda p: len(p) == 1 and p[0] != old_pid,
                      timeout=WAIT_PID_SEC, what="new pid after reboot")


def _process_stop(rt: TopologyRuntime, device: str, container: str, module: str) -> None:
    _wait_module_pids(container, module, predicate=lambda p: len(p) == 1,
                      timeout=10, what="single pid before stop")
    process_stop(rt, device, module)
    _wait_module_pids(container, module, predicate=lambda p: not p, timeout=WAIT_GONE_SEC, what="no pid after stop")


def _process_start(rt: TopologyRuntime, device: str, container: str, module: str) -> None:
    process_start(rt, device, module)
    _wait_module_pids(container, module, predicate=lambda p: len(p) == 1, timeout=WAIT_PID_SEC,
                      what="single pid after start")


# ---------------------------------------------------------------------------
# 业务断言
# ---------------------------------------------------------------------------


def _wait_session_operational(rt: TopologyRuntime, device: str, *, peer_lsr: str,
                              timeout: int = WAIT_SESSION_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command="show ldp neighbor",
        timeout=timeout,
        interval=2,
        contains=[peer_lsr, GE_IF, "OPERATIONAL"],
        regex=[rf"(?im)^\s*{re.escape(peer_lsr)}\s+0\s+{re.escape(GE_IF)}\s+\S+\s+OPERATIONAL\b"],
        label=f"{device} ldp peer {peer_lsr} OPERATIONAL",
    )


def _wait_session_gone(rt: TopologyRuntime, device: str, *, peer_lsr: str,
                       timeout: int = WAIT_SESSION_SEC) -> None:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, "show ldp neighbor", strict=False, timeout=5)
        last = out
        if re.search(rf"(?im)^\s*{re.escape(peer_lsr)}\s+0\s+{re.escape(GE_IF)}\s+\S+\s+OPERATIONAL\b", out) is None:
            return
        time.sleep(2)
    raise AssertionError(f"timeout waiting {device} ldp peer {peer_lsr} to drop OPERATIONAL; last:\n{last}")


def _wait_remote_lib_has_peer_loop(rt: TopologyRuntime, device: str, *, peer_lsr: str, peer_loop_v4: str,
                                    peer_loop_v4_len: int, timeout: int = WAIT_LIB_SEC) -> None:
    wait_check(
        rt,
        device=device,
        command="show ldp binding",
        timeout=timeout,
        interval=3,
        regex=[rf"(?im)^\s*{re.escape(peer_lsr)}\s+{re.escape(peer_loop_v4)}/{peer_loop_v4_len}\s+\d+\s*$"],
        label=f"{device} remote LIB has {peer_lsr} {peer_loop_v4}/{peer_loop_v4_len}",
    )


# ---------------------------------------------------------------------------
# 拓扑准备 / 清理
# ---------------------------------------------------------------------------


def _cleanup(rt: TopologyRuntime) -> None:
    for dev, loop in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"if {GE_IF}",
                "no ldp enable",
                "exit",
                f"no if loop {loop}",
                "no ldp",
                "end",
            ],
        )


def _configure(rt: TopologyRuntime, *, device: str, lsr_id: str, loop_id: int,
               loop_v4: str, loop_v4_len: int) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if loop {loop_id}",
            f"ip address {loop_v4} {loop_v4_len}",
            "exit",
            "ldp",
            f"lsr-id {lsr_id}",
            f"hello-interval {HELLO_INTERVAL_MS}",
            f"hold-time {HOLD_TIME_MS}",
            f"keepalive-interval {KEEPALIVE_INTERVAL_MS}",
            "exit",
            f"if {GE_IF}",
            "ldp enable",
            "exit",
            "end",
        ],
    )


# ---------------------------------------------------------------------------
# 测试主流程
# ---------------------------------------------------------------------------


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    container = rt.container_name("r1")

    try:
        _cleanup(rt)

        step("Configure baseline LDP + loopback on both routers")
        _configure(rt, device="r1", lsr_id=R1_LSR_ID, loop_id=R1_LOOP_ID,
                   loop_v4=R1_LOOP_V4, loop_v4_len=R1_LOOP_V4_LEN)
        _configure(rt, device="r2", lsr_id=R2_LSR_ID, loop_id=R2_LOOP_ID,
                   loop_v4=R2_LOOP_V4, loop_v4_len=R2_LOOP_V4_LEN)

        step("Baseline: LDP OPERATIONAL on both sides + remote LIB has peer loopback")
        _wait_session_operational(rt, "r1", peer_lsr=R2_LSR_ID)
        _wait_session_operational(rt, "r2", peer_lsr=R1_LSR_ID)
        _wait_remote_lib_has_peer_loop(rt, "r1", peer_lsr=R2_LSR_ID,
                                       peer_loop_v4=R2_LOOP_V4, peer_loop_v4_len=R2_LOOP_V4_LEN)
        _wait_remote_lib_has_peer_loop(rt, "r2", peer_lsr=R1_LSR_ID,
                                       peer_loop_v4=R1_LOOP_V4, peer_loop_v4_len=R1_LOOP_V4_LEN)

        # ============== Phase B: reboot ldp on r1 ==============
        step("Phase B: process reboot ldp on r1")
        _process_reboot(rt, "r1", container, "ldp")
        step("Phase B: ldp 邻居/remote LIB 重新恢复")
        _wait_session_operational(rt, "r1", peer_lsr=R2_LSR_ID)
        _wait_session_operational(rt, "r2", peer_lsr=R1_LSR_ID)
        _wait_remote_lib_has_peer_loop(rt, "r1", peer_lsr=R2_LSR_ID,
                                       peer_loop_v4=R2_LOOP_V4, peer_loop_v4_len=R2_LOOP_V4_LEN)

        # ============== Phase C: stop ldp on r1 ==============
        step("Phase C: process stop ldp on r1")
        _process_stop(rt, "r1", container, "ldp")
        step("Phase C: r2 端的 ldp 邻居最终 drop OPERATIONAL")
        _wait_session_gone(rt, "r2", peer_lsr=R1_LSR_ID)

        # ============== Phase D: start ldp on r1 ==============
        step("Phase D: process start ldp on r1")
        _process_start(rt, "r1", container, "ldp")
        step("Phase D: ldp 邻居 / remote LIB 自动恢复")
        _wait_session_operational(rt, "r1", peer_lsr=R2_LSR_ID)
        _wait_session_operational(rt, "r2", peer_lsr=R1_LSR_ID)
        _wait_remote_lib_has_peer_loop(rt, "r1", peer_lsr=R2_LSR_ID,
                                       peer_loop_v4=R2_LOOP_V4, peer_loop_v4_len=R2_LOOP_V4_LEN)

        # ============== Phase E: reboot if on r1 ==============
        step("Phase E: process reboot if on r1（验证 LDP 等 IF SMOOTHEND 后再 db_restore）")
        _process_reboot(rt, "r1", container, "if")
        step("Phase E: ldp 邻居重新 OPERATIONAL")
        _wait_session_operational(rt, "r1", peer_lsr=R2_LSR_ID)
        _wait_session_operational(rt, "r2", peer_lsr=R1_LSR_ID)
        _wait_remote_lib_has_peer_loop(rt, "r1", peer_lsr=R2_LSR_ID,
                                       peer_loop_v4=R2_LOOP_V4, peer_loop_v4_len=R2_LOOP_V4_LEN)

        # ============== Phase F: stop if on r1 ==============
        step("Phase F: process stop if on r1")
        _process_stop(rt, "r1", container, "if")
        step("Phase F: r2 端 ldp 邻居因 hello/hold 超时拆除")
        _wait_session_gone(rt, "r2", peer_lsr=R1_LSR_ID)

        # ============== Phase G: start if on r1 ==============
        step("Phase G: process start if on r1")
        _process_start(rt, "r1", container, "if")
        step("Phase G: ldp 邻居 / remote LIB 自动恢复")
        _wait_session_operational(rt, "r1", peer_lsr=R2_LSR_ID)
        _wait_session_operational(rt, "r2", peer_lsr=R1_LSR_ID)
        _wait_remote_lib_has_peer_loop(rt, "r1", peer_lsr=R2_LSR_ID,
                                       peer_loop_v4=R2_LOOP_V4, peer_loop_v4_len=R2_LOOP_V4_LEN)

        print("LDP process lifecycle check passed.")
    finally:
        # 失败时若进程不在，先确保它在跑再清理
        for mod in ("if", "ldp"):
            if not _module_pids(container, mod):
                try:
                    process_start(rt, "r1", mod)
                    _wait_module_pids(container, mod, predicate=lambda p: len(p) == 1,
                                      timeout=WAIT_PID_SEC, what="pid after cleanup-start")
                except Exception as e:
                    print(f"cleanup warn: failed to restart {mod}: {e}", flush=True)
        _cleanup(rt)
