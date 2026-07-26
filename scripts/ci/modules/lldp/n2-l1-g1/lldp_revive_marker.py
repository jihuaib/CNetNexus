#!/usr/bin/env python3
"""
LLDP on-demand revive-marker regression coverage.

The lldp_protocol singleton is both protocol configuration and DEV's on-demand
revive marker.  It must be absent when LLDP only has in-memory defaults, remain
present for replayable protocol or interface settings, and become empty again
after the last setting is undone.  The case also cold-boots both DB and CFG
startup modes, including a CFG containing only an interface-level negative
override and the legacy positive ``enable -> admin-status -> description``
sequence.
"""

from __future__ import annotations

import re
import sqlite3
import subprocess
import tempfile
import time
from pathlib import Path

from module_api import cold_reboot_device, cmd, process_start, require_devices, run_cmds, step  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


MODULE = "lldp"
MARKER_TABLE = "lldp_protocol"
NONDEFAULT_TIMER = 7
CONFIGURED_SNAPSHOT = "ci_lldp_marker_configured"
EMPTY_SNAPSHOT = "ci_lldp_marker_empty"
INTERFACE_SNAPSHOT = "ci_lldp_marker_interface"
DESCRIPTION_ONLY_SNAPSHOT = "ci_lldp_marker_description_only"
POSITIVE_INTERFACE_SNAPSHOT = "ci_lldp_marker_positive_interface"
ADMIN_ONLY_SNAPSHOT = "ci_lldp_marker_admin_only"
INTERFACE = "GE-1"
PORT_DESCRIPTION = "marker-only"
POSITIVE_ADMIN_STATUS = "txonly"
ADMIN_ONLY_STATUS = "rxonly"
WAIT_PROCESS_SEC = 12.0


def _module_pids(container: str) -> list[int]:
    proc = subprocess.run(
        ["docker", "exec", container, "pgrep", "-x", "-f", f"netnexus-{MODULE}"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode not in (0, 1):
        raise RuntimeError(f"pgrep {MODULE} failed (rc={proc.returncode}): {proc.stderr}")
    return [int(value) for value in proc.stdout.split() if value.isdigit()]


def _wait_pids(container: str, *, running: bool, what: str) -> list[int]:
    deadline = time.monotonic() + WAIT_PROCESS_SEC
    last: list[int] = []
    while time.monotonic() < deadline:
        last = _module_pids(container)
        if bool(last) == running:
            return last
        time.sleep(0.2)
    raise AssertionError(f"timeout waiting for {what}; last {MODULE} pids={last}")


def _marker_data(rt: TopologyRuntime) -> str:
    return cmd(rt, "r1", f"show db table-data {MARKER_TABLE}")


def _assert_marker_empty(rt: TopologyRuntime, phase: str) -> None:
    out = _marker_data(rt)
    if "(no rows)" not in out:
        raise AssertionError(f"{phase}: expected empty {MARKER_TABLE}, got:\n{out}")


def _assert_marker_timer(rt: TopologyRuntime, phase: str) -> None:
    out = _marker_data(rt)
    if "(no rows)" in out or f"{NONDEFAULT_TIMER}" not in out or "1 row(s)" not in out:
        raise AssertionError(
            f"{phase}: expected one {MARKER_TABLE} row with timer {NONDEFAULT_TIMER}, got:\n{out}"
        )


def _assert_marker_present(rt: TopologyRuntime, phase: str) -> None:
    out = _marker_data(rt)
    if "(no rows)" in out or "1 row(s)" not in out:
        raise AssertionError(f"{phase}: expected one {MARKER_TABLE} row, got:\n{out}")


def _assert_interface_empty(rt: TopologyRuntime, phase: str) -> None:
    out = cmd(rt, "r1", "show db table-data lldp_interface")
    if "(no rows)" not in out:
        raise AssertionError(f"{phase}: expected empty lldp_interface, got:\n{out}")


def _assert_negative_interface(rt: TopologyRuntime, phase: str) -> None:
    db_out = cmd(rt, "r1", "show db table-data lldp_interface")
    if "(no rows)" in db_out or "1 row(s)" not in db_out or INTERFACE not in db_out:
        raise AssertionError(f"{phase}: expected persisted {INTERFACE} LLDP interface row, got:\n{db_out}")

    current = cmd(rt, "r1", "show current-configuration")
    if f"if {INTERFACE}" not in current or " no lldp enable" not in current:
        raise AssertionError(f"{phase}: negative LLDP interface override missing from BDR:\n{current}")


def _assert_positive_interface(rt: TopologyRuntime, phase: str) -> None:
    _assert_marker_present(rt, phase)
    db_out = cmd(rt, "r1", "show db table-data lldp_interface")
    required = ("1 row(s)", INTERFACE, PORT_DESCRIPTION)
    missing = [token for token in required if token not in db_out]
    if missing or "(no rows)" in db_out:
        raise AssertionError(
            f"{phase}: positive LLDP interface row is incomplete; missing={missing}\n{db_out}"
        )

    current = cmd(rt, "r1", "show current-configuration").replace("\r", "")
    canonical = (
        rf"(?ms)^if {re.escape(INTERFACE)}[ \t]*$"
        rf".*?^[ \t]+lldp enable[ \t]*$"
        rf".*?^[ \t]+lldp admin-status {POSITIVE_ADMIN_STATUS}[ \t]*$"
        rf".*?^[ \t]+lldp port-description {re.escape(PORT_DESCRIPTION)}[ \t]*$"
    )
    if re.search(canonical, current) is None:
        raise AssertionError(
            f"{phase}: BDR did not preserve enable/admin/description replay order:\n{current}"
        )
    if re.search(r"(?m)^\s*no lldp enable\s*$", current):
        raise AssertionError(f"{phase}: positive interface state rendered as a negative override:\n{current}")


def _assert_description_only_interface(rt: TopologyRuntime, phase: str) -> None:
    _assert_marker_present(rt, phase)
    db_out = cmd(rt, "r1", "show db table-data lldp_interface")
    required = ("1 row(s)", INTERFACE, PORT_DESCRIPTION)
    missing = [token for token in required if token not in db_out]
    if missing or "(no rows)" in db_out:
        raise AssertionError(
            f"{phase}: description-only LLDP row is incomplete; missing={missing}\n{db_out}"
        )

    current = cmd(rt, "r1", "show current-configuration").replace("\r", "")
    canonical = (
        rf"(?ms)^if {re.escape(INTERFACE)}[ \t]*$"
        rf".*?^[ \t]+lldp enable[ \t]*$"
        rf".*?^[ \t]+lldp port-description {re.escape(PORT_DESCRIPTION)}[ \t]*$"
    )
    if re.search(canonical, current) is None:
        raise AssertionError(
            f"{phase}: BDR omitted the compatible enable/description replay sequence:\n{current}"
        )
    if re.search(r"(?m)^[ \t]+lldp admin-status[ \t]+", current):
        raise AssertionError(f"{phase}: description-only configuration gained admin status:\n{current}")
    if re.search(r"(?m)^[ \t]+no lldp enable[ \t]*$", current):
        raise AssertionError(f"{phase}: description-only state rendered as a negative override:\n{current}")


def _assert_admin_only_interface(rt: TopologyRuntime, phase: str) -> None:
    _assert_marker_present(rt, phase)
    db_out = cmd(rt, "r1", "show db table-data lldp_interface")
    if "(no rows)" in db_out or "1 row(s)" not in db_out or INTERFACE not in db_out:
        raise AssertionError(f"{phase}: expected one admin-only interface row:\n{db_out}")

    current = cmd(rt, "r1", "show current-configuration").replace("\r", "")
    canonical = (
        rf"(?ms)^if {re.escape(INTERFACE)}[ \t]*$"
        rf".*?^[ \t]+lldp enable[ \t]*$"
        rf".*?^[ \t]+lldp admin-status {ADMIN_ONLY_STATUS}[ \t]*$"
    )
    if re.search(canonical, current) is None:
        raise AssertionError(f"{phase}: admin-only legacy replay sequence is missing:\n{current}")
    if "lldp port-description" in current:
        raise AssertionError(f"{phase}: admin-only configuration gained a port description:\n{current}")


def _assert_cfg_replay_clean(rt: TopologyRuntime, phase: str) -> None:
    out = cmd(rt, "r1", "show configuration replay-failures")
    if "Configuration replay failures:" not in out or "<none>" not in out:
        raise AssertionError(f"{phase}: startup/cfg replay reported failures:\n{out}")


def _save_startup(rt: TopologyRuntime, name: str, mode: str) -> None:
    if mode not in ("db", "cfg"):
        raise ValueError(f"unsupported startup mode: {mode}")
    run_cmds(
        rt,
        "r1",
        commands=[
            "end",
            f"save configuration {name}",
            f"startup configuration {name} {mode}",
        ],
    )


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


def _inject_legacy_default_interface_only(rt: TopologyRuntime, container: str) -> None:
    remote_db = f"/opt/netnexus/data/configs/{EMPTY_SNAPSHOT}.db"
    with tempfile.TemporaryDirectory(prefix="netnexus-lldp-interface-only-") as temp_dir:
        local_db = Path(temp_dir) / f"{EMPTY_SNAPSHOT}.db"
        _docker_cp(f"{container}:{remote_db}", str(local_db))
        conn = sqlite3.connect(local_db)
        try:
            conn.execute("DELETE FROM lldp_interface")
            conn.execute("DELETE FROM lldp_protocol")
            # This is the old inconsistency being migrated: only the secondary
            # revive table has a row, and that row is an implicit default.
            conn.execute(
                "INSERT INTO lldp_interface "
                "(ifname, enabled, admin_status, tx_interval_sec, hold_multiplier, port_desc) "
                "VALUES (?, 1, 3, 0, 0, '')",
                (INTERFACE,),
            )
            conn.commit()
        finally:
            conn.close()
        _docker_cp(str(local_db), f"{container}:{remote_db}")

    run_cmds(rt, "r1", commands=["end", f"startup configuration {EMPTY_SNAPSHOT} db"])


def _inject_legacy_protocol_rows(rt: TopologyRuntime, container: str) -> None:
    remote_db = f"/opt/netnexus/data/configs/{EMPTY_SNAPSHOT}.db"
    with tempfile.TemporaryDirectory(prefix="netnexus-lldp-protocol-dirty-") as temp_dir:
        local_db = Path(temp_dir) / f"{EMPTY_SNAPSHOT}.db"
        _docker_cp(f"{container}:{remote_db}", str(local_db))
        conn = sqlite3.connect(local_db)
        try:
            conn.execute("DELETE FROM lldp_interface")
            conn.execute("DELETE FROM lldp_protocol")
            conn.executemany(
                "INSERT INTO lldp_protocol "
                "(inst_id, admin_up, tx_interval_sec, hold_multiplier, reinit_delay_sec, tx_delay_sec) "
                "VALUES (?, ?, ?, ?, ?, ?)",
                (
                    # Hidden fields have no CLI/BDR representation.
                    (1, 0, 30, 4, 99, 88),
                    # A non-canonical singleton must not survive or revive LLDP forever.
                    (2, 1, 9, 9, 9, 9),
                ),
            )
            conn.commit()
        finally:
            conn.close()
        _docker_cp(str(local_db), f"{container}:{remote_db}")

    run_cmds(rt, "r1", commands=["end", f"startup configuration {EMPTY_SNAPSHOT} db"])


def _reset_marker_state(rt: TopologyRuntime, container: str) -> None:
    if not _module_pids(container):
        process_start(rt, "r1", MODULE, ready_timeout=30)
    _wait_pids(container, running=True, what="LLDP cleanup start")
    run_cmds(
        rt,
        "r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"if {INTERFACE}",
            "lldp enable",
            "no lldp port-description",
            "no lldp admin-status",
            "exit",
            "no lldp hold-multiplier",
            "no lldp timer",
            "no lldp",
            "end",
        ],
    )
    _wait_pids(container, running=False, what="LLDP cleanup completion")
    _assert_marker_empty(rt, "cleanup")
    _assert_interface_empty(rt, "cleanup")


def _run_scenario(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))
    container = rt.container_name("r1")

    step("Start empty LLDP manually and verify reads do not create the revive marker")
    process_start(rt, "r1", MODULE, ready_timeout=30)
    _wait_pids(container, running=True, what="manually started LLDP")
    summary = cmd(rt, "r1", "show lldp")
    if "Admin          : down" not in summary:
        raise AssertionError(f"empty LLDP did not expose default admin-down state:\n{summary}")
    _assert_marker_empty(rt, "read-only default state")

    step("Undo commands on implicit interface defaults do not create configuration")
    run_cmds(
        rt,
        "r1",
        commands=[
            "config",
            f"if {INTERFACE}",
            "no lldp admin-status",
            "no lldp port-description",
            "exit",
            "end",
        ],
    )
    _wait_pids(container, running=True, what="LLDP to remain available after default-only interface undo")
    _assert_marker_empty(rt, "default-only interface undo")
    _assert_interface_empty(rt, "default-only interface undo")

    step("Verify no lldp keeps an empty marker empty and stops the process")
    run_cmds(rt, "r1", commands=["config", "no lldp", "end"])
    _wait_pids(container, running=False, what="LLDP exit after no lldp")
    _assert_marker_empty(rt, "no lldp from defaults")

    step("A non-default timer creates a real revive marker")
    run_cmds(rt, "r1", commands=["config", f"lldp timer {NONDEFAULT_TIMER}", "end"])
    _wait_pids(container, running=True, what="LLDP auto-start for timer config")
    _assert_marker_timer(rt, "non-default timer")
    current = cmd(rt, "r1", "show current-configuration")
    expected = f"lldp timer {NONDEFAULT_TIMER}"
    if expected not in current:
        raise AssertionError(f"LLDP timer missing from current configuration:\n{current}")

    step("Saved non-default LLDP config revives the module after a cold reboot")
    pre_reboot_pid = _module_pids(container)
    _save_startup(rt, CONFIGURED_SNAPSHOT, "db")
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    post_reboot_pid = _wait_pids(container, running=True, what="LLDP revive from configured marker")
    if pre_reboot_pid and post_reboot_pid == pre_reboot_pid:
        raise AssertionError(f"LLDP pid did not change across reboot: {post_reboot_pid}")
    _assert_marker_timer(rt, "configured reboot")
    current = cmd(rt, "r1", "show current-configuration")
    if expected not in current:
        raise AssertionError(f"LLDP timer was not restored after reboot:\n{current}")

    step("Undoing the last LLDP setting deletes the marker")
    run_cmds(rt, "r1", commands=["config", "no lldp timer"])
    _assert_marker_empty(rt, "last setting undone")
    run_cmds(rt, "r1", commands=["no lldp", "end"])
    _wait_pids(container, running=False, what="LLDP exit after final cleanup")
    _assert_marker_empty(rt, "final cleanup")

    step("Saved empty LLDP state remains on-demand after a startup/db cold reboot")
    _save_startup(rt, EMPTY_SNAPSHOT, "db")
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=False, what="LLDP to remain on-demand with empty marker")
    _assert_marker_empty(rt, "empty reboot")
    modules = cmd(rt, "r1", "show dev modules")
    lldp_rows = [line for line in modules.splitlines() if MODULE in line.lower()]
    if not lldp_rows or not any("ON-DEMAND" in line.upper() and "down" in line.lower() for line in lldp_rows):
        raise AssertionError(f"LLDP is not ON-DEMAND/down after empty reboot:\n{modules}")

    step("A legacy interface-only default row revives through the secondary table and is pruned")
    _inject_legacy_default_interface_only(rt, container)
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=True, what="LLDP secondary-table legacy normalization")
    _assert_marker_empty(rt, "legacy interface-only marker normalization")
    _assert_interface_empty(rt, "legacy interface-only default normalization")
    current = cmd(rt, "r1", "show current-configuration")
    if "lldp enable" in current:
        raise AssertionError(f"legacy implicit-default interface leaked into BDR:\n{current}")

    run_cmds(rt, "r1", commands=["config", "no lldp", "end"])
    _wait_pids(container, running=False, what="LLDP exit after interface-only normalization")

    step("Hidden protocol fields and a non-canonical singleton are normalized after one DB boot")
    _inject_legacy_protocol_rows(rt, container)
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=True, what="LLDP dirty protocol singleton normalization")
    _assert_marker_empty(rt, "hidden and non-canonical protocol normalization")
    _assert_interface_empty(rt, "dirty protocol normalization")
    current = cmd(rt, "r1", "show current-configuration")
    if re.search(r"(?m)^\s*(?:no\s+)?lldp(?:\s|$)", current):
        raise AssertionError(f"hidden/rogue LLDP protocol state leaked into BDR:\n{current}")

    run_cmds(rt, "r1", commands=["config", "no lldp", "end"])
    _wait_pids(container, running=False, what="LLDP exit after protocol normalization")
    _save_startup(rt, EMPTY_SNAPSHOT, "db")
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=False, what="normalized legacy rows to stay down")
    _assert_marker_empty(rt, "second normalized legacy reboot")
    _assert_interface_empty(rt, "second normalized legacy reboot")

    step("An interface-only negative override creates a real marker")
    process_start(rt, "r1", MODULE, ready_timeout=30)
    _wait_pids(container, running=True, what="LLDP manual start for negative override")
    run_cmds(
        rt,
        "r1",
        commands=["config", f"if {INTERFACE}", "no lldp enable", "exit", "end"],
    )
    _wait_pids(container, running=True, what="LLDP auto-start for interface-only config")
    _assert_marker_present(rt, "interface-only negative override")
    _assert_negative_interface(rt, "interface-only negative override")

    step("no lldp keeps a configured child override and its BDR owner online")
    run_cmds(rt, "r1", commands=["config", "no lldp", "end"])
    _wait_pids(container, running=True, what="LLDP owner retained for interface override")
    _assert_marker_present(rt, "negative override after no lldp")
    _assert_negative_interface(rt, "negative override after no lldp")

    step("Interface-only LLDP config auto-starts and replays after a startup/cfg cold reboot")
    _save_startup(rt, INTERFACE_SNAPSHOT, "cfg")
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=True, what="LLDP startup/cfg interface-only replay")
    _assert_marker_present(rt, "interface-only cfg reboot")
    _assert_negative_interface(rt, "interface-only cfg reboot")
    _assert_cfg_replay_clean(rt, "interface-only cfg reboot")

    step("lldp enable removes the negative override but keeps the replay owner available")
    run_cmds(rt, "r1", commands=["config", f"if {INTERFACE}", "lldp enable", "exit", "end"])
    _wait_pids(container, running=True, what="LLDP owner retained after last interface override is removed")
    _assert_marker_empty(rt, "negative override removed")
    _assert_interface_empty(rt, "negative override removed")

    step("Explicit no lldp exits the empty owner and its DB snapshot cannot revive LLDP")
    run_cmds(rt, "r1", commands=["config", "no lldp", "end"])
    _wait_pids(container, running=False, what="LLDP exit after explicit no lldp")
    _save_startup(rt, EMPTY_SNAPSHOT, "db")
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=False, what="empty LLDP DB snapshot to remain on-demand")
    _assert_marker_empty(rt, "empty DB snapshot after interface override removal")
    _assert_interface_empty(rt, "empty DB snapshot after interface override removal")

    step("A description-only override renders a compatible positive enable")
    run_cmds(
        rt,
        "r1",
        commands=[
            "config",
            f"if {INTERFACE}",
            f"lldp port-description {PORT_DESCRIPTION}",
            "exit",
            "end",
        ],
    )
    _wait_pids(container, running=True, what="LLDP description-only interface override")
    _assert_description_only_interface(rt, "description-only interface override")

    step("Description-only LLDP config replays from a startup/cfg cold reboot")
    _save_startup(rt, DESCRIPTION_ONLY_SNAPSHOT, "cfg")
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=True, what="LLDP startup/cfg description-only replay")
    _assert_description_only_interface(rt, "description-only cfg reboot")
    _assert_cfg_replay_clean(rt, "description-only cfg reboot")

    step("Undoing the description clears its marker while keeping the replay owner online")
    run_cmds(
        rt,
        "r1",
        commands=["config", f"if {INTERFACE}", "no lldp port-description", "exit", "end"],
    )
    _wait_pids(container, running=True, what="LLDP owner retained after description-only undo")
    _assert_marker_empty(rt, "description-only override removed")
    _assert_interface_empty(rt, "description-only override removed")
    run_cmds(rt, "r1", commands=["config", "no lldp", "end"])
    _wait_pids(container, running=False, what="LLDP explicit exit after description-only cleanup")

    step("Build the legacy positive interface command sequence")
    run_cmds(
        rt,
        "r1",
        commands=[
            "config",
            f"if {INTERFACE}",
            "lldp enable",
            f"lldp admin-status {POSITIVE_ADMIN_STATUS}",
            f"lldp port-description {PORT_DESCRIPTION}",
            "exit",
            "end",
        ],
    )
    _wait_pids(container, running=True, what="LLDP legacy positive interface sequence")
    _assert_positive_interface(rt, "legacy positive interface sequence")

    step("Legacy enable/admin/description LLDP config replays from a startup/cfg cold reboot")
    _save_startup(rt, POSITIVE_INTERFACE_SNAPSHOT, "cfg")
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=True, what="LLDP startup/cfg positive interface replay")
    _assert_positive_interface(rt, "legacy positive interface cfg reboot")
    _assert_cfg_replay_clean(rt, "legacy positive interface cfg reboot")

    step("Undoing the positive overrides clears the marker without interrupting replay")
    run_cmds(
        rt,
        "r1",
        commands=[
            "config",
            f"if {INTERFACE}",
            "no lldp port-description",
            "no lldp admin-status",
            "exit",
            "end",
        ],
    )
    _wait_pids(container, running=True, what="LLDP owner retained after positive overrides are removed")
    _assert_marker_empty(rt, "positive interface overrides removed")
    _assert_interface_empty(rt, "positive interface overrides removed")
    run_cmds(rt, "r1", commands=["config", "no lldp", "end"])
    _wait_pids(container, running=False, what="LLDP explicit exit after positive replay cleanup")

    step("An admin-status-only legacy sequence also survives startup/cfg replay")
    run_cmds(
        rt,
        "r1",
        commands=[
            "config",
            f"if {INTERFACE}",
            "lldp enable",
            f"lldp admin-status {ADMIN_ONLY_STATUS}",
            "exit",
            "end",
        ],
    )
    _wait_pids(container, running=True, what="LLDP admin-only legacy interface sequence")
    _assert_admin_only_interface(rt, "admin-only legacy interface sequence")
    _save_startup(rt, ADMIN_ONLY_SNAPSHOT, "cfg")
    cold_reboot_device(rt, "r1", timeout=120, save_config=False)
    _wait_pids(container, running=True, what="LLDP startup/cfg admin-only interface replay")
    _assert_admin_only_interface(rt, "admin-only legacy cfg reboot")
    _assert_cfg_replay_clean(rt, "admin-only legacy cfg reboot")

    run_cmds(
        rt,
        "r1",
        commands=["config", f"if {INTERFACE}", "no lldp admin-status", "exit", "end"],
    )
    _wait_pids(container, running=True, what="LLDP owner retained after admin-only override removal")
    _assert_marker_empty(rt, "admin-only override removed")
    _assert_interface_empty(rt, "admin-only override removed")
    run_cmds(rt, "r1", commands=["config", "no lldp", "end"])
    _wait_pids(container, running=False, what="LLDP explicit exit after admin-only replay cleanup")

    print("LLDP revive-marker lifecycle check passed.")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))
    container = rt.container_name("r1")
    _reset_marker_state(rt, container)
    try:
        _run_scenario(rt, top)
    finally:
        _reset_marker_state(rt, container)
