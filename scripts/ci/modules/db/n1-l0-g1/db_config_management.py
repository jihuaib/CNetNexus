#!/usr/bin/env python3
"""DB running/startup configuration management regression.

Coverage:
1. Factory display state: no selected startup configuration.
2. Error paths: missing startup snapshot and invalid configuration name.
3. DB show commands reflect the temporary running.db and dev_config data.
4. A configured but disconnected on-demand module makes snapshot capture fail closed.
5. Empty manually-started BGP/ISIS modules emit no BDR text and `no` returns them to on-demand.
6. `save configuration` without a selected startup saves `startup` but does not select it.
7. Named `save configuration <name>` + `startup configuration <name> db` survives reboot.
8. Unsaved running changes are discarded on reboot and startup snapshot is restored.
9. `save configuration` without a name uses the currently selected startup snapshot.
10. Switching startup to a second named snapshot in cfg mode changes the next reboot restore source.
11. Minor-version metadata changes still boot from `.db`; removing `.cfg` must not matter.
12. Major-version metadata changes fall back to `.cfg`; removing `.db` must not matter.
"""

from __future__ import annotations

import re
import shlex
import subprocess

from module_api import (  # noqa: E402
    check_output,
    cmd,
    reboot_device,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402


DEV = "r1"
BASE_SYSNAME = "r1"
DEFAULT_SYSNAME = "dbdefault"
ALPHA_SYSNAME = "dbalpha"
ALPHA2_SYSNAME = "dbalpha2"
SCRATCH_SYSNAME = "dbscratch"
BETA_SYSNAME = "dbbeta"

ALPHA_CFG = "ci_alpha"
BETA_CFG = "ci-beta_1"
INCOMPLETE_CFG = "ci_incomplete_capture"
INCOMPLETE_BGP_AS = 65099
EMPTY_ISIS_TAG = 424242


def _show(rt: TopologyRuntime, command: str) -> str:
    return run_cmds(rt=rt, device=DEV, strict=False, timeout=25, commands=["end", command])[-1]


def _assert(label: str, text: str, *, contains=None, not_contains=None) -> None:
    violations = check_output(text, contains=contains or [], not_contains=not_contains or [])
    if violations:
        raise AssertionError(f"{label}: {'; '.join(violations)}\nOutput:\n{text}")


def _container_sh(rt: TopologyRuntime, command: str, *, check: bool = True) -> str:
    proc = subprocess.run(
        ["docker", "exec", rt.container_name(DEV), "/bin/sh", "-lc", command],
        text=True,
        capture_output=True,
        check=False,
    )
    text = (proc.stdout or "") + (proc.stderr or "")
    if check and proc.returncode != 0:
        raise RuntimeError(f"container command failed ({proc.returncode}): {command}\n{text}")
    return text


def _remove_saved_configs(rt: TopologyRuntime) -> None:
    _container_sh(
        rt,
        "rm -f "
        "/opt/netnexus/data/startup.cfg /opt/netnexus/data/startup.cfg.tmp "
        "/opt/netnexus/data/configs/startup.db* /opt/netnexus/data/configs/startup.cfg* "
        "/opt/netnexus/data/configs/startup.meta* "
        f"/opt/netnexus/data/configs/{ALPHA_CFG}.db* /opt/netnexus/data/configs/{ALPHA_CFG}.cfg* "
        f"/opt/netnexus/data/configs/{ALPHA_CFG}.meta* "
        f"/opt/netnexus/data/configs/{BETA_CFG}.db* /opt/netnexus/data/configs/{BETA_CFG}.cfg* "
        f"/opt/netnexus/data/configs/{BETA_CFG}.meta* "
        f"/opt/netnexus/data/configs/{INCOMPLETE_CFG}.db* "
        f"/opt/netnexus/data/configs/{INCOMPLETE_CFG}.cfg* "
        f"/opt/netnexus/data/configs/{INCOMPLETE_CFG}.meta*",
        check=False,
    )


def _assert_file_exists(rt: TopologyRuntime, rel_path: str) -> None:
    out = _container_sh(rt, f"test -s /opt/netnexus/data/{rel_path} && echo present || echo missing")
    _assert(f"file exists {rel_path}", out, contains=["present"], not_contains=["missing"])


def _assert_file_missing(rt: TopologyRuntime, rel_path: str) -> None:
    out = _container_sh(rt, f"test ! -e /opt/netnexus/data/{rel_path} && echo absent || echo present")
    _assert(f"file missing {rel_path}", out, contains=["absent"], not_contains=["present"])


def _assert_startup_pointer(rt: TopologyRuntime, expected: str, mode: str) -> None:
    out = _container_sh(rt, "cat /opt/netnexus/data/startup.cfg 2>/dev/null || true")
    _assert("startup.cfg pointer", out, contains=[f"{mode} {expected}"])


def _cfg_commands(text: str) -> list[str]:
    commands: list[str] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("!"):
            continue
        commands.append(line)
    return commands


def _read_saved_cfg_commands(rt: TopologyRuntime, name: str) -> list[str]:
    text = _container_sh(rt, f"cat /opt/netnexus/data/configs/{name}.cfg")
    commands = _cfg_commands(text)
    if not commands:
        raise AssertionError(f"saved cfg {name!r} has no replayable commands\n{text}")
    return commands


def _assert_current_contains_cfg_commands(rt: TopologyRuntime, expected: list[str], *, label: str) -> None:
    wait_check(
        rt,
        device=DEV,
        command="show current-configuration",
        timeout=30,
        interval=2,
        regex=[rf"(?m)^\s*{re.escape(line)}\s*$" for line in expected],
        label=label,
    )


def _assert_replay_failures_empty(rt: TopologyRuntime, *, label: str) -> None:
    out = _show(rt, "show configuration replay-failures")
    _assert(label, out, contains=["Configuration replay failures:", "<none>"])


def _normalize_running_config(text: str) -> str:
    lines: list[str] = []
    for raw in text.replace("\r", "").splitlines():
        stripped = raw.strip()
        if not stripped or stripped == "show current-configuration":
            continue
        if re.match(r"^<[^>]+>(?:\s.*)?$", stripped):
            continue
        lines.append(raw.rstrip())
    return "\n".join(lines)


def _parse_major_version(version: str | None) -> int | None:
    match = re.match(r"\s*(\d+)", version or "")
    if not match:
        return None
    major = int(match.group(1))
    if major > 255:
        return None
    return major


def _read_current_version(rt: TopologyRuntime) -> str | None:
    out = _container_sh(rt, "if [ -f /opt/netnexus/VERSION ]; then head -n 1 /opt/netnexus/VERSION; fi")
    text = out.strip()
    return text or None


def _write_current_version(rt: TopologyRuntime, version: str | None) -> None:
    if version is None:
        _container_sh(rt, "rm -f /opt/netnexus/VERSION", check=False)
        return
    _container_sh(rt, f"printf '%s\n' {shlex.quote(version)} > /opt/netnexus/VERSION")


def _ensure_parseable_current_version(rt: TopologyRuntime) -> tuple[str, int]:
    current = _read_current_version(rt)
    major = _parse_major_version(current)
    if current is not None and major is not None:
        return current, major

    current = "1.0.0"
    _write_current_version(rt, current)
    return current, 1


def _minor_mismatch_version(current: str, major: int) -> str:
    candidate = f"{major}.250.0"
    if current.strip() == candidate:
        candidate = f"{major}.251.0"
    return candidate


def _major_mismatch_version(major: int) -> str:
    if major < 255:
        return f"{major + 1}.0.0"
    return f"{major - 1}.0.0"


def _write_saved_version(rt: TopologyRuntime, name: str, version: str) -> None:
    path = f"/opt/netnexus/data/configs/{name}.meta"
    _container_sh(rt, f"printf 'version=%s\n' {shlex.quote(version)} > {shlex.quote(path)}")


def _remove_config_file(rt: TopologyRuntime, name: str, suffix: str) -> None:
    if suffix not in ("db", "cfg"):
        raise ValueError(f"unsupported config suffix {suffix!r}")
    path = f"/opt/netnexus/data/configs/{name}.{suffix}"
    _container_sh(
        rt,
        "rm -f "
        f"{shlex.quote(path)} "
        f"{shlex.quote(path + '-wal')} "
        f"{shlex.quote(path + '-shm')} "
        f"{shlex.quote(path + '-journal')}",
        check=False,
    )


def _set_sysname(rt: TopologyRuntime, name: str) -> None:
    run_cmds(rt=rt, device=DEV, strict=True, commands=["config", f"sysname {name}", "end"])


def _sysname_regex(name: str) -> str:
    return rf"(?m)^\s*sysname\s+{re.escape(name)}\s*$"


def _wait_sysname(rt: TopologyRuntime, name: str, *, not_names: list[str] | None = None, label: str) -> None:
    wait_check(
        rt,
        device=DEV,
        command="show current-configuration",
        timeout=30,
        interval=2,
        regex=[_sysname_regex(name)],
        not_regex=[_sysname_regex(n) for n in (not_names or [])],
        label=label,
    )


def _wait_sysname_absent(rt: TopologyRuntime, name: str, *, label: str) -> None:
    wait_check(
        rt,
        device=DEV,
        command="show current-configuration",
        timeout=30,
        interval=2,
        not_regex=[_sysname_regex(name)],
        label=label,
    )


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup DB config snapshots and restore sysname")
    _show(rt, "process start bgp")
    run_cmds(
        rt=rt,
        device=DEV,
        strict=False,
        commands=["config", "no bgp", "end"],
    )
    _show(rt, "process stop bgp")
    _show(rt, "process stop isis")
    _set_sysname(rt, BASE_SYSNAME)
    _remove_saved_configs(rt)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, (DEV,))
    original_version: str | None = None
    original_version_loaded = False

    try:
        original_version = _read_current_version(rt)
        original_version_loaded = True
        current_version, current_major = _ensure_parseable_current_version(rt)
        _remove_saved_configs(rt)
        _set_sysname(rt, BASE_SYSNAME)

        step("Phase A: default display state and error paths")
        out = _show(rt, "show startup configuration")
        _assert("default startup display", out, contains=["Startup configuration: <none> (factory default)"])

        out = _show(rt, "startup configuration missing_cfg db")
        _assert("missing db startup config rejected", out, contains=["Error: Configuration not found"])

        out = _show(rt, "startup configuration missing_cfg cfg")
        _assert("missing cfg startup config rejected", out, contains=["Error: Configuration not found"])

        out = _show(rt, "save configuration bad.name")
        _assert("invalid config name rejected", out, contains=["Error: Invalid configuration name"])

        step("Phase B: show DB running database tables and dev_config row")
        out = _show(rt, "show db table-list")
        _assert("show db table-list", out, contains=["Tables in running.db:", "dev_config"])
        out = _show(rt, "show db table-field dev_config")
        _assert("show db table-field dev_config", out, contains=["Fields of table 'dev_config':", "sysname"])
        out = _show(rt, "show db table-data dev_config")
        _assert("show db table-data dev_config", out, contains=["Table: dev_config", BASE_SYSNAME])

        step("Phase C: configured but disconnected module makes capture fail closed")
        run_cmds(
            rt=rt,
            device=DEV,
            strict=True,
            commands=["config", f"bgp {INCOMPLETE_BGP_AS}", "end"],
        )
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=2,
            regex=[r"(?m)^\s*6\s+bgp\s+READY\s+"],
            label="BGP ready before incomplete capture test",
        )
        _show(rt, "process stop bgp")
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=2,
            regex=[r"(?m)^\s*6\s+bgp\s+ON-DEMAND\s+\S+\s+down\s+"],
            label="configured BGP deliberately disconnected",
        )
        out = _show(rt, "show current-configuration")
        _assert(
            "incomplete running configuration rejected",
            out,
            contains=["Error: Current configuration capture is incomplete", "no partial output was shown"],
        )
        out = _show(rt, f"save configuration {INCOMPLETE_CFG}")
        _assert(
            "incomplete capture rejected",
            out,
            contains=[
                "Error: Configuration capture incomplete:",
                "required module bgp",
                "is not connected",
            ],
        )
        _assert_file_missing(rt, f"configs/{INCOMPLETE_CFG}.db")
        _assert_file_missing(rt, f"configs/{INCOMPLETE_CFG}.cfg")

        _show(rt, "process start bgp")
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=2,
            regex=[r"(?m)^\s*6\s+bgp\s+READY\s+"],
            label="BGP restarted for cleanup",
        )
        run_cmds(rt=rt, device=DEV, strict=True, commands=["config", "no bgp", "end"])
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=2,
            regex=[r"(?m)^\s*6\s+bgp\s+ON-DEMAND\s+\S+\s+down\s+"],
            label="configured BGP exits after its configuration is removed",
        )

        step("Phase C2: empty BGP BDR is stable and no bgp returns it to on-demand")
        bdr_without_bgp = _normalize_running_config(_show(rt, "show current-configuration"))
        _show(rt, "process start bgp")
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=2,
            regex=[r"(?m)^\s*6\s+bgp\s+READY\s+"],
            label="empty BGP manually started",
        )
        bdr_with_empty_bgp = _normalize_running_config(_show(rt, "show current-configuration"))
        if bdr_with_empty_bgp != bdr_without_bgp:
            raise AssertionError(
                "empty BGP changed show current-configuration\n"
                f"without BGP:\n{bdr_without_bgp}\n"
                f"with empty BGP:\n{bdr_with_empty_bgp}"
            )

        run_cmds(rt=rt, device=DEV, strict=True, commands=["config", "no bgp", "end"])
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=2,
            regex=[r"(?m)^\s*6\s+bgp\s+ON-DEMAND\s+\S+\s+down\s+"],
            label="no bgp exits an empty manually-started BGP process",
        )
        bdr_after_empty_bgp = _normalize_running_config(_show(rt, "show current-configuration"))
        if bdr_after_empty_bgp != bdr_without_bgp:
            raise AssertionError(
                "show current-configuration changed after empty BGP exited\n"
                f"before:\n{bdr_without_bgp}\n"
                f"after:\n{bdr_after_empty_bgp}"
            )

        step("Phase C3: empty ISIS BDR is stable and no isis returns it to on-demand")
        bdr_without_isis = bdr_after_empty_bgp
        _show(rt, "process start isis")
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=2,
            regex=[r"(?m)^\s*9\s+isis\s+READY\s+"],
            label="empty ISIS manually started",
        )
        bdr_with_empty_isis = _normalize_running_config(_show(rt, "show current-configuration"))
        if bdr_with_empty_isis != bdr_without_isis:
            raise AssertionError(
                "empty ISIS changed show current-configuration\n"
                f"without ISIS:\n{bdr_without_isis}\n"
                f"with empty ISIS:\n{bdr_with_empty_isis}"
            )

        run_cmds(rt=rt, device=DEV, strict=True, commands=["config", f"no isis {EMPTY_ISIS_TAG}", "end"])
        wait_check(
            rt,
            device=DEV,
            command="show dev modules",
            timeout=30,
            interval=2,
            regex=[r"(?m)^\s*9\s+isis\s+ON-DEMAND\s+\S+\s+down\s+"],
            label="no isis exits an empty manually-started ISIS process",
        )
        bdr_after_empty_isis = _normalize_running_config(_show(rt, "show current-configuration"))
        if bdr_after_empty_isis != bdr_without_isis:
            raise AssertionError(
                "show current-configuration changed after empty ISIS exited\n"
                f"before:\n{bdr_without_isis}\n"
                f"after:\n{bdr_after_empty_isis}"
            )

        step("Phase D: save configuration without startup pointer does not select it")
        _set_sysname(rt, DEFAULT_SYSNAME)
        out = _show(rt, "save configuration")
        _assert("default save name", out, contains=["Configuration saved as 'startup'."])
        _assert_file_exists(rt, "configs/startup.db")
        _assert_file_exists(rt, "configs/startup.cfg")
        out = _show(rt, "show startup configuration")
        _assert("save does not set startup", out, contains=["Startup configuration: <none> (factory default)"])

        reboot_device(rt, DEV, timeout=120)
        _wait_sysname_absent(rt, DEFAULT_SYSNAME, label="unspecified startup boots without default saved snapshot")
        out = _show(rt, "show startup configuration")
        _assert("startup still none after reboot", out, contains=["Startup configuration: <none> (factory default)"])

        step("Phase E: named save + startup db selection restores after reboot")
        _set_sysname(rt, ALPHA_SYSNAME)
        out = _show(rt, f"save configuration {ALPHA_CFG}")
        _assert("save alpha", out, contains=[f"Configuration saved as '{ALPHA_CFG}'."])
        _assert_file_exists(rt, f"configs/{ALPHA_CFG}.db")
        _assert_file_exists(rt, f"configs/{ALPHA_CFG}.cfg")

        out = _show(rt, f"startup configuration {ALPHA_CFG} db")
        _assert("set alpha startup", out, contains=[f"Startup configuration set to '{ALPHA_CFG}' (db)."])
        _assert_startup_pointer(rt, ALPHA_CFG, "db")

        out = _show(rt, "show startup configuration")
        _assert("show alpha startup", out, contains=[f"Startup configuration: {ALPHA_CFG} (db)"])

        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(rt, ALPHA_SYSNAME, label="alpha startup restored")
        out = _show(rt, "show db table-data dev_config")
        _assert("dev_config restored from alpha snapshot", out, contains=["Table: dev_config", ALPHA_SYSNAME])

        step("Phase F: minor-version startup/db does not require cfg text")
        _write_saved_version(rt, ALPHA_CFG, _minor_mismatch_version(current_version, current_major))
        _remove_config_file(rt, ALPHA_CFG, "cfg")
        _assert_file_missing(rt, f"configs/{ALPHA_CFG}.cfg")
        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(rt, ALPHA_SYSNAME, label="alpha restored from db with only minor version metadata drift")
        out = _show(rt, "show db table-data dev_config")
        _assert("minor-version restore used alpha db snapshot", out, contains=["Table: dev_config", ALPHA_SYSNAME])
        _assert_replay_failures_empty(rt, label="minor-version db startup did not attempt missing cfg replay")

        step("Phase G: unsaved running change is discarded on reboot")
        _set_sysname(rt, SCRATCH_SYSNAME)
        _wait_sysname(rt, SCRATCH_SYSNAME, label="scratch sysname applied before reboot")
        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(
            rt,
            ALPHA_SYSNAME,
            not_names=[SCRATCH_SYSNAME],
            label="alpha startup restored over unsaved scratch change",
        )

        step("Phase H: save configuration uses current startup name when omitted")
        _set_sysname(rt, ALPHA2_SYSNAME)
        out = _show(rt, "save configuration")
        _assert("unnamed save uses current startup", out, contains=[f"Configuration saved as '{ALPHA_CFG}'."])
        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(
            rt,
            ALPHA2_SYSNAME,
            not_names=[ALPHA_SYSNAME],
            label="alpha snapshot overwritten by unnamed save",
        )

        step("Phase I: switch startup to second named snapshot in cfg mode")
        _set_sysname(rt, BETA_SYSNAME)
        out = _show(rt, f"save configuration {BETA_CFG}")
        _assert("save beta", out, contains=[f"Configuration saved as '{BETA_CFG}'."])
        _assert_file_exists(rt, f"configs/{BETA_CFG}.db")
        _assert_file_exists(rt, f"configs/{BETA_CFG}.cfg")
        beta_saved_cfg = _read_saved_cfg_commands(rt, BETA_CFG)
        _assert("saved beta cfg content", "\n".join(beta_saved_cfg), contains=[f"sysname {BETA_SYSNAME}"])
        out = _show(rt, f"startup configuration {BETA_CFG} cfg")
        _assert("set beta startup", out, contains=[f"Startup configuration set to '{BETA_CFG}' (cfg)."])
        _assert_startup_pointer(rt, BETA_CFG, "cfg")
        out = _show(rt, "show startup configuration")
        _assert("show beta startup", out, contains=[f"Startup configuration: {BETA_CFG} (cfg)"])

        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(
            rt,
            BETA_SYSNAME,
            not_names=[ALPHA2_SYSNAME, SCRATCH_SYSNAME],
            label="beta startup restored after reboot",
        )
        _assert_current_contains_cfg_commands(
            rt,
            beta_saved_cfg,
            label="all saved beta cfg commands restored after cfg startup",
        )
        _assert_replay_failures_empty(rt, label="cfg startup replay failure list is empty")

        step("Phase J: major-version startup/db falls back to cfg when db snapshot is missing")
        out = _show(rt, f"startup configuration {BETA_CFG} db")
        _assert("set beta startup back to db", out, contains=[f"Startup configuration set to '{BETA_CFG}' (db)."])
        _assert_startup_pointer(rt, BETA_CFG, "db")
        _write_saved_version(rt, BETA_CFG, _major_mismatch_version(current_major))
        _remove_config_file(rt, BETA_CFG, "db")
        _assert_file_missing(rt, f"configs/{BETA_CFG}.db")
        _set_sysname(rt, SCRATCH_SYSNAME)
        _wait_sysname(rt, SCRATCH_SYSNAME, label="scratch sysname applied before major fallback reboot")
        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(
            rt,
            BETA_SYSNAME,
            not_names=[SCRATCH_SYSNAME, ALPHA2_SYSNAME],
            label="beta restored from cfg after major version metadata drift",
        )
        _assert_current_contains_cfg_commands(
            rt,
            beta_saved_cfg,
            label="all saved beta cfg commands restored after major-version db fallback",
        )
        _assert_replay_failures_empty(rt, label="major-version fallback cfg replay failure list is empty")

        print("DB configuration management check passed.")
    finally:
        if original_version_loaded:
            _write_current_version(rt, original_version)
        if not should_skip_cleanup():
            _cleanup(rt)
