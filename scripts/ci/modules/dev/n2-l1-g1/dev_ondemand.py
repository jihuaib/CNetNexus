#!/usr/bin/env python3
"""
端到端验证按需模块 (SBMP) + 订阅启动 + reboot process + 自动恢复机制。

覆盖：
  Phase A. boot 完成 → SBMP 进程不存在（DB 空）
  Phase B. `bmp-server` 命令触发 SBMP 启动（DEV 按订阅 fork）
  Phase C. `no bmp-server` 让 SBMP 进程自退出（raise SIGTERM）
  Phase D. 重新配置 → 整机 `reboot` → boot 时 SBMP 由 revive-table 自动 spawn
  Phase E. `reboot process sbmp` SIGTERM + 自动 respawn，DB 配置不丢

跑在 dev/n2-l1-g1 拓扑里，只用 r1。
"""

from __future__ import annotations

import re
import subprocess
import time

from module_api import cmd, mark_step_failed, require_devices, step  # noqa: E402
from top_runner import TopologyRuntime, run_cmd  # noqa: E402


SBMP_PORT = 17777
WAIT_SPAWN_SEC = 10
WAIT_EXIT_SEC = 8


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


def _list_sbmp_pids(container: str) -> list[int]:
    return _list_module_pids(container, "sbmp")


def _wait_for_pids(container: str, *, predicate, timeout: float, what: str,
                   mod: str = "sbmp") -> list[int]:
    """轮询 pids，predicate(pids) == True 即返回；超时抛错并打印最近 pids。"""
    deadline = time.monotonic() + timeout
    pids: list[int] = []
    while time.monotonic() < deadline:
        pids = _list_module_pids(container, mod)
        if predicate(pids):
            return pids
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting {what}; last {mod} pids={pids}")


def _show_retry(rt, device: str, command: str, *, must_contain: str, timeout: float) -> str:
    """重试发 show 直到响应包含 must_contain 或超时。

    用于刚 reboot/spawn 模块的场景：模块进程已起来但还在 init，CFG 暂时还没收到
    它的 subscribe(CLI) → is_connected=false → 'Info: not running'。
    模块完成 init 后 CFG 看到 inbound 连接，show 才有真数据。
    """
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        out = cmd(rt, device, command, timeout=10)
        if must_contain in out:
            return out
        last = out
        time.sleep(0.5)
    raise AssertionError(f"timeout waiting '{must_contain}' in `{command}`; last output:\n{last}")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))
    container = rt.container_name("r1")

    try:
        step("Phase A: SBMP not running at boot (empty DB)")
        # 注意：本 case 跟 dev_swap_image.py 共用拓扑；若之前的 case 已配过 SBMP，跳过断言
        pids = _list_sbmp_pids(container)
        if pids:
            # 兼容前置 case 残留：把 SBMP 配置清掉，等进程退出
            cmd(rt, "r1", "config", strict=False)
            cmd(rt, "r1", "no bmp-server", strict=False)
            cmd(rt, "r1", "end", strict=False)
            _wait_for_pids(container, predicate=lambda p: not p, timeout=WAIT_EXIT_SEC,
                           what="sbmp to exit after pre-cleanup")
        pids = _list_sbmp_pids(container)
        if pids:
            mark_step_failed()
            raise AssertionError(f"Phase A: SBMP should not be running; pids={pids}")

        step("Phase B: 'bmp-server' triggers SBMP spawn")
        out = cmd(rt, "r1", "config")
        out += cmd(rt, "r1", "bmp-server", timeout=15)
        if "Starting module" not in out:
            mark_step_failed()
            raise AssertionError(f"Phase B: missing 'Starting module' prompt:\n{out}")
        pids_after_spawn = _wait_for_pids(
            container, predicate=lambda p: len(p) == 1, timeout=WAIT_SPAWN_SEC,
            what="exactly 1 sbmp pid after bmp-server")
        sbmp_pid_v1 = pids_after_spawn[0]
        # 完成基础配置
        cmd(rt, "r1", f"server port {SBMP_PORT}")
        cmd(rt, "r1", "exit")
        cmd(rt, "r1", "end")

        step("Phase C: 'no bmp-server' makes SBMP self-exit")
        cmd(rt, "r1", "config")
        cmd(rt, "r1", "no bmp-server", timeout=10)
        cmd(rt, "r1", "end", strict=False)
        _wait_for_pids(container, predicate=lambda p: sbmp_pid_v1 not in p,
                       timeout=WAIT_EXIT_SEC, what=f"old sbmp pid {sbmp_pid_v1} to exit")
        remaining = _list_sbmp_pids(container)
        if remaining:
            mark_step_failed()
            raise AssertionError(
                f"Phase C: no sbmp process should remain after 'no bmp-server'; got {remaining}"
            )

        step("Phase D: reconfigure + full reboot → SBMP auto-revives at boot")
        # 重新配一份
        cmd(rt, "r1", "config")
        cmd(rt, "r1", "bmp-server", timeout=15)
        cmd(rt, "r1", f"server port {SBMP_PORT}")
        cmd(rt, "r1", "exit")
        cmd(rt, "r1", "end")
        pre_reboot_pids = _wait_for_pids(container, predicate=lambda p: len(p) == 1,
                                          timeout=WAIT_SPAWN_SEC,
                                          what="sbmp to be running before reboot")

        rt.reboot_device("r1", reconnect_timeout=120)

        # boot 完成后，由 dev_revive_on_demand_modules 触发；不需要再发 CLI 触发
        post_reboot_pids = _wait_for_pids(
            container, predicate=lambda p: len(p) == 1, timeout=WAIT_SPAWN_SEC,
            what="sbmp to be auto-revived after reboot")
        if post_reboot_pids[0] == pre_reboot_pids[0]:
            mark_step_failed()
            raise AssertionError(
                f"Phase D: pid should change across reboot; pre={pre_reboot_pids} post={post_reboot_pids}"
            )
        # 端口已恢复（说明 SBMP 已 db_restore + listen + subscribe(CLI)）。
        # 用重试：SBMP init 中 subscribe(CLI) 在 db_restore 之后，CFG 看到连接才能 dispatch。
        out = _show_retry(rt, "r1", "show bmp-server", must_contain=str(SBMP_PORT), timeout=10)

        step("Phase E: 'reboot process sbmp' restarts process, config preserved")
        old_pid = post_reboot_pids[0]
        out = cmd(rt, "r1", "reboot process sbmp", timeout=10)
        if "reboot process sbmp" not in out and "respawn" not in out.lower():
            mark_step_failed()
            raise AssertionError(f"Phase E: unexpected reboot process response:\n{out}")
        # 等旧 pid 消失 + 新 pid 出现
        _wait_for_pids(container, predicate=lambda p: old_pid not in p,
                       timeout=WAIT_EXIT_SEC, what=f"old sbmp pid {old_pid} to exit on reboot process")
        new_pids = _wait_for_pids(container, predicate=lambda p: len(p) == 1 and p[0] != old_pid,
                                   timeout=WAIT_SPAWN_SEC, what="new sbmp pid after reboot process")
        # DB 配置应在；新进程 init 完成 subscribe(CLI) 后 CFG 才能 dispatch
        out = _show_retry(rt, "r1", "show bmp-server", must_contain=str(SBMP_PORT), timeout=10)

        step("Phase F: TUNNEL on-demand idle (show is read-only, must NOT trigger spawn)")
        tun_pids_boot = _list_module_pids(container, "tunnel")
        if tun_pids_boot:
            mark_step_failed()
            raise AssertionError(
                f"Phase F: TUNNEL should not run at boot (no DB persistence); got {tun_pids_boot}"
            )
        # show 不应拉起 TUNNEL；应得到 "Info: target module is not running" 之类提示
        out = cmd(rt, "r1", "show tunnel label", timeout=10)
        if "not running" not in out.lower():
            mark_step_failed()
            raise AssertionError(
                f"Phase F: show command should report module not running (no side effect):\n{out}"
            )
        # 等几秒确认 TUNNEL 没被偷偷拉起
        time.sleep(2)
        tun_pids_post = _list_module_pids(container, "tunnel")
        if tun_pids_post:
            mark_step_failed()
            raise AssertionError(
                f"Phase F: show command should NOT spawn TUNNEL; got {tun_pids_post}"
            )

        step("Phase G: LDP on-demand + 模块依赖（LDP 拉起 TUNNEL）")
        ldp_pids_boot = _list_module_pids(container, "ldp")
        if ldp_pids_boot:
            mark_step_failed()
            raise AssertionError(f"Phase G: LDP should not run at boot; got {ldp_pids_boot}")
        # 进 ldp 配置视图：触发 LDP 按需启动
        out = cmd(rt, "r1", "config", timeout=10)
        out += cmd(rt, "r1", "ldp", timeout=15)
        if "Starting module" not in out:
            mark_step_failed()
            raise AssertionError(f"Phase G: expected 'Starting module' prompt for LDP:\n{out}")
        ldp_pids = _wait_for_pids(
            container, predicate=lambda p: len(p) == 1, timeout=WAIT_SPAWN_SEC,
            what="LDP to spawn", mod="ldp")
        # 验证模块依赖：LDP 在 init 中 subscribe(TUNNEL, auto_start=1) 会让 DEV 把 TUNNEL 也拉起
        tun_pids = _wait_for_pids(
            container, predicate=lambda p: len(p) == 1, timeout=WAIT_SPAWN_SEC,
            what="TUNNEL to be pulled up by LDP", mod="tunnel")
        cmd(rt, "r1", "lsr-id 1.1.1.1", timeout=5)
        cmd(rt, "r1", "exit", strict=False)
        cmd(rt, "r1", "end", strict=False)

        step("Phase H: ISIS on-demand")
        isis_pids_boot = _list_module_pids(container, "isis")
        if isis_pids_boot:
            mark_step_failed()
            raise AssertionError(f"Phase H: ISIS should not run at boot; got {isis_pids_boot}")
        out = cmd(rt, "r1", "config", timeout=5)
        out += cmd(rt, "r1", "isis 1", timeout=15)
        if "Starting module" not in out:
            mark_step_failed()
            raise AssertionError(f"Phase H: expected 'Starting module' for ISIS:\n{out}")
        isis_pids = _wait_for_pids(
            container, predicate=lambda p: len(p) == 1, timeout=WAIT_SPAWN_SEC,
            what="ISIS to spawn", mod="isis")
        cmd(rt, "r1", "net 49.0001.0000.0000.0001.00", timeout=5)
        cmd(rt, "r1", "exit", strict=False)
        cmd(rt, "r1", "end", strict=False)

        step("Phase I: BGP on-demand + 拉起 TUNNEL（依赖关系）")
        bgp_pids_boot = _list_module_pids(container, "bgp")
        if bgp_pids_boot:
            mark_step_failed()
            raise AssertionError(f"Phase I: BGP should not run at boot; got {bgp_pids_boot}")
        out = cmd(rt, "r1", "config", timeout=5)
        out += cmd(rt, "r1", "bgp 65001", timeout=15)
        if "Starting module" not in out:
            mark_step_failed()
            raise AssertionError(f"Phase I: expected 'Starting module' for BGP:\n{out}")
        bgp_pids = _wait_for_pids(
            container, predicate=lambda p: len(p) == 1, timeout=WAIT_SPAWN_SEC,
            what="BGP to spawn", mod="bgp")
        cmd(rt, "r1", "router-id 1.1.1.1", timeout=5)
        cmd(rt, "r1", "exit", strict=False)
        cmd(rt, "r1", "end", strict=False)

        print(
            f"OnDemand check passed: SBMP(pid={sbmp_pid_v1}→{pre_reboot_pids[0]}→{old_pid}→{new_pids[0]})"
            f", TUNNEL(idle then pulled by LDP pid={tun_pids[0]})"
            f", LDP(spawn pid={ldp_pids[0]})"
            f", ISIS(spawn pid={isis_pids[0]})"
            f", BGP(spawn pid={bgp_pids[0]})"
        )

    finally:
        # 清理：把 SBMP/LDP/ISIS/BGP 配置清掉
        try:
            cmd(rt, "r1", "config", strict=False)
            cmd(rt, "r1", "no bmp-server", strict=False)
            cmd(rt, "r1", "no ldp", strict=False)
            cmd(rt, "r1", "no isis 1", strict=False)
            cmd(rt, "r1", "no bgp", strict=False)
            cmd(rt, "r1", "end", strict=False)
        except Exception as e:
            print(f"cleanup warn: {e}", flush=True)
