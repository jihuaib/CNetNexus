#!/usr/bin/env python3
"""SNMP singleton configuration and on-demand revive-marker regression.

The ``snmp_config`` table is both persistent configuration and DEV's marker
for reviving the on-demand SNMP process.  It must contain exactly one row only
while a replayable trap-server configuration exists.  Both DB snapshot boot
and cfg text replay are covered.
"""

from __future__ import annotations

import sqlite3
import subprocess
import tempfile
import time
from pathlib import Path

from module_api import (  # noqa: E402
    cold_reboot_device,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402


DEV = "r1"
TRAP_HOST = "192.0.2.15"
TRAP_PORT = 55162
CFG_CONFIGURED_SNAPSHOT = "ci_snmp_cfg_configured"
CFG_EMPTY_SNAPSHOT = "ci_snmp_cfg_empty"
LEGACY_EMPTY_SNAPSHOT = "ci_snmp_legacy_empty"


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


def _assert_no_config_row(rt: TopologyRuntime, *, allow_missing_table: bool = False) -> None:
    out = _show(rt, "show db table-data snmp_config")
    if "Table 'snmp_config' not found" in out:
        if allow_missing_table:
            return
        raise AssertionError(f"snmp_config table unexpectedly missing:\n{out}")
    if "(no rows)" not in out or "1 row(s)" in out:
        raise AssertionError(f"disabled SNMP must leave an empty revive table:\n{out}")


def _save_and_select_cfg(rt: TopologyRuntime, name: str) -> None:
    out = _show(rt, f"save configuration {name}")
    expected = f"Configuration saved as '{name}'."
    if expected not in out:
        raise AssertionError(f"failed to save cfg startup snapshot {name!r}:\n{out}")

    out = _show(rt, f"startup configuration {name} cfg")
    expected = f"Startup configuration set to '{name}' (cfg)."
    if expected not in out:
        raise AssertionError(f"failed to select cfg startup snapshot {name!r}:\n{out}")


def _docker_cp(source: str, destination: str) -> None:
    proc = subprocess.run(
        ["docker", "cp", source, destination],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"docker cp failed: {source} -> {destination}\n{proc.stdout}\n{proc.stderr}")


def _save_and_select_legacy_empty_db(rt: TopologyRuntime, container: str) -> None:
    out = _show(rt, f"save configuration {LEGACY_EMPTY_SNAPSHOT}")
    expected = f"Configuration saved as '{LEGACY_EMPTY_SNAPSHOT}'."
    if expected not in out:
        raise AssertionError(f"failed to save legacy-marker fixture snapshot:\n{out}")

    snapshot = f"/opt/netnexus/data/configs/{LEGACY_EMPTY_SNAPSHOT}.db"
    with tempfile.TemporaryDirectory(prefix="netnexus-snmp-legacy-") as temp_dir:
        local_db = Path(temp_dir) / f"{LEGACY_EMPTY_SNAPSHOT}.db"
        _docker_cp(f"{container}:{snapshot}", str(local_db))
        conn = sqlite3.connect(local_db)
        try:
            table = conn.execute(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name='snmp_config'"
            ).fetchone()
            if table is None:
                raise AssertionError("legacy-marker fixture snapshot has no snmp_config table")
            conn.execute(
                "INSERT OR REPLACE INTO snmp_config (id, trap_host, trap_port) VALUES (?, ?, ?)",
                (1, "", 0),
            )
            conn.execute(
                "INSERT OR REPLACE INTO snmp_config (id, trap_host, trap_port) VALUES (?, ?, ?)",
                (2, "192.0.2.200", 162),
            )
            conn.commit()
        finally:
            conn.close()
        _docker_cp(str(local_db), f"{container}:{snapshot}")

    out = _show(rt, f"startup configuration {LEGACY_EMPTY_SNAPSHOT} db")
    expected = f"Startup configuration set to '{LEGACY_EMPTY_SNAPSHOT}' (db)."
    if expected not in out:
        raise AssertionError(f"failed to select legacy-marker fixture snapshot:\n{out}")


def _read_snmp_log(container: str) -> str:
    proc = subprocess.run(
        ["docker", "exec", container, "cat", "/opt/netnexus/log/snmp.log"],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"failed to read SNMP log:\n{proc.stdout}\n{proc.stderr}")
    return proc.stdout


def _assert_cfg_replay_success(rt: TopologyRuntime) -> None:
    out = _show(rt, "show configuration replay-failures")
    if "Configuration replay failures:" not in out or "<none>" not in out:
        raise AssertionError(f"cfg startup replay reported a failure:\n{out}")


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
        cold_reboot_device(rt, DEV, timeout=120, save_config=True)
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

        step("A cfg cold start replays the trap command and revives SNMP")
        _save_and_select_cfg(rt, CFG_CONFIGURED_SNAPSHOT)
        cold_reboot_device(rt, DEV, timeout=120)
        _wait_pids(container, running=True)
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=1,
            regex=[r"(?m)^\s*15\s+snmp\s+READY\s+\S+\s+up\s+\d+\s*$"],
            label="configured SNMP revived by cfg startup replay",
        )
        wait_check(
            rt,
            device=DEV,
            command="show current-configuration",
            timeout=30,
            interval=1,
            contains=[expected_command],
            label="SNMP trap configuration replayed from cfg startup",
        )
        _assert_config_row(rt)
        _assert_cfg_replay_success(rt)

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
        cold_reboot_device(rt, DEV, timeout=120, save_config=True)
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

        step("Legacy empty and non-canonical singleton rows are normalized on DB startup")
        _save_and_select_legacy_empty_db(rt, container)
        cold_reboot_device(rt, DEV, timeout=120)
        _wait_pids(container, running=True)
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=1,
            regex=[r"(?m)^\s*15\s+snmp\s+READY\s+\S+\s+up\s+\d+\s*$"],
            label="legacy marker starts SNMP once for normalization",
        )
        _assert_no_config_row(rt)
        if "removed invalid legacy singleton row from snmp_config" not in _read_snmp_log(container):
            raise AssertionError("SNMP did not log canonical legacy singleton normalization")
        current = _show(rt, "show current-configuration")
        if "snmp trap server" in current:
            raise AssertionError(f"legacy empty singleton produced replayable configuration:\n{current}")
        run_cmds(rt=rt, device=DEV, strict=True, commands=["config", "no snmp trap server", "end"])
        _wait_pids(container, running=False)

        step("A normalized legacy DB snapshot no longer revives SNMP")
        out = _show(rt, f"save configuration {LEGACY_EMPTY_SNAPSHOT}")
        expected = f"Configuration saved as '{LEGACY_EMPTY_SNAPSHOT}'."
        if expected not in out:
            raise AssertionError(f"failed to persist normalized legacy snapshot:\n{out}")
        cold_reboot_device(rt, DEV, timeout=120)
        _wait_pids(container, running=False)
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=1,
            regex=[r"(?m)^\s*15\s+snmp\s+ON-DEMAND\s+\S+\s+down\s+-\s*$"],
            label="normalized legacy snapshot leaves SNMP on-demand",
        )
        _assert_no_config_row(rt)

        step("An empty cfg cold start neither revives SNMP nor creates a marker row")
        _save_and_select_cfg(rt, CFG_EMPTY_SNAPSHOT)
        cold_reboot_device(rt, DEV, timeout=120)
        _wait_pids(container, running=False)
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=1,
            regex=[r"(?m)^\s*15\s+snmp\s+ON-DEMAND\s+\S+\s+down\s+-\s*$"],
            label="unconfigured SNMP remains on-demand after cfg startup",
        )
        _assert_no_config_row(rt, allow_missing_table=True)
        _assert_cfg_replay_success(rt)

        print("SNMP configuration marker lifecycle check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt, container)
