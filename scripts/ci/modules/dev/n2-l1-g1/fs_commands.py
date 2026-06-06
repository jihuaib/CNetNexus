#!/usr/bin/env python3
"""Regression for dev fs commands: ls, cd, pwd and more."""

from __future__ import annotations

import subprocess

from module_api import check_output, cmd, require_devices, step
from top_runner import TopologyRuntime


DEV = "r1"
CASE_DIR = "/opt/netnexus/data/ci_fs"


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


def _assert_output(label: str, output: str, *, contains=None, not_contains=None) -> None:
    violations = check_output(output, contains=contains or [], not_contains=not_contains or [])
    if violations:
        raise AssertionError(f"{label}: {'; '.join(violations)}\nOutput:\n{output}")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, (DEV,))

    try:
        step("Prepare files under work directory")
        _container_sh(
            rt,
            f"rm -rf {CASE_DIR} && "
            f"mkdir -p {CASE_DIR}/nested && "
            f"printf 'alpha\\nbeta\\n' > {CASE_DIR}/sample.txt && "
            f"printf 'inner\\n' > {CASE_DIR}/nested/inner.txt && "
            f"ln -s /etc/passwd {CASE_DIR}/outside-link",
        )

        step("ls lists current work directory")
        out = cmd(rt, DEV, "ls")
        _assert_output("root ls", out, contains=["Directory: /", "data/"])

        step("pwd prints the current work directory")
        out = cmd(rt, DEV, "pwd")
        _assert_output("root pwd", out, contains=["/"])

        step("cd enters a directory under the work directory")
        out = cmd(rt, DEV, "cd data/ci_fs")
        _assert_output("cd case dir", out, contains=["Current directory: /data/ci_fs"])

        step("pwd follows cd state")
        out = cmd(rt, DEV, "pwd")
        _assert_output("case pwd", out, contains=["/data/ci_fs"])

        step("ls lists files in the new current directory")
        out = cmd(rt, DEV, "ls")
        _assert_output("case ls", out, contains=["sample.txt", "nested/", "outside-link"])

        step("more displays only regular file content")
        out = cmd(rt, DEV, "more sample.txt")
        _assert_output("more sample", out, contains=["alpha", "beta"], not_contains=["File:"])

        step("cd .. stays within the work directory")
        out = cmd(rt, DEV, "cd nested")
        _assert_output("cd nested", out, contains=["Current directory: /data/ci_fs/nested"])
        out = cmd(rt, DEV, "cd ..")
        _assert_output("cd parent", out, contains=["Current directory: /data/ci_fs"])

        step("cd escape is rejected")
        out = cmd(rt, DEV, "cd ../../..", strict=False)
        _assert_output("cd escape", out, contains=["Error: path escapes work directory"])

        step("more rejects symlink escape")
        out = cmd(rt, DEV, "more outside-link", strict=False)
        _assert_output("more symlink escape", out, contains=["Error: path escapes work directory"])

        step("absolute virtual path resets to work root")
        out = cmd(rt, DEV, "cd /")
        _assert_output("cd root", out, contains=["Current directory: /"])

        step("fs commands are user-view only")
        cmd(rt, DEV, "config")
        out = cmd(rt, DEV, "pwd", strict=False)
        _assert_output("pwd config view", out, contains=["Error: Invalid command"])
        out = cmd(rt, DEV, "ls", strict=False)
        _assert_output("ls config view", out, contains=["Error: Invalid command"])
        cmd(rt, DEV, "end", strict=False)

        step("line close clears per-line cwd state")
        out = cmd(rt, DEV, "cd data/ci_fs/nested")
        _assert_output("cd nested before close", out, contains=["Current directory: /data/ci_fs/nested"])
        try:
            rt.exec_cmd(DEV, "exit", strict=False, timeout=5)
        except RuntimeError as exc:
            if "CLI connection closed" not in str(exc):
                raise
        rt.ensure_cli_alive(DEV, reconnect_timeout=30)
        out = cmd(rt, DEV, "pwd")
        _assert_output("pwd after reconnect", out, contains=["/"], not_contains=["/data/ci_fs"])

        print("DEV fs command check passed.")
    finally:
        _container_sh(rt, f"rm -rf {CASE_DIR}", check=False)
