#!/usr/bin/env python3
"""LDP persistence, cold-start, and revive-marker lifecycle checks.

Coverage:
- configured LDP revives from a DB startup snapshot;
- configured LDP replays from a CFG startup snapshot;
- ``no ldp`` clears protocol/interface rows and stops the module;
- neither DB nor CFG cold startup revives LDP after protocol deletion;
- legacy interface-only DB state revives through the secondary table;
- invalid canonical and non-canonical singleton rows are pruned.
"""

from __future__ import annotations

import re
import sqlite3
import subprocess
import tempfile
import time
from pathlib import Path

from module_api import (  # noqa: E402
    cold_reboot_device,
    cmd,
    require_devices,
    run_cmds,
    step,
    wait_dev_module_unloaded,
)
from top_runner import TopologyRuntime  # noqa: E402


DEV = "r1"
IFNAME = "GE-1"
CONFIGURED_DB = "ci_ldp_configured_db"
CONFIGURED_CFG = "ci_ldp_configured_cfg"
EMPTY_DB = "ci_ldp_empty_db"
EMPTY_CFG = "ci_ldp_empty_cfg"
LEGACY_INTERFACE_CFG = "ci_ldp_legacy_interface_cfg"

LSR_ID = "1.1.1.1"
HELLO_MS = 1100
HOLD_MS = 3300
KEEPALIVE_MS = 2200
IF_HELLO_MS = 1200
IF_HOLD_MS = 3600
DEFAULT_HELLO_MS = 5000
DEFAULT_HOLD_MS = 15000
DEFAULT_KEEPALIVE_MS = 10000

WAIT_PROCESS_SEC = 20.0


def _module_pids(container: str) -> list[int]:
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", "netnexus-ldp"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(f"pgrep netnexus-ldp failed: {proc.stderr}")
    return [int(value) for value in proc.stdout.split() if value.isdigit()]


def _wait_running(container: str, timeout: float = WAIT_PROCESS_SEC) -> int:
    deadline = time.monotonic() + timeout
    last: list[int] = []
    while time.monotonic() < deadline:
        last = _module_pids(container)
        if len(last) == 1:
            return last[0]
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting for one LDP process; last pids={last}")


def _wait_stopped(rt: TopologyRuntime, container: str) -> None:
    deadline = time.monotonic() + WAIT_PROCESS_SEC
    last: list[int] = []
    while time.monotonic() < deadline:
        last = _module_pids(container)
        if not last:
            break
        time.sleep(0.2)
    else:
        raise AssertionError(f"timeout waiting for LDP to stop; last pids={last}")
    wait_dev_module_unloaded(rt, DEV, "ldp", timeout=WAIT_PROCESS_SEC)


def _assert_stays_stopped(rt: TopologyRuntime, container: str, duration: float = 3.0) -> None:
    _wait_stopped(rt, container)
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        pids = _module_pids(container)
        if pids:
            raise AssertionError(f"deleted LDP configuration unexpectedly revived process; pids={pids}")
        time.sleep(0.2)


def _show_table(rt: TopologyRuntime, table: str) -> str:
    return cmd(rt, DEV, f"show db table-data {table}", strict=False, timeout=10)


def _assert_protocol_configured(rt: TopologyRuntime) -> None:
    proto = _show_table(rt, "ldp_protocol")
    if "1 row(s)" not in proto:
        raise AssertionError(f"ldp_protocol should contain one configured row:\n{proto}")
    if re.search(rf"(?m)^\s*1\s+1\s+\d+\s+{HELLO_MS}\s+{HOLD_MS}\s+{KEEPALIVE_MS}\s*$", proto) is None:
        raise AssertionError(f"ldp_protocol row does not contain the configured enabled state:\n{proto}")


def _assert_configured_tables(rt: TopologyRuntime) -> None:
    _assert_protocol_configured(rt)
    iface = _show_table(rt, "ldp_interface")
    if "1 row(s)" not in iface:
        raise AssertionError(f"ldp_interface should contain one configured row:\n{iface}")
    if re.search(rf"(?m)^\s*{re.escape(IFNAME)}\s+1\s+{IF_HELLO_MS}\s+{IF_HOLD_MS}\s*$", iface) is None:
        raise AssertionError(f"ldp_interface row does not contain the configured values:\n{iface}")


def _assert_empty_table(rt: TopologyRuntime, table: str, *, allow_absent: bool = False) -> None:
    out = _show_table(rt, table)
    if "(no rows)" in out:
        return
    if allow_absent and "not found" in out.lower():
        return
    expected = "empty or absent" if allow_absent else "empty"
    raise AssertionError(f"{table} should be {expected}:\n{out}")


def _assert_config_present(rt: TopologyRuntime) -> None:
    out = cmd(rt, DEV, "show current-configuration", timeout=20)
    required = (
        "\nldp\n",
        f" lsr-id {LSR_ID}",
        f" hello-interval {HELLO_MS}",
        f" hold-time {HOLD_MS}",
        f" keepalive-interval {KEEPALIVE_MS}",
        " ldp enable",
        f" ldp hello-interval {IF_HELLO_MS}",
        f" ldp hold-time {IF_HOLD_MS}",
    )
    missing = [text for text in required if text not in out.replace("\r", "")]
    if missing:
        raise AssertionError(f"running configuration is missing LDP commands {missing}:\n{out}")


def _assert_global_config_present(rt: TopologyRuntime) -> None:
    out = cmd(rt, DEV, "show current-configuration", timeout=20).replace("\r", "")
    required = (
        "\nldp\n",
        f" lsr-id {LSR_ID}",
        f" hello-interval {HELLO_MS}",
        f" hold-time {HOLD_MS}",
        f" keepalive-interval {KEEPALIVE_MS}",
    )
    missing = [text for text in required if text not in out]
    if missing:
        raise AssertionError(f"global-only running configuration is missing LDP commands {missing}:\n{out}")
    if re.search(r"(?m)^\s+ldp enable\s*$", out):
        raise AssertionError(f"global-only LDP unexpectedly contains interface configuration:\n{out}")


def _assert_config_absent(rt: TopologyRuntime) -> None:
    out = cmd(rt, DEV, "show current-configuration", timeout=20).replace("\r", "")
    if re.search(r"(?m)^\s*ldp(?:\s|$)", out):
        raise AssertionError(f"running configuration still contains LDP commands after no ldp:\n{out}")


def _save_and_select(rt: TopologyRuntime, name: str, mode: str) -> None:
    out = cmd(rt, DEV, f"save configuration {name}", timeout=30)
    if f"Configuration saved as '{name}'." not in out:
        raise AssertionError(f"failed to save {name}:\n{out}")
    _select_startup(rt, name, mode)


def _select_startup(rt: TopologyRuntime, name: str, mode: str) -> None:
    out = cmd(rt, DEV, f"startup configuration {name} {mode}", timeout=15)
    if f"Startup configuration set to '{name}' ({mode})." not in out:
        raise AssertionError(f"failed to select {name}/{mode}:\n{out}")


def _inject_legacy_default_marker(rt: TopologyRuntime, snapshot: str) -> None:
    container = rt.container_name(DEV)
    remote_db = f"/opt/netnexus/data/configs/{snapshot}.db"
    with tempfile.TemporaryDirectory(prefix="cnetnexus-ldp-marker-") as temp_dir:
        local_db = Path(temp_dir) / f"{snapshot}.db"
        copied = subprocess.run(
            ["docker", "cp", f"{container}:{remote_db}", str(local_db)],
            capture_output=True,
            text=True,
            check=False,
        )
        if copied.returncode != 0:
            raise RuntimeError(f"failed to copy {remote_db} from {container}: {copied.stderr}")

        conn = sqlite3.connect(local_db)
        try:
            conn.execute("DELETE FROM ldp_interface")
            conn.execute("DELETE FROM ldp_protocol")
            conn.executemany(
                "INSERT INTO ldp_protocol "
                "(inst_id, admin_up, lsr_id, hello_interval_ms, hold_time_ms, keepalive_ms) "
                "VALUES (?, 0, 16843009, 1100, 3300, 2200)",
                ((1,), (2,)),
            )
            conn.commit()
        finally:
            conn.close()

        copied = subprocess.run(
            ["docker", "cp", str(local_db), f"{container}:{remote_db}"],
            capture_output=True,
            text=True,
            check=False,
        )
        if copied.returncode != 0:
            raise RuntimeError(f"failed to restore injected {remote_db} to {container}: {copied.stderr}")


def _inject_legacy_interface_marker(rt: TopologyRuntime, snapshot: str) -> None:
    container = rt.container_name(DEV)
    remote_db = f"/opt/netnexus/data/configs/{snapshot}.db"
    with tempfile.TemporaryDirectory(prefix="cnetnexus-ldp-interface-marker-") as temp_dir:
        local_db = Path(temp_dir) / f"{snapshot}.db"
        copied = subprocess.run(
            ["docker", "cp", f"{container}:{remote_db}", str(local_db)],
            capture_output=True,
            text=True,
            check=False,
        )
        if copied.returncode != 0:
            raise RuntimeError(f"failed to copy {remote_db} from {container}: {copied.stderr}")

        conn = sqlite3.connect(local_db)
        try:
            conn.execute("DELETE FROM ldp_interface")
            conn.execute("DELETE FROM ldp_protocol")
            # Older releases could persist interface configuration without the
            # ldp_protocol marker.  Only ldp_interface can trigger this boot.
            conn.execute(
                "INSERT INTO ldp_interface "
                "(ifname, enabled, hello_interval_ms, hold_time_ms) VALUES (?, 1, ?, ?)",
                (IFNAME, IF_HELLO_MS, IF_HOLD_MS),
            )
            conn.execute(
                "INSERT INTO ldp_interface "
                "(ifname, enabled, hello_interval_ms, hold_time_ms) VALUES ('GE-2', 0, 9000, 9000)"
            )
            conn.commit()
        finally:
            conn.close()

        copied = subprocess.run(
            ["docker", "cp", str(local_db), f"{container}:{remote_db}"],
            capture_output=True,
            text=True,
            check=False,
        )
        if copied.returncode != 0:
            raise RuntimeError(f"failed to restore injected {remote_db} to {container}: {copied.stderr}")


def _assert_legacy_interface_normalized(rt: TopologyRuntime) -> None:
    proto = _show_table(rt, "ldp_protocol")
    defaults = (
        rf"(?m)^\s*1\s+0\s+0\s+{DEFAULT_HELLO_MS}\s+"
        rf"{DEFAULT_HOLD_MS}\s+{DEFAULT_KEEPALIVE_MS}\s*$"
    )
    if "1 row(s)" not in proto or re.search(defaults, proto) is None:
        raise AssertionError(f"legacy admin-down protocol fields were not normalized:\n{proto}")

    iface = _show_table(rt, "ldp_interface")
    expected = rf"(?m)^\s*{re.escape(IFNAME)}\s+1\s+{IF_HELLO_MS}\s+{IF_HOLD_MS}\s*$"
    if "1 row(s)" not in iface or re.search(expected, iface) is None or "GE-2" in iface:
        raise AssertionError(f"legacy LDP interfaces were not normalized:\n{iface}")

    current = cmd(rt, DEV, "show current-configuration", timeout=20).replace("\r", "")
    if (
        " ldp enable" not in current
        or f" ldp hello-interval {IF_HELLO_MS}" not in current
        or f" ldp hold-time {IF_HOLD_MS}" not in current
    ):
        raise AssertionError(f"normalized legacy interface config is missing from BDR:\n{current}")
    forbidden = (
        f"lsr-id {LSR_ID}",
        f"hello-interval {HELLO_MS}",
        f"hold-time {HOLD_MS}",
        f"keepalive-interval {KEEPALIVE_MS}",
    )
    if any(text in current for text in forbidden):
        raise AssertionError(f"hidden admin-down protocol config leaked into BDR:\n{current}")


def _configure_global_ldp(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        commands=[
            "config",
            "ldp",
            f"lsr-id {LSR_ID}",
            f"hello-interval {HELLO_MS}",
            f"hold-time {HOLD_MS}",
            f"keepalive-interval {KEEPALIVE_MS}",
            "exit",
            "end",
        ],
    )


def _configure_ldp_interface(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        commands=[
            "config",
            f"if {IFNAME}",
            "ldp enable",
            f"ldp hello-interval {IF_HELLO_MS}",
            f"ldp hold-time {IF_HOLD_MS}",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, (DEV,))
    container = rt.container_name(DEV)

    step("Global-only LDP is a valid configuration with an empty interface table")
    _configure_global_ldp(rt)
    _wait_running(container)
    _assert_protocol_configured(rt)
    _assert_empty_table(rt, "ldp_interface")
    _assert_global_config_present(rt)

    step("Add replayable interface LDP state")
    _configure_ldp_interface(rt)
    _assert_configured_tables(rt)
    _assert_config_present(rt)

    step("Configured LDP revives from a DB cold startup")
    _save_and_select(rt, CONFIGURED_DB, "db")
    cold_reboot_device(rt, DEV, timeout=120)
    _wait_running(container)
    _assert_configured_tables(rt)
    _assert_config_present(rt)

    step("Configured LDP replays from a CFG cold startup")
    _save_and_select(rt, CONFIGURED_CFG, "cfg")
    cold_reboot_device(rt, DEV, timeout=120)
    _wait_running(container)
    _assert_configured_tables(rt)
    _assert_config_present(rt)

    step("no ldp clears protocol and interface configuration before exiting")
    run_cmds(rt=rt, device=DEV, commands=["config", "no ldp", "end"])
    _wait_stopped(rt, container)
    _assert_empty_table(rt, "ldp_protocol")
    _assert_empty_table(rt, "ldp_interface")
    _assert_config_absent(rt)

    step("Deleted LDP configuration does not revive from a DB cold startup")
    _save_and_select(rt, EMPTY_DB, "db")
    cold_reboot_device(rt, DEV, timeout=120)
    _assert_stays_stopped(rt, container)
    _assert_empty_table(rt, "ldp_protocol")
    _assert_empty_table(rt, "ldp_interface")
    _assert_config_absent(rt)

    step("Deleted LDP configuration does not revive from a CFG cold startup")
    _save_and_select(rt, EMPTY_CFG, "cfg")
    cold_reboot_device(rt, DEV, timeout=120)
    _assert_stays_stopped(rt, container)
    _assert_empty_table(rt, "ldp_protocol", allow_absent=True)
    _assert_empty_table(rt, "ldp_interface", allow_absent=True)
    _assert_config_absent(rt)

    step("Legacy canonical and rogue protocol singletons are pruned and cannot revive twice")
    _inject_legacy_default_marker(rt, EMPTY_DB)
    _select_startup(rt, EMPTY_DB, "db")
    cold_reboot_device(rt, DEV, timeout=120)
    _wait_running(container)
    _assert_empty_table(rt, "ldp_protocol")
    _assert_empty_table(rt, "ldp_interface")
    _assert_config_absent(rt)

    # DEV may have spawned LDP once from the stale snapshot before the module
    # reconciled the row. Persist the reconciled running DB and prove the next
    # boot no longer revives it.
    _save_and_select(rt, EMPTY_DB, "db")
    cold_reboot_device(rt, DEV, timeout=120)
    _assert_stays_stopped(rt, container)
    _assert_empty_table(rt, "ldp_protocol")
    _assert_empty_table(rt, "ldp_interface")
    _assert_config_absent(rt)

    step("Legacy interface-only DB state revives via the secondary table and rebuilds its marker")
    _inject_legacy_interface_marker(rt, EMPTY_DB)
    _select_startup(rt, EMPTY_DB, "db")
    cold_reboot_device(rt, DEV, timeout=120)
    _wait_running(container)
    _assert_legacy_interface_normalized(rt)

    _save_and_select(rt, LEGACY_INTERFACE_CFG, "cfg")
    cold_reboot_device(rt, DEV, timeout=120)
    _wait_running(container)
    _assert_legacy_interface_normalized(rt)

    run_cmds(rt=rt, device=DEV, commands=["config", "no ldp", "end"])
    _wait_stopped(rt, container)
    _assert_empty_table(rt, "ldp_protocol")
    _assert_empty_table(rt, "ldp_interface")

    print("LDP DB/CFG cold-start and marker lifecycle check passed.")
