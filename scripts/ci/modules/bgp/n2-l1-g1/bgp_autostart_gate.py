#!/usr/bin/env python3
"""
BGP on-demand start gate.

Only the protocol entry command (`bgp <as-number>`) may auto-start the BGP
process. Commands already inside the BGP view must not revive a stopped BGP
process by themselves. A normal `no bgp` self-exit must also retire the CLI's
connection to BGP: the IPC layer must stay quiet past its 10-second maximum
reconnect delay, and a later protocol entry must still auto-start BGP. An
initial nonblocking connect failure must not fall through into a false DEV
`Connection lost` warning either.
"""

from __future__ import annotations

import subprocess
import time

from module_api import cmd, require_devices, run_cmds, should_skip_cleanup, step, wait_check  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


BGP_MODULE_ID = "0x00000006"
# Mirrors DEV_IPC_RECONNECT_DELAY_MAX (10,000 ms); the observation window must
# cross that boundary or a stale connection at maximum backoff could hide.
IPC_RECONNECT_DELAY_MAX_SEC = 10.0
IPC_QUIET_WINDOW_SEC = IPC_RECONNECT_DELAY_MAX_SEC + 2.0


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


def _read_module_log(container: str, module: str) -> str:
    """Read the same per-module log tree exported by the common CI runner."""
    path = f"/opt/netnexus/log/{module}.log"
    proc = subprocess.run(
        ["docker", "exec", container, "cat", path],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"failed to read {path} from {container}: "
            f"rc={proc.returncode}, stdout={proc.stdout!r}, stderr={proc.stderr!r}"
        )
    return proc.stdout


def _log_suffix(before: str, after: str, *, label: str) -> str:
    """Return newly appended log text, failing closed if the file was replaced."""
    if not after.startswith(before):
        raise AssertionError(
            f"{label} changed non-append-only while lifecycle logging was under test; "
            "cannot prove the reconnect interval is quiet"
        )
    return after[len(before) :]


def _assert_no_cli_bgp_reconnect(log_text: str) -> None:
    forbidden = (
        f"Reconnecting module({BGP_MODULE_ID})",
        f"Connection lost (module={BGP_MODULE_ID})",
    )
    found = [token for token in forbidden if token in log_text]
    if found:
        raise AssertionError(
            "normal BGP on-demand exit left a reconnecting CLI connection; "
            f"unexpected={found}\nnew cli.log text:\n{log_text}"
        )


def _assert_no_bgp_connection_lost(log_text: str, *, source: str) -> None:
    token = f"Connection lost (module={BGP_MODULE_ID})"
    if token in log_text:
        raise AssertionError(
            f"{source} reported a false BGP connection loss during normal startup; "
            f"unexpected={token!r}\nnew {source.lower()}.log text:\n{log_text}"
        )


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
        step("Start from BGP on-demand standby")
        _cleanup(rt)
        _wait_module_pids(container, "bgp", predicate=lambda p: not p, timeout=20, what="initially stopped")
        wait_check(
            rt,
            device="r1",
            command="show dev modules",
            timeout=20,
            interval=1,
            regex=[r"(?m)^\s*6\s+bgp\s+ON-DEMAND\s+\S+\s+down\s+-\s*$"],
            label="initial BGP on-demand standby",
        )

        step("Configure BGP and enter BGP view")
        cmd(rt, "r1", "config")
        dev_log_before_start = _read_module_log(container, "dev")
        out = cmd(rt, "r1", "bgp 65001", timeout=20)
        if "starting module" not in out:
            raise AssertionError(f"expected initial BGP auto-start, got:\n{out}")
        configured = True
        _wait_module_pids(container, "bgp", predicate=lambda p: len(p) == 1, timeout=20, what="running")
        wait_check(
            rt,
            device="r1",
            command="show dev modules",
            timeout=20,
            interval=1,
            regex=[r"(?m)^\s*6\s+bgp\s+READY\s+\S+\s+up\s+\d+\s*$"],
            label="initial BGP auto-start ready",
        )
        dev_log_after_start = _read_module_log(container, "dev")
        dev_log_during_start = _log_suffix(
            dev_log_before_start,
            dev_log_after_start,
            label="dev.log",
        )
        _assert_no_bgp_connection_lost(dev_log_during_start, source="DEV")
        cmd(rt, "r1", "router-id 1.1.1.1")

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

        step("no bgp exits normally and returns BGP to on-demand standby")
        run_cmds(rt=rt, device="r1", commands=["end", "config"])
        cli_log_before_exit = _read_module_log(container, "cli")
        out = cmd(rt, "r1", "no bgp", timeout=20)
        if "BGP: configuration cleared, process exiting." not in out:
            raise AssertionError(f"expected normal BGP self-exit response, got:\n{out}")
        cmd(rt, "r1", "end")
        configured = False
        _wait_module_pids(container, "bgp", predicate=lambda p: not p, timeout=20, what="normally exited")
        wait_check(
            rt,
            device="r1",
            command="show dev modules",
            timeout=20,
            interval=1,
            regex=[r"(?m)^\s*6\s+bgp\s+ON-DEMAND\s+\S+\s+down\s+-\s*$"],
            label="BGP returns to on-demand standby",
        )

        step("CLI stays quiet beyond the maximum IPC reconnect delay")
        quiet_started = time.monotonic()
        deadline = quiet_started + IPC_QUIET_WINDOW_SEC
        while time.monotonic() < deadline:
            pids = _module_pids(container, "bgp")
            if pids:
                raise AssertionError(f"BGP restarted during the IPC quiet window; pids={pids}")
            time.sleep(min(0.5, max(0.0, deadline - time.monotonic())))
        cli_log_after_window = _read_module_log(container, "cli")
        cli_log_during_exit = _log_suffix(
            cli_log_before_exit,
            cli_log_after_window,
            label="cli.log",
        )
        _assert_no_cli_bgp_reconnect(cli_log_during_exit)
        quiet_elapsed = time.monotonic() - quiet_started
        print(
            f"CLI/BGP IPC stayed quiet for {quiet_elapsed:.1f}s "
            f"(max reconnect delay: {IPC_RECONNECT_DELAY_MAX_SEC:.1f}s)",
            flush=True,
        )

        step("BGP protocol entry still auto-starts after the quiet on-demand interval")
        out = cmd(rt, "r1", "config")
        out += cmd(rt, "r1", "bgp 65001", timeout=20)
        if "starting module" not in out:
            raise AssertionError(f"expected BGP auto-start after normal self-exit, got:\n{out}")
        configured = True
        _wait_module_pids(
            container,
            "bgp",
            predicate=lambda p: len(p) == 1,
            timeout=20,
            what="auto-started after quiet interval",
        )
        wait_check(
            rt,
            device="r1",
            command="show dev modules",
            timeout=20,
            interval=1,
            regex=[r"(?m)^\s*6\s+bgp\s+READY\s+\S+\s+up\s+\d+\s*$"],
            label="BGP ready after quiet-interval auto-start",
        )
    finally:
        if not should_skip_cleanup():
            if configured:
                _cleanup_configured(rt, container)
            else:
                _cleanup(rt)
