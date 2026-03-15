#!/usr/bin/env python3
"""
Common helpers for CI module scripts.

Modules should focus on scenario commands and assertions, while shared
execution/wait boilerplate lives here.
"""

from __future__ import annotations

import time
from typing import Iterable

from top_runner import TopologyRuntime, execCmd


def step(title: str) -> None:
    print(f"\n===== STEP: {title} =====", flush=True)


def require_devices(top: dict, required: Iterable[str]) -> None:
    devices = top.get("devices")
    if not isinstance(devices, dict) or not devices:
        raise ValueError("top.devices must be a non-empty mapping")

    have = set(devices.keys())
    need = set(required)
    missing = sorted(need - have)
    if missing:
        raise ValueError(f"topology missing required devices: {', '.join(missing)}")


def cmd(
    rt: TopologyRuntime,
    device: str,
    command: str,
    *,
    strict: bool = True,
    timeout: int | None = None,
) -> str:
    return execCmd(rt, device).exec(command, strict=strict, timeout=timeout)


def run_cmds(
    rt: TopologyRuntime,
    device: str,
    commands: Iterable[str],
    *,
    strict: bool = True,
    timeout: int | None = None,
) -> list[str]:
    outputs: list[str] = []
    for command in commands:
        outputs.append(cmd(rt, device, command, strict=strict, timeout=timeout))
    return outputs


def reboot_device(rt: TopologyRuntime, device: str, *, timeout: int = 90) -> None:
    rt.reboot_device(device, reconnect_timeout=timeout)


def wait_checks(
    rt: TopologyRuntime,
    checks: list[dict[str, object]],
    *,
    timeout: int,
    interval: int = 2,
) -> None:
    """
    Generic polling checker.

    Each check item:
      - device: str
      - command: str
      - contains: list[str]  (all substrings must appear)
      - label: str (optional, used in error text)
    """
    if not checks:
        return

    deadline = time.time() + timeout
    last_out: dict[str, str] = {}
    last_missing: list[str] = []

    while time.time() < deadline:
        pending = 0
        missing_detail: list[str] = []

        for chk in checks:
            device = str(chk["device"])
            command = str(chk["command"])
            tokens = [str(x) for x in chk.get("contains", [])]
            label = str(chk.get("label", f"{device}: {command}"))

            out = cmd(rt, device, command, strict=False)
            last_out[device] = out

            miss = [t for t in tokens if t not in out]
            if miss:
                pending += 1
                missing_detail.append(f"{label} missing: {', '.join(miss)}")

        if pending == 0:
            return

        last_missing = missing_detail
        time.sleep(interval)

    detail = "\n".join(last_missing)
    output_dump = "\n\n".join([f"[{dev}]\n{out}" for dev, out in last_out.items()])
    raise RuntimeError(
        f"checks not satisfied within {timeout}s\n{detail}\n\nlast outputs:\n{output_dump}"
    )
