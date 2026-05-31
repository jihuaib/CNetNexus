#!/usr/bin/env python3
"""ACCESS（line 层）接入特性回归。

覆盖：
1. 出厂默认：只 console 能登录，telnet(23) 不监听；show line / show running-config 体现默认态。
2. `telnet server enable` 才监听 23；per-line `transport input telnet` 才允许该 vty 接入。
3. `show line` 反映 server 状态 + 各线 transport。
4. `line console 0` 可进入 console 视图，但视图内不支持配置命令（串口无 transport input）。
5. show current-configuration 含 ACCESS 配置块（从 DB 读：telnet server / line console 0 / line vty）。
6. reboot 后配置 + 监听从 DB 恢复；关闭后不复活。

注意：CI 经 console（串口）连设备，本用例全程经 console 验证；telnet(23) 的"是否监听"
通过 docker exec ss 直接探端口，验证两道闸门（server enable + per-line transport）的真实效果。
"""

from __future__ import annotations

import subprocess
import time

from module_api import (  # noqa: E402
    check_output,
    reboot_device,
    require_devices,
    run_cmds,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402

DEV = "r1"


def _show(rt: TopologyRuntime, command: str) -> str:
    """回到顶层再执行只读命令，返回输出。"""
    return run_cmds(rt=rt, device=DEV, strict=False, timeout=20, commands=["end", command])[-1]


def _assert(label: str, text: str, *, contains=None, not_contains=None) -> None:
    violations = check_output(text, contains=contains or [], not_contains=not_contains or [])
    if violations:
        raise RuntimeError(f"{label} 违规: {'; '.join(violations)}\n输出:\n{text}")


def _port23_listening(rt: TopologyRuntime, device: str) -> bool:
    """docker exec 进容器，看 TCP 23 是否在监听。"""
    cname = rt.container_name(device)
    proc = subprocess.run(["docker", "exec", cname, "ss", "-ltn"], text=True, capture_output=True)
    for line in proc.stdout.splitlines():
        cols = line.split()
        if len(cols) >= 4 and cols[3].endswith(":23"):
            return True
    return False


def _wait_port23(rt: TopologyRuntime, device: str, want: bool, timeout: int = 30) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _port23_listening(rt, device) == want:
            return
        time.sleep(1)
    raise RuntimeError(f"{device}: TCP 23 listening != {want} within {timeout}s")


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup (best-effort)")
    run_cmds(
        rt=rt,
        device=DEV,
        strict=False,
        commands=[
            "end",
            "config",
            "line vty 0 4",
            "transport input none",
            "exit",
            "no telnet server enable",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict) -> None:
    require_devices(top, (DEV,))

    try:
        _cleanup(rt)

        # ---- Phase A: 出厂默认（只 console，telnet 不监听）----
        step("Phase A: 默认 telnet 关闭、仅 console 可登录")
        line = _show(rt, "show line")
        _assert("default show line", line, contains=["Telnet server: disabled", "con 0", "vty 0"])
        if _port23_listening(rt, DEV):
            raise RuntimeError("Phase A: telnet 23 不应监听（默认未使能）")
        cfg = _show(rt, "show current-configuration")
        _assert(
            "default running-config",
            cfg,
            contains=["line console 0"],
            not_contains=["telnet server enable", "transport input telnet"],
        )

        # ---- Phase B: 使能 telnet server + per-line transport ----
        step("Phase B: telnet server enable + line vty 0 4 / transport input telnet")
        run_cmds(
            rt=rt,
            device=DEV,
            strict=True,
            commands=["config", "telnet server enable", "line vty 0 4", "transport input telnet", "end"],
        )
        _wait_port23(rt, DEV, want=True, timeout=20)
        line = _show(rt, "show line")
        _assert("enabled show line", line, contains=["Telnet server: enabled", "telnet"])
        cfg = _show(rt, "show current-configuration")
        _assert(
            "enabled running-config",
            cfg,
            contains=["telnet server enable", "line console 0", "line vty 0 4", "transport input telnet"],
        )

        # ---- Phase C: console 视图无配置命令 ----
        step("Phase C: line console 0 可进入但内部不支持 transport input")
        out = run_cmds(
            rt=rt,
            device=DEV,
            strict=False,
            commands=["end", "config", "line console 0", "transport input telnet"],
        )[-1]
        _assert("console view rejects config", out, contains=["Invalid command"])
        run_cmds(rt=rt, device=DEV, strict=False, commands=["end"])

        # ---- Phase D: reboot 后从 DB 恢复 ----
        step("Phase D: reboot r1，配置 + 监听应从 DB 恢复")
        reboot_device(rt, DEV, timeout=120)
        wait_check(
            rt,
            device=DEV,
            command="show current-configuration",
            timeout=60,
            interval=3,
            contains=["telnet server enable", "line console 0", "line vty 0 4", "transport input telnet"],
            label="Phase D running-config restored from DB",
        )
        _wait_port23(rt, DEV, want=True, timeout=30)

        # ---- Phase E: 关闭后验证 + 不复活 ----
        step("Phase E: no telnet server enable + transport input none")
        run_cmds(
            rt=rt,
            device=DEV,
            strict=True,
            commands=["config", "no telnet server enable", "line vty 0 4", "transport input none", "end"],
        )
        _wait_port23(rt, DEV, want=False, timeout=20)
        line = _show(rt, "show line")
        _assert("disabled show line", line, contains=["Telnet server: disabled"])
        cfg = _show(rt, "show current-configuration")
        _assert(
            "disabled running-config",
            cfg,
            not_contains=["telnet server enable", "transport input telnet"],
        )

        step("Phase E: 再次 reboot，确认关闭态被持久化（不复活）")
        reboot_device(rt, DEV, timeout=120)
        wait_check(
            rt,
            device=DEV,
            command="show current-configuration",
            timeout=60,
            interval=3,
            not_contains=["telnet server enable", "transport input telnet"],
            label="Phase E no resurrection after reboot",
        )
        if _port23_listening(rt, DEV):
            raise RuntimeError("Phase E: reboot 后 telnet 23 不应监听（已关闭并持久化）")

        print("ACCESS line transport feature passed.")
    finally:
        _cleanup(rt)
