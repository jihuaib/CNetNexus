#!/usr/bin/env python3
"""
BGP on-demand start gate.

Only the protocol entry command (`bgp <as-number>`) may auto-start the BGP
process. Commands already inside the BGP view must not revive a stopped BGP
process by themselves.
"""

from __future__ import annotations

import subprocess
import time

from module_api import cmd, require_devices, run_cmds, should_skip_cleanup, step  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


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


def _cleanup(rt: TopologyRuntime) -> None:
    run_cmds(rt=rt, device="r1", strict=False, commands=["end", "config", "no bgp", "end"])


def _cleanup_configured(rt: TopologyRuntime, container: str) -> None:
    if not _module_pids(container, "bgp"):
        cmd(rt, "r1", "end", strict=False)
        cmd(rt, "r1", "process start bgp", strict=False, timeout=20)
        _wait_module_pids(container, "bgp", predicate=lambda p: len(p) == 1, timeout=20, what="running for cleanup")
    _cleanup(rt)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))
    container = rt.container_name("r1")
    configured = False

    try:
        _cleanup(rt)

        step("Configure BGP and enter BGP view")
        run_cmds(rt=rt, device="r1", commands=["config", "bgp 65001", "router-id 1.1.1.1"])
        configured = True
        _wait_module_pids(container, "bgp", predicate=lambda p: len(p) == 1, timeout=20, what="running")

        step("Stop BGP while CLI remains in BGP view")
        cmd(rt, "r1", "process stop bgp", strict=False, timeout=20)
        _wait_module_pids(container, "bgp", predicate=lambda p: not p, timeout=20, what="stopped")

        step("BGP view command must not auto-start BGP")
        out = cmd(rt, "r1", "router-id 1.1.1.2", strict=False)
        if "Error: target module is not running; command not applied" not in out:
            raise AssertionError(f"expected non-auto-start response, got:\n{out}")
        _wait_module_pids(container, "bgp", predicate=lambda p: not p, timeout=3, what="still stopped")

        step("Only bgp <as-number> auto-starts BGP")
        run_cmds(rt=rt, device="r1", commands=["end", "config", "bgp 65001"])
        _wait_module_pids(container, "bgp", predicate=lambda p: len(p) == 1, timeout=20, what="restarted")
    finally:
        if not should_skip_cleanup():
            if configured:
                _cleanup_configured(rt, container)
            else:
                _cleanup(rt)
