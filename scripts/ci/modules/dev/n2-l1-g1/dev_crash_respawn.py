#!/usr/bin/env python3
"""
端到端验证常驻模块意外退出 (SIGKILL 模拟 crash) 后由 DEV 自动 respawn 的能力。

DEV 在 SIGCHLD handler 里对意外退出的模块走 crash 自愈分支（常驻 + on-demand 一视同仁）：
  - last_crash_time 距今超 DEV_MODULE_CRASH_WINDOW_SEC (60s)，crash_count 归零
  - crash_count++，<= DEV_MODULE_CRASH_MAX_RETRIES (5) 时自动 respawn + connect
  - 超过上限放弃，phase=REGISTERED，等待人工 process start <mod> 重置 crash_count
  - on-demand 模块同样自愈：能收到 SIGCHLD 说明它已被 fork 且在服务中，崩溃后必须拉起
    让它 db_restore 续上服务，否则其它模块残留它再也撤不掉的状态

覆盖的 Phase：
  Phase A. 基线：fib（常驻）已 READY，取当前 pid
  Phase B. SIGKILL fib → DEV 自动 respawn，pid 变更
  Phase C. 连续 SIGKILL 4 次，每次都自动 respawn（crash_count 累计到 5，未超上限）
  Phase D. 第 6 次 SIGKILL → crash_count=6 > 5 → DEV 放弃 → show dev modules 中 fib 行 pid=-
  Phase E. process start fib → crash_count 归零，模块重新可用；再 kill 一次仍能自动 respawn
  Phase F. SIGKILL DEV 进程本身 → supervise.sh 在容器里把整个 netnexus 拉回来，
            所有模块（含 fib）重新 READY，CLI 重连后命令可用
  Phase G. on-demand 模块（sbmp）崩溃自愈：按需 fork 起来后 SIGKILL，DEV 应自动 respawn
            并 db_restore 续上配置（这是「on-demand 崩溃不自愈」修复点的回归保护）

仅用 r1。
"""

from __future__ import annotations

import subprocess
import time

from module_api import cmd, g_top, mark_step_failed, process_start, require_devices, run_cmds, step
from top_runner import TopologyRuntime


TARGET_MOD = "fib"
# 跟 src/dev/dev_module.h 保持同步
CRASH_WINDOW_SEC = 60
CRASH_MAX_RETRIES = 5

WAIT_RESPAWN_SEC = 8
WAIT_AFTER_GIVEUP_SEC = 3


def _restore_r1_topology_config(rt: TopologyRuntime) -> None:
    """Phase F kills DEV, so the volatile topology bootstrap config must be put back."""
    r1_ip = str(g_top.r1.GE_1.ip)
    r1_prefix = int(g_top.r1.GE_1.prefix)
    run_cmds(
        rt,
        "r1",
        strict=False,
        commands=[
            "end",
            "config",
            "sysname r1",
            "if GE-1",
            f"ip address {r1_ip} {r1_prefix}",
            "no shutdown",
            "end",
        ],
    )


def _list_module_pids(container: str, mod_name: str) -> list[int]:
    """容器内查 netnexus-<mod_name> 的 pid。pgrep 找不到 → 返回空列表（rc=1 视为正常）。"""
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", f"netnexus-{mod_name}"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(
            f"pgrep {mod_name} in {container} failed (rc={proc.returncode}): {proc.stderr}"
        )
    return [int(x) for x in proc.stdout.split() if x.strip().isdigit()]


def _list_dev_pids(container: str) -> list[int]:
    """容器内查 DEV 主进程 pid。
    DEV argv[0] 是 supervise.sh 里传的完整路径 (/opt/netnexus/bin/netnexus)，
    所以匹配 comm（进程名，由 /proc/N/comm 提供，basename 截断到 15 字节）。
    模块进程的 comm 是 'netnexus-bgp' / 'netnexus-vrf' 这种，跟 'netnexus' 不会撞。"""
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "netnexus"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(
            f"pgrep -x netnexus in {container} failed (rc={proc.returncode}): {proc.stderr}"
        )
    return [int(x) for x in proc.stdout.split() if x.strip().isdigit()]


def _wait_for_pids(container: str, *, predicate, timeout: float, what: str,
                   mod: str = TARGET_MOD) -> list[int]:
    """轮询 pids，predicate(pids) == True 即返回；超时抛 AssertionError。"""
    deadline = time.monotonic() + timeout
    pids: list[int] = []
    while time.monotonic() < deadline:
        pids = _list_module_pids(container, mod)
        if predicate(pids):
            return pids
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting {what}; last {mod} pids={pids}")


def _kill_module(container: str, pid: int, sig: str = "9") -> None:
    """模拟 crash：直接 SIGKILL，DEV 父进程的 SIGCHLD handler 接管。
    用 SIGKILL（而不是 SIGTERM）确保模块没机会发 PRE_EXIT —— 这是真的 crash 路径。"""
    proc = subprocess.run(
        ["docker", "exec", container, "kill", f"-{sig}", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"kill -{sig} {pid} failed: rc={proc.returncode} stderr={proc.stderr}")


def _show_fib_row(rt: TopologyRuntime, device: str) -> str:
    """从 show dev modules 里抽出 fib 那一行（不存在返回空串）。"""
    out = cmd(rt, device, "show dev modules", strict=False)
    for line in out.splitlines():
        if f" {TARGET_MOD} " in f" {line.strip()} " or line.strip().split()[1:2] == [TARGET_MOD]:
            return line.strip()
    return ""


def _kill_and_wait_respawn(container: str, attempt: int) -> tuple[int, int]:
    """kill 当前 fib 进程，等待新 pid 出现。返回 (old_pid, new_pid)。"""
    pids = _list_module_pids(container, TARGET_MOD)
    if not pids:
        raise AssertionError(f"kill #{attempt}: 预期 {TARGET_MOD} alive, 实得空")
    old_pid = pids[0]
    _kill_module(container, old_pid)
    new_pids = _wait_for_pids(
        container,
        predicate=lambda p, op=old_pid: len(p) == 1 and p[0] != op,
        timeout=WAIT_RESPAWN_SEC,
        what=f"{TARGET_MOD} auto-respawn after kill #{attempt}",
    )
    return old_pid, new_pids[0]


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))
    container = rt.container_name("r1")

    step(f"Phase A: 基线 — {TARGET_MOD} 应已 READY")
    rt.wait_modules_ready("r1", timeout=30)
    pids = _list_module_pids(container, TARGET_MOD)
    if len(pids) != 1:
        mark_step_failed()
        raise AssertionError(
            f"Phase A: 预期 {TARGET_MOD} 1 个 pid，实得 {pids}"
        )
    pid0 = pids[0]
    print(f"  baseline pid={pid0}", flush=True)

    step(f"Phase B: SIGKILL {TARGET_MOD} → DEV 应自动 respawn")
    old_pid, new_pid = _kill_and_wait_respawn(container, attempt=1)
    if old_pid != pid0:
        mark_step_failed()
        raise AssertionError(f"Phase B: 期望 kill pid0={pid0}, 实得 {old_pid}")
    print(f"  kill #1: {old_pid} → {new_pid}", flush=True)
    rt.wait_modules_ready("r1", timeout=15)

    step(f"Phase C: 连续再 SIGKILL 4 次，每次都应自动 respawn (crash_count 累至 5)")
    last_pid = new_pid
    for attempt in range(2, 6):
        old_pid, new_pid = _kill_and_wait_respawn(container, attempt=attempt)
        if old_pid != last_pid:
            mark_step_failed()
            raise AssertionError(
                f"Phase C kill #{attempt}: 期望 kill {last_pid}, 实得 {old_pid}"
            )
        print(f"  kill #{attempt}: {old_pid} → {new_pid}", flush=True)
        last_pid = new_pid
        # 短 wait 即可；不必等满 30s READY，目的是让 DEV 主线程把 SIGCHLD 处理掉就行
        # 但要确保新 fib 进程稳定运行，至少 IPC 建联（否则下次 kill 可能撞到尚未握手）
        rt.wait_modules_ready("r1", timeout=15)

    step(f"Phase D: 第 {CRASH_MAX_RETRIES + 1} 次 SIGKILL → 超 crash 上限，DEV 应放弃")
    pids = _list_module_pids(container, TARGET_MOD)
    if not pids or pids[0] != last_pid:
        mark_step_failed()
        raise AssertionError(
            f"Phase D 前置: 预期 {TARGET_MOD} pid={last_pid}, 实得 {pids}"
        )
    _kill_module(container, last_pid)
    # 给 DEV 主线程留时间处理 SIGCHLD + log；放弃路径不 respawn
    time.sleep(WAIT_AFTER_GIVEUP_SEC)
    pids_after = _list_module_pids(container, TARGET_MOD)
    if pids_after:
        mark_step_failed()
        raise AssertionError(
            f"Phase D: crash_count={CRASH_MAX_RETRIES + 1} > {CRASH_MAX_RETRIES}，"
            f"DEV 不该再拉起 {TARGET_MOD}，但拿到 pids={pids_after}"
        )
    # show dev modules 里 fib 应为 REGISTERED / pid=-
    fib_row = _show_fib_row(rt, "r1")
    if not fib_row:
        mark_step_failed()
        raise AssertionError(f"Phase D: show dev modules 缺 {TARGET_MOD} 行")
    if "REGISTERED" not in fib_row.upper():
        mark_step_failed()
        raise AssertionError(
            f"Phase D: 预期 phase=REGISTERED，实得行: {fib_row!r}"
        )
    print(f"  DEV 已放弃，行: {fib_row!r}", flush=True)

    step(f"Phase E: process start {TARGET_MOD} 重置窗口，验证再 kill 仍可 respawn")
    out = process_start(rt, "r1", TARGET_MOD, ready_timeout=30)
    print(f"  process start 响应: {out.strip()}", flush=True)
    pids = _list_module_pids(container, TARGET_MOD)
    if len(pids) != 1:
        mark_step_failed()
        raise AssertionError(
            f"Phase E: process start 后预期 {TARGET_MOD} 1 个 pid，实得 {pids}"
        )
    restart_pid = pids[0]

    # 再 kill 一次：crash_count 已被 process start 重置为 0，所以这次相当于 #1，仍 respawn
    _kill_module(container, restart_pid)
    new_pids = _wait_for_pids(
        container,
        predicate=lambda p: len(p) == 1 and p[0] != restart_pid,
        timeout=WAIT_RESPAWN_SEC,
        what=f"{TARGET_MOD} auto-respawn after window reset",
    )
    print(
        f"  process start → pid={restart_pid}；reset 后 kill 又自动 respawn → pid={new_pids[0]}",
        flush=True,
    )
    rt.wait_modules_ready("r1", timeout=30)

    step("Phase F: SIGKILL DEV 主进程 → supervise.sh 应把整个 netnexus 拉回来")
    dev_pids_before = _list_dev_pids(container)
    if len(dev_pids_before) != 1:
        mark_step_failed()
        raise AssertionError(f"Phase F 前置: 预期 1 个 DEV pid，实得 {dev_pids_before}")
    dev_pid_before = dev_pids_before[0]
    fib_pid_before = _list_module_pids(container, TARGET_MOD)[0]
    print(f"  baseline: DEV pid={dev_pid_before}, {TARGET_MOD} pid={fib_pid_before}", flush=True)

    # SIGKILL DEV：所有模块通过 PR_SET_PDEATHSIG=SIGTERM 跟着退出；
    # supervise.sh 收到 child 退出后按 backoff (CI 默认 1s) 重新 fork 一份 netnexus
    _kill_module(container, dev_pid_before)

    # 等新 DEV pid 出现且与旧 pid 不同。supervise.sh sleep BACKOFF=1s 后才 fork，
    # 加上 DEV 自身的 dev_init_all_modules 大约 5s，给宽裕一点窗口
    deadline = time.monotonic() + 30.0
    dev_pid_after = None
    while time.monotonic() < deadline:
        cur = _list_dev_pids(container)
        if len(cur) == 1 and cur[0] != dev_pid_before:
            dev_pid_after = cur[0]
            break
        time.sleep(0.3)
    if dev_pid_after is None:
        mark_step_failed()
        raise AssertionError(
            f"Phase F: supervise.sh 应在 30s 内重新拉起 DEV；旧 pid={dev_pid_before}, "
            f"当前 {_list_dev_pids(container)}"
        )
    print(f"  DEV 重启: pid {dev_pid_before} → {dev_pid_after}", flush=True)

    # DEV 死亡时旧 CLI 连接也断了，需要触发重连并等所有模块 READY
    rt.ensure_cli_alive("r1", reconnect_timeout=60)

    # 验证 fib 也回来了（DEV 重新 dev_init_all_modules，所有常驻模块都被重 fork）
    fib_pids_after = _list_module_pids(container, TARGET_MOD)
    if len(fib_pids_after) != 1 or fib_pids_after[0] == fib_pid_before:
        mark_step_failed()
        raise AssertionError(
            f"Phase F: DEV 重启后 {TARGET_MOD} 应有新 pid；旧={fib_pid_before}, 实得={fib_pids_after}"
        )
    print(f"  {TARGET_MOD} 重启: pid {fib_pid_before} → {fib_pids_after[0]}", flush=True)

    # 命令通路 sanity：show dev modules 应有 fib 行 phase=READY
    fib_row = _show_fib_row(rt, "r1")
    if "READY" not in fib_row.upper():
        mark_step_failed()
        raise AssertionError(f"Phase F: DEV 重启后 fib 行应 READY，实得: {fib_row!r}")

    step("Phase G: on-demand 模块（sbmp）崩溃也应自动 respawn + db_restore")
    on_demand_mod = "sbmp"
    sbmp_port = 17777
    # 冷启动 on-demand 不 fork；若前置 case 残留先清掉
    if _list_module_pids(container, on_demand_mod):
        cmd(rt, "r1", "config", strict=False)
        cmd(rt, "r1", "no bmp-server", strict=False)
        cmd(rt, "r1", "end", strict=False)
        _wait_for_pids(container, predicate=lambda p: not p, timeout=WAIT_RESPAWN_SEC,
                       what=f"{on_demand_mod} 预清理退出", mod=on_demand_mod)
    # 用 bmp-server 触发 DEV 按订阅 fork，并写一份 DB 配置（respawn 后验证 db_restore）。
    # 不对提示文案做断言（auto-start 文案可能是 "[auto-start] starting module"）；
    # 是否真的 fork 起来由紧随其后的 _wait_for_pids 判定。
    cmd(rt, "r1", "config")
    cmd(rt, "r1", "bmp-server", timeout=15)
    cmd(rt, "r1", f"server port {sbmp_port}")
    cmd(rt, "r1", "exit", strict=False)
    cmd(rt, "r1", "end", strict=False)
    od_pids = _wait_for_pids(container, predicate=lambda p: len(p) == 1, timeout=WAIT_RESPAWN_SEC,
                             what=f"{on_demand_mod} 按需 fork", mod=on_demand_mod)
    od_pid_v1 = od_pids[0]
    print(f"  on-demand {on_demand_mod} 已起 pid={od_pid_v1}", flush=True)

    # 关键断言：SIGKILL 一个 on-demand 模块，DEV 也应自动 respawn。
    # 修复前 on-demand 崩溃不自愈，只置 REGISTERED 等下次 SUBSCRIBE → 这里会拿不到新 pid 而失败。
    _kill_module(container, od_pid_v1)
    od_pids_v2 = _wait_for_pids(
        container,
        predicate=lambda p, op=od_pid_v1: len(p) == 1 and p[0] != op,
        timeout=WAIT_RESPAWN_SEC,
        what=f"on-demand {on_demand_mod} auto-respawn after crash",
        mod=on_demand_mod,
    )
    print(f"  on-demand crash 自愈: {od_pid_v1} → {od_pids_v2[0]}", flush=True)

    # respawn 后应自动 db_restore：重试 show 直到端口回来（init 中 subscribe(CLI) 在 restore 之后）
    deadline = time.monotonic() + 10.0
    restored = False
    while time.monotonic() < deadline:
        if str(sbmp_port) in cmd(rt, "r1", "show bmp-server", strict=False, timeout=10):
            restored = True
            break
        time.sleep(0.5)
    if not restored:
        mark_step_failed()
        raise AssertionError(
            f"Phase G: {on_demand_mod} respawn 后应 db_restore 出 port {sbmp_port}"
        )
    print(f"  on-demand {on_demand_mod} respawn 后 db_restore 成功 (port={sbmp_port})", flush=True)

    # 清理 on-demand 配置
    cmd(rt, "r1", "config", strict=False)
    cmd(rt, "r1", "no bmp-server", strict=False)
    cmd(rt, "r1", "end", strict=False)
    _wait_for_pids(container, predicate=lambda p: not p, timeout=WAIT_RESPAWN_SEC,
                   what=f"{on_demand_mod} 清理退出", mod=on_demand_mod)

    step("Phase H: 恢复 r1 拓扑基础配置")
    _restore_r1_topology_config(rt)

    print(
        f"CrashRespawn check passed: 常驻 fib 5 次 kill 全自动 respawn → 第 6 次放弃 → "
        f"process start 重置 → 再 kill 仍 respawn → DEV 自身 kill 被 supervise 拉回 → "
        f"on-demand sbmp 崩溃也自动 respawn+db_restore",
        flush=True,
    )
