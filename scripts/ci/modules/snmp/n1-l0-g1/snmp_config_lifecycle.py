#!/usr/bin/env python3
"""SNMP singleton configuration and on-demand revive-marker regression.

The ``snmp_config`` table is both persistent configuration and DEV's marker
for reviving the on-demand SNMP process.  It must contain exactly one row only
while a replayable trap-server configuration exists.
"""

from __future__ import annotations

import subprocess
import time

from module_api import cmd, reboot_device, require_devices, run_cmds, should_skip_cleanup, step, wait_check  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


DEV = "r1"
TRAP_HOST = "192.0.2.15"
TRAP_PORT = 55162


def _module_pids(container: str) -> list[int]:
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", "netnexus-snmp"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(f"pgrep netnexus-snmp failed: {proc.stderr}")
    return [int(value) for value in proc.stdout.split() if value.isdigit()]


def _wait_pids(container: str, *, running: bool, timeout: float = 20.0) -> list[int]:
    deadline = time.monotonic() + timeout
    pids: list[int] = []
    while time.monotonic() < deadline:
        pids = _module_pids(container)
        if bool(pids) == running:
            return pids
        time.sleep(0.2)
    state = "running" if running else "stopped"
    raise AssertionError(f"timeout waiting for SNMP to be {state}; last pids={pids}")


def _show(rt: TopologyRuntime, command: str) -> str:
    return run_cmds(rt=rt, device=DEV, strict=False, timeout=30, commands=["end", command])[-1]


def _assert_config_row(rt: TopologyRuntime) -> None:
    out = _show(rt, "show db table-data snmp_config")
    expected = (TRAP_HOST, str(TRAP_PORT), "1 row(s)")
    missing = [token for token in expected if token not in out]
    if missing:
        raise AssertionError(f"configured snmp_config marker is incomplete; missing={missing}\n{out}")
    if "(no rows)" in out:
        raise AssertionError(f"configured snmp_config unexpectedly has no rows:\n{out}")


def _assert_no_config_row(rt: TopologyRuntime) -> None:
    out = _show(rt, "show db table-data snmp_config")
    if "(no rows)" not in out or "1 row(s)" in out:
        raise AssertionError(f"disabled SNMP must leave an empty revive table:\n{out}")


def _cleanup(rt: TopologyRuntime, container: str) -> None:
    if _module_pids(container):
        run_cmds(rt=rt, device=DEV, strict=False, commands=["end", "config", "no snmp trap server", "end"])
        _wait_pids(container, running=False)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, (DEV,))
    container = rt.container_name(DEV)

    try:
        step("Start with SNMP in on-demand standby")
        _cleanup(rt, container)
        _wait_pids(container, running=False)
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=20,
            interval=1,
            regex=[r"(?m)^\s*15\s+snmp\s+ON-DEMAND\s+\S+\s+down\s+-\s*$"],
            label="initial SNMP on-demand standby",
        )

        step("A replayable trap configuration creates exactly one marker row")
        run_cmds(
            rt=rt,
            device=DEV,
            strict=True,
            commands=["config", f"snmp trap server {TRAP_HOST} port {TRAP_PORT}", "end"],
        )
        _wait_pids(container, running=True)
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=20,
            interval=1,
            regex=[r"(?m)^\s*15\s+snmp\s+READY\s+\S+\s+up\s+\d+\s*$"],
            label="configured SNMP ready",
        )
        _assert_config_row(rt)
        current = _show(rt, "show current-configuration")
        expected_command = f"snmp trap server {TRAP_HOST} port {TRAP_PORT}"
        if expected_command not in current:
            raise AssertionError(f"SNMP BDR omitted configured trap server:\n{current}")

        step("A saved configured marker revives SNMP and restores its configuration")
        reboot_device(rt, DEV, timeout=120, save_config=True)
        _wait_pids(container, running=True)
        wait_check(
            rt,
            device=DEV,
            command="show current-configuration",
            timeout=30,
            interval=1,
            contains=[expected_command],
            label="SNMP trap configuration restored after reboot",
        )
        _assert_config_row(rt)

        step("no snmp deletes the marker row and returns the process to standby")
        run_cmds(rt=rt, device=DEV, strict=True, commands=["config", "no snmp trap server", "end"])
        _wait_pids(container, running=False)
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=20,
            interval=1,
            regex=[r"(?m)^\s*15\s+snmp\s+ON-DEMAND\s+\S+\s+down\s+-\s*$"],
            label="disabled SNMP on-demand standby",
        )
        _assert_no_config_row(rt)
        current = _show(rt, "show current-configuration")
        if "snmp trap server" in current:
            raise AssertionError(f"disabled SNMP remained in running configuration:\n{current}")

        step("An empty saved marker table does not revive SNMP after reboot")
        reboot_device(rt, DEV, timeout=120, save_config=True)
        _wait_pids(container, running=False)
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=1,
            regex=[r"(?m)^\s*15\s+snmp\s+ON-DEMAND\s+\S+\s+down\s+-\s*$"],
            label="unconfigured SNMP remains on-demand after reboot",
        )
        _assert_no_config_row(rt)

        print("SNMP configuration marker lifecycle check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt, container)
