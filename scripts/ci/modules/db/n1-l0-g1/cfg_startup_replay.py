#!/usr/bin/env python3
"""CFG startup replay regression for explicit exits and all-or-nothing preflight."""

from __future__ import annotations

import hashlib
import re
import subprocess

from module_api import (  # noqa: E402
    check_output,
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
EXPLICIT_SNAPSHOT = "ci_cfg_explicit_exit"
INVALID_SNAPSHOT = "ci_cfg_invalid_indent"
INVALID_EXIT_SNAPSHOT = "ci_cfg_invalid_exit"
INVALID_GLOBAL_SNAPSHOT = "ci_cfg_invalid_global"
INVALID_CASE_SNAPSHOT = "ci_cfg_invalid_exit_case"
EXPLICIT_SYSNAME = "cfg-explicit-exit"
INVALID_SYSNAME = "cfg-must-not-apply"
INVALID_EXIT_SYSNAME = "cfg-exit-must-not-apply"
INVALID_GLOBAL_SYSNAME = "cfg-global-must-not-apply"
INVALID_CASE_SYSNAME = "cfg-case-must-not-apply"
LOOP_ID = 901
LOOP_IP = "198.18.9.1"
LOOP_PREFIX = 32
VRF_NAME = "ci-cfg-exit"
VRF_RT = "64512:901"
ROUTE_IP = "198.18.90.0"
ROUTE_PREFIX = 24
ISIS_TAG = 901
VTY_FIRST = 3
VTY_LAST = 4


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


def _write_container_file(rt: TopologyRuntime, path: str, content: str) -> None:
    proc = subprocess.run(
        [
            "docker",
            "exec",
            "-i",
            rt.container_name(DEV),
            "/bin/sh",
            "-c",
            'umask 077; cat > "$1"',
            "cfg-startup-write",
            path,
        ],
        input=content,
        text=True,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        text = (proc.stdout or "") + (proc.stderr or "")
        raise RuntimeError(f"failed to write container file {path!r}: {text}")


def _write_cfg_snapshot(rt: TopologyRuntime, name: str, content: str) -> None:
    config_dir = "/opt/netnexus/data/configs"
    cfg_path = f"{config_dir}/{name}.cfg"
    meta_path = f"{config_dir}/{name}.meta"
    checksum = hashlib.sha256(content.encode("utf-8")).hexdigest()
    metadata = (
        "version=ci\n"
        "format=bdr-indent-v1\n"
        "capture_complete=1\n"
        f"cfg_sha256={checksum}\n"
    )

    _container_sh(rt, f"mkdir -p {config_dir}")
    _write_container_file(rt, cfg_path, content)
    _write_container_file(rt, meta_path, metadata)
    _container_sh(rt, f"rm -f {config_dir}/{name}.db*")


def _show(rt: TopologyRuntime, command: str) -> str:
    return run_cmds(rt=rt, device=DEV, strict=False, timeout=30, commands=["end", command])[-1]


def _assert(
    label: str,
    output: str,
    *,
    contains: list[str] | None = None,
    not_contains: list[str] | None = None,
    regex: list[str] | None = None,
    not_regex: list[str] | None = None,
) -> None:
    violations = check_output(
        output,
        contains=contains or [],
        not_contains=not_contains or [],
        regex=regex or [],
        not_regex=not_regex or [],
        normalize_whitespace=False,
    )
    if violations:
        raise AssertionError(f"{label}: {'; '.join(violations)}\nOutput:\n{output}")


def _remove_artifacts(rt: TopologyRuntime) -> None:
    _container_sh(
        rt,
        "rm -f "
        "/opt/netnexus/data/startup.cfg "
        "/opt/netnexus/data/startup.cfg.tmp "
        "/opt/netnexus/data/startup-replay-failures.log "
        f"/opt/netnexus/data/configs/{EXPLICIT_SNAPSHOT}.cfg* "
        f"/opt/netnexus/data/configs/{EXPLICIT_SNAPSHOT}.db* "
        f"/opt/netnexus/data/configs/{EXPLICIT_SNAPSHOT}.meta* "
        f"/opt/netnexus/data/configs/{INVALID_SNAPSHOT}.cfg* "
        f"/opt/netnexus/data/configs/{INVALID_SNAPSHOT}.db* "
        f"/opt/netnexus/data/configs/{INVALID_SNAPSHOT}.meta* "
        f"/opt/netnexus/data/configs/{INVALID_EXIT_SNAPSHOT}.cfg* "
        f"/opt/netnexus/data/configs/{INVALID_EXIT_SNAPSHOT}.db* "
        f"/opt/netnexus/data/configs/{INVALID_EXIT_SNAPSHOT}.meta* "
        f"/opt/netnexus/data/configs/{INVALID_GLOBAL_SNAPSHOT}.cfg* "
        f"/opt/netnexus/data/configs/{INVALID_GLOBAL_SNAPSHOT}.db* "
        f"/opt/netnexus/data/configs/{INVALID_GLOBAL_SNAPSHOT}.meta* "
        f"/opt/netnexus/data/configs/{INVALID_CASE_SNAPSHOT}.cfg* "
        f"/opt/netnexus/data/configs/{INVALID_CASE_SNAPSHOT}.db* "
        f"/opt/netnexus/data/configs/{INVALID_CASE_SNAPSHOT}.meta*",
        check=False,
    )


def _cleanup_running(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=False,
        commands=[
            "end",
            "config",
            f"no route static ipv4 {ROUTE_IP} {ROUTE_PREFIX} interface null0",
            f"no isis {ISIS_TAG}",
            f"line vty {VTY_FIRST} {VTY_LAST}",
            "no transport input",
            "exit",
            f"no vrf {VRF_NAME}",
            f"no if loop {LOOP_ID}",
            f"sysname {BASE_SYSNAME}",
            "end",
        ],
    )


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup cfg startup replay state")
    _cleanup_running(rt)
    _remove_artifacts(rt)


def _explicit_exit_cfg() -> str:
    return (
        f"sysname {EXPLICIT_SYSNAME}\n"
        f"if loop {LOOP_ID}\n"
        f" ip address {LOOP_IP} {LOOP_PREFIX}\n"
        " exit\n"
        f"vrf {VRF_NAME}\n"
        " af ipv4\n"
        f"  vpn-target {VRF_RT} import\n"
        "  exit\n"
        " exit\n"
        f"line vty {VTY_FIRST} {VTY_LAST}\n"
        " transport input telnet\n"
        " exit\n"
        f"route static ipv4 {ROUTE_IP} {ROUTE_PREFIX} interface null0\n"
        f"isis {ISIS_TAG}\n"
        " no af ipv6\n"
        " exit\n"
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, (DEV,))

    try:
        _remove_artifacts(rt)
        _cleanup_running(rt)

        step("Create a cfg snapshot with explicit exits between sibling views")
        explicit_cfg = _explicit_exit_cfg()
        _write_cfg_snapshot(rt, EXPLICIT_SNAPSHOT, explicit_cfg)
        raw = _container_sh(rt, f"cat /opt/netnexus/data/configs/{EXPLICIT_SNAPSHOT}.cfg")
        _assert(
            "explicit exit cfg layout",
            raw,
            contains=[
                "  exit\n exit\n",
                f"line vty {VTY_FIRST} {VTY_LAST}\n transport input telnet\n exit\n",
                f"route static ipv4 {ROUTE_IP} {ROUTE_PREFIX} interface null0\n",
            ],
        )

        out = _show(rt, f"startup configuration {EXPLICIT_SNAPSHOT} cfg")
        _assert(
            "select explicit-exit cfg startup",
            out,
            contains=[f"Startup configuration set to '{EXPLICIT_SNAPSHOT}' (cfg)."],
        )

        step("Reboot and verify every sibling-view command after an explicit exit was replayed")
        reboot_device(rt, DEV, timeout=120)
        wait_check(
            rt,
            device=DEV,
            command="show current-configuration",
            timeout=40,
            interval=2,
            contains=[
                f"sysname {EXPLICIT_SYSNAME}",
                f"if loop {LOOP_ID}",
                f"ip address {LOOP_IP} {LOOP_PREFIX}",
                f"vrf {VRF_NAME}",
                "af ipv4",
                f"vpn-target {VRF_RT} import",
                f"line vty {VTY_FIRST} {VTY_LAST}",
                "transport input telnet",
                f"route static ipv4 {ROUTE_IP} {ROUTE_PREFIX} interface null0",
                f"isis {ISIS_TAG}",
                "no af ipv6",
            ],
            not_regex=[r"(?m)^\s*exit\s*$"],
            label="explicit exits return replay to each parent view",
        )
        out = _show(rt, "show configuration replay-failures")
        _assert("explicit-exit replay failures", out, contains=["Configuration replay failures:", "<none>"])

        step("Verify malformed hierarchy is rejected before its first command can mutate running state")
        _cleanup_running(rt)
        invalid_cfg = (
            f"sysname {INVALID_SYSNAME}\n"
            f"  if loop {LOOP_ID + 1}\n"
        )
        _write_cfg_snapshot(rt, INVALID_SNAPSHOT, invalid_cfg)
        out = _show(rt, f"startup configuration {INVALID_SNAPSHOT} cfg")
        _assert(
            "select malformed cfg startup",
            out,
            contains=[f"Startup configuration set to '{INVALID_SNAPSHOT}' (cfg)."],
        )

        reboot_device(rt, DEV, timeout=120)
        out = _show(rt, "show configuration replay-failures")
        _assert(
            "malformed cfg preflight failure",
            out,
            contains=[
                "Configuration replay failures:",
                "indentation requires view depth 3",
                "previous command left depth 1",
            ],
            regex=[rf"{INVALID_SNAPSHOT}\.cfg:2:"],
        )
        out = _show(rt, "show current-configuration")
        _assert(
            "malformed cfg caused no partial apply",
            out,
            not_contains=[INVALID_SYSNAME, f"if loop {LOOP_ID + 1}"],
        )

        step("Verify a root-level explicit exit is also rejected before partial apply")
        invalid_exit_cfg = (
            f"sysname {INVALID_EXIT_SYSNAME}\n"
            "exit\n"
        )
        _write_cfg_snapshot(rt, INVALID_EXIT_SNAPSHOT, invalid_exit_cfg)
        out = _show(rt, f"startup configuration {INVALID_EXIT_SNAPSHOT} cfg")
        _assert(
            "select invalid-exit cfg startup",
            out,
            contains=[f"Startup configuration set to '{INVALID_EXIT_SNAPSHOT}' (cfg)."],
        )

        reboot_device(rt, DEV, timeout=120)
        out = _show(rt, "show configuration replay-failures")
        _assert(
            "invalid explicit exit preflight failure",
            out,
            contains=[
                "Configuration replay failures:",
                "explicit exit would leave the config view",
            ],
        )
        out = _show(rt, "show current-configuration")
        _assert(
            "invalid explicit exit caused no partial apply",
            out,
            not_contains=[INVALID_EXIT_SYSNAME],
        )

        step("Verify global operational commands are forbidden in startup cfg")
        invalid_global_cfg = (
            f"sysname {INVALID_GLOBAL_SYSNAME}\n"
            "terminal length 0\n"
        )
        _write_cfg_snapshot(rt, INVALID_GLOBAL_SNAPSHOT, invalid_global_cfg)
        out = _show(rt, f"startup configuration {INVALID_GLOBAL_SNAPSHOT} cfg")
        _assert(
            "select invalid-global cfg startup",
            out,
            contains=[f"Startup configuration set to '{INVALID_GLOBAL_SNAPSHOT}' (cfg)."],
        )

        reboot_device(rt, DEV, timeout=120)
        out = _show(rt, "show configuration replay-failures")
        _assert(
            "global operational command preflight failure",
            out,
            contains=[
                "Configuration replay failures:",
                "command 'terminal length 0' is invalid in view 'config'",
            ],
        )
        out = _show(rt, "show current-configuration")
        _assert(
            "global operational command caused no partial apply",
            out,
            not_contains=[INVALID_GLOBAL_SYSNAME],
        )

        step("Verify control command case cannot diverge between preflight and execution")
        invalid_case_cfg = (
            f"sysname {INVALID_CASE_SYSNAME}\n"
            f"line vty {VTY_FIRST} {VTY_LAST}\n"
            " EXIT\n"
        )
        _write_cfg_snapshot(rt, INVALID_CASE_SNAPSHOT, invalid_case_cfg)
        out = _show(rt, f"startup configuration {INVALID_CASE_SNAPSHOT} cfg")
        _assert(
            "select invalid-case cfg startup",
            out,
            contains=[f"Startup configuration set to '{INVALID_CASE_SNAPSHOT}' (cfg)."],
        )

        reboot_device(rt, DEV, timeout=120)
        out = _show(rt, "show configuration replay-failures")
        _assert(
            "control command case preflight failure",
            out,
            contains=[
                "Configuration replay failures:",
                "command 'EXIT' is invalid",
            ],
        )
        out = _show(rt, "show current-configuration")
        _assert(
            "invalid control command case caused no partial apply",
            out,
            not_contains=[INVALID_CASE_SYSNAME],
        )

        print("CFG startup replay check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)


def main(rt: TopologyRuntime, top: dict[str, object]) -> None:
    run(rt, top)
