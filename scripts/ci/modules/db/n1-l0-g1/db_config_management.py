#!/usr/bin/env python3
"""DB running/startup configuration management regression.

Coverage:
1. Factory display state: no selected startup configuration.
2. Error paths: missing startup snapshot and invalid configuration name.
3. DB show commands reflect the temporary running.db and dev_config data.
4. `save configuration` without a selected startup saves `startup` but does not select it.
5. Named `save configuration <name>` + `startup configuration <name>` survives reboot.
6. Unsaved running changes are discarded on reboot and startup snapshot is restored.
7. `save configuration` without a name uses the currently selected startup snapshot.
8. Switching startup to a second named snapshot changes the next reboot restore source.
"""

from __future__ import annotations

import re
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
        "/opt/netnexus/data/configs/startup.db* "
        f"/opt/netnexus/data/configs/{ALPHA_CFG}.db* "
        f"/opt/netnexus/data/configs/{BETA_CFG}.db*",
        check=False,
    )


def _assert_file_exists(rt: TopologyRuntime, rel_path: str) -> None:
    out = _container_sh(rt, f"test -s /opt/netnexus/data/{rel_path} && echo present || echo missing")
    _assert(f"file exists {rel_path}", out, contains=["present"], not_contains=["missing"])


def _assert_startup_pointer(rt: TopologyRuntime, expected: str) -> None:
    out = _container_sh(rt, "cat /opt/netnexus/data/startup.cfg 2>/dev/null || true")
    _assert("startup.cfg pointer", out, contains=[expected])


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
    _set_sysname(rt, BASE_SYSNAME)
    _remove_saved_configs(rt)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, (DEV,))

    try:
        _remove_saved_configs(rt)
        _set_sysname(rt, BASE_SYSNAME)

        step("Phase A: default display state and error paths")
        out = _show(rt, "show startup configuration")
        _assert("default startup display", out, contains=["Startup configuration: <none> (factory default)"])

        out = _show(rt, "startup configuration missing_cfg")
        _assert("missing startup config rejected", out, contains=["Error: Configuration not found"])

        out = _show(rt, "save configuration bad.name")
        _assert("invalid config name rejected", out, contains=["Error: Invalid configuration name"])

        step("Phase B: show DB running database tables and dev_config row")
        out = _show(rt, "show db table-list")
        _assert("show db table-list", out, contains=["Tables in running.db:", "dev_config"])
        out = _show(rt, "show db table-field dev_config")
        _assert("show db table-field dev_config", out, contains=["Fields of table 'dev_config':", "sysname"])
        out = _show(rt, "show db table-data dev_config")
        _assert("show db table-data dev_config", out, contains=["Table: dev_config", BASE_SYSNAME])

        step("Phase C: save configuration without startup pointer does not select it")
        _set_sysname(rt, DEFAULT_SYSNAME)
        out = _show(rt, "save configuration")
        _assert("default save name", out, contains=["Configuration saved as 'startup'."])
        _assert_file_exists(rt, "configs/startup.db")
        out = _show(rt, "show startup configuration")
        _assert("save does not set startup", out, contains=["Startup configuration: <none> (factory default)"])

        reboot_device(rt, DEV, timeout=120)
        _wait_sysname_absent(rt, DEFAULT_SYSNAME, label="unspecified startup boots without default saved snapshot")
        out = _show(rt, "show startup configuration")
        _assert("startup still none after reboot", out, contains=["Startup configuration: <none> (factory default)"])

        step("Phase D: named save + startup selection restores after reboot")
        _set_sysname(rt, ALPHA_SYSNAME)
        out = _show(rt, f"save configuration {ALPHA_CFG}")
        _assert("save alpha", out, contains=[f"Configuration saved as '{ALPHA_CFG}'."])
        _assert_file_exists(rt, f"configs/{ALPHA_CFG}.db")

        out = _show(rt, f"startup configuration {ALPHA_CFG}")
        _assert("set alpha startup", out, contains=[f"Startup configuration set to '{ALPHA_CFG}'."])
        _assert_startup_pointer(rt, ALPHA_CFG)

        out = _show(rt, "show startup configuration")
        _assert("show alpha startup", out, contains=[f"Startup configuration: {ALPHA_CFG}"])

        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(rt, ALPHA_SYSNAME, label="alpha startup restored")
        out = _show(rt, "show db table-data dev_config")
        _assert("dev_config restored from alpha snapshot", out, contains=["Table: dev_config", ALPHA_SYSNAME])

        step("Phase E: unsaved running change is discarded on reboot")
        _set_sysname(rt, SCRATCH_SYSNAME)
        _wait_sysname(rt, SCRATCH_SYSNAME, label="scratch sysname applied before reboot")
        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(
            rt,
            ALPHA_SYSNAME,
            not_names=[SCRATCH_SYSNAME],
            label="alpha startup restored over unsaved scratch change",
        )

        step("Phase F: save configuration uses current startup name when omitted")
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

        step("Phase G: switch startup to second named snapshot")
        _set_sysname(rt, BETA_SYSNAME)
        out = _show(rt, f"save configuration {BETA_CFG}")
        _assert("save beta", out, contains=[f"Configuration saved as '{BETA_CFG}'."])
        _assert_file_exists(rt, f"configs/{BETA_CFG}.db")
        out = _show(rt, f"startup configuration {BETA_CFG}")
        _assert("set beta startup", out, contains=[f"Startup configuration set to '{BETA_CFG}'."])
        _assert_startup_pointer(rt, BETA_CFG)
        out = _show(rt, "show startup configuration")
        _assert("show beta startup", out, contains=[f"Startup configuration: {BETA_CFG}"])

        reboot_device(rt, DEV, timeout=120)
        _wait_sysname(
            rt,
            BETA_SYSNAME,
            not_names=[ALPHA2_SYSNAME, SCRATCH_SYSNAME],
            label="beta startup restored after reboot",
        )

        print("DB configuration management check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
