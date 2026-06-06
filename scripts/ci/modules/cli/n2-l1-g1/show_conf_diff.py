#!/usr/bin/env python3
"""Regression for `show configuration difference current-configuration <configuration-file>`."""

from __future__ import annotations

import re
import subprocess

from module_api import (  # noqa: E402
    check_output,
    g_top,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402


DEV = "r1"
BASE_SYSNAME = "r1"
SNAPSHOT = "ci_diff_base"
DIFF_CMD = "show configuration difference current-configuration"
BASE_LOOP_ID = 101
BASE_LOOP_IP4 = "172.31.101.1"
BASE_LOOP_IP6 = "2001:db8:101::1"
BASE_ROUTE4 = "198.51.100.0"
BASE_ROUTE6 = "2001:db8:100::"
BASE_BGP_AS = 65101
BASE_BGP_ROUTER_ID = "10.255.101.1"
CURRENT_LOOP_ID = 102
CURRENT_LOOP_IP4 = "172.31.102.1"
CURRENT_LOOP_IP6 = "2001:db8:102::1"
CURRENT_ROUTE4 = "198.51.101.0"
CURRENT_ROUTE6 = "2001:db8:101::"
CURRENT_BGP_AS = 65102
CURRENT_BGP_ROUTER_ID = "10.255.102.1"
LOOP_PREFIX4 = 32
LOOP_PREFIX6 = 128
ROUTE_PREFIX4 = 24
ROUTE_PREFIX6 = 64


def _base_lines() -> list[str]:
    return [
        f"if loop {BASE_LOOP_ID}",
        f" ip address {BASE_LOOP_IP4} {LOOP_PREFIX4}",
        f" ipv6 address {BASE_LOOP_IP6} {LOOP_PREFIX6}",
        f"route static ipv4 {BASE_ROUTE4} {ROUTE_PREFIX4} interface null0",
        f"route static ipv6 {BASE_ROUTE6} {ROUTE_PREFIX6} interface null0",
        f"bgp {BASE_BGP_AS}",
        f" router-id {BASE_BGP_ROUTER_ID}",
    ]


def _current_lines() -> list[str]:
    return [
        f"if loop {CURRENT_LOOP_ID}",
        f" ip address {CURRENT_LOOP_IP4} {LOOP_PREFIX4}",
        f" ipv6 address {CURRENT_LOOP_IP6} {LOOP_PREFIX6}",
        f"route static ipv4 {CURRENT_ROUTE4} {ROUTE_PREFIX4} interface null0",
        f"route static ipv6 {CURRENT_ROUTE6} {ROUTE_PREFIX6} interface null0",
        f"bgp {CURRENT_BGP_AS}",
        f" router-id {CURRENT_BGP_ROUTER_ID}",
    ]


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


def _show(rt: TopologyRuntime, command: str) -> str:
    return run_cmds(rt=rt, device=DEV, strict=False, timeout=30, commands=["end", command])[-1]


def _assert_output(label: str, output: str, *, contains=None, not_contains=None, regex=None, not_regex=None) -> None:
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


def _remove_snapshot(rt: TopologyRuntime) -> None:
    _container_sh(
        rt,
        "rm -f "
        f"/opt/netnexus/data/configs/{SNAPSHOT}.db* "
        f"/opt/netnexus/data/configs/{SNAPSHOT}.cfg* "
        f"/opt/netnexus/data/configs/{SNAPSHOT}.meta*",
        check=False,
    )


def _set_sysname(rt: TopologyRuntime, name: str, *, strict: bool = True) -> None:
    run_cmds(rt=rt, device=DEV, strict=strict, commands=["end", "config", f"sysname {name}", "end"])


def _line_regex(line: str) -> str:
    return rf"(?m)^\s*{re.escape(line.strip())}\s*$"


def _marked_line_regex(marker: str, line: str) -> str:
    return rf"(?m)^{re.escape(marker + line)}\s*$"


def _wait_config_lines(rt: TopologyRuntime, lines: list[str], *, label: str) -> None:
    wait_check(
        rt,
        device=DEV,
        command="show current-configuration",
        timeout=30,
        interval=2,
        regex=[_line_regex(line) for line in lines],
        label=label,
    )


def _cleanup_config(rt: TopologyRuntime, *, strict: bool = False) -> None:
    r1_ip4 = str(g_top.r1.GE_1.ip)
    r1_prefix4 = int(g_top.r1.GE_1.prefix)
    r1_ip6 = str(g_top.r1.GE_1.ip6)
    r1_prefix6 = int(g_top.r1.GE_1.prefix6)

    run_cmds(
        rt=rt,
        device=DEV,
        strict=strict,
        commands=[
            "end",
            "config",
            f"no route static ipv4 {BASE_ROUTE4} {ROUTE_PREFIX4} interface null0",
            f"no route static ipv6 {BASE_ROUTE6} {ROUTE_PREFIX6} interface null0",
            f"no route static ipv4 {CURRENT_ROUTE4} {ROUTE_PREFIX4} interface null0",
            f"no route static ipv6 {CURRENT_ROUTE6} {ROUTE_PREFIX6} interface null0",
            "no bgp",
            f"no if loop {BASE_LOOP_ID}",
            f"no if loop {CURRENT_LOOP_ID}",
            "if GE-1",
            f"no ip address {BASE_LOOP_IP4} {LOOP_PREFIX4}",
            f"no ip address {CURRENT_LOOP_IP4} {LOOP_PREFIX4}",
            f"no ipv6 address {BASE_LOOP_IP6} {LOOP_PREFIX6}",
            f"no ipv6 address {CURRENT_LOOP_IP6} {LOOP_PREFIX6}",
            f"ip address {r1_ip4} {r1_prefix4}",
            f"ipv6 address {r1_ip6} {r1_prefix6}",
            "exit",
            "end",
        ],
    )


def _configure_base(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=True,
        commands=[
            "end",
            "config",
            f"if loop {BASE_LOOP_ID}",
            f"ip address {BASE_LOOP_IP4} {LOOP_PREFIX4}",
            f"ipv6 address {BASE_LOOP_IP6} {LOOP_PREFIX6}",
            "exit",
            f"route static ipv4 {BASE_ROUTE4} {ROUTE_PREFIX4} interface null0",
            f"route static ipv6 {BASE_ROUTE6} {ROUTE_PREFIX6} interface null0",
            f"bgp {BASE_BGP_AS}",
            f"router-id {BASE_BGP_ROUTER_ID}",
            "end",
        ],
    )


def _configure_current(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=True,
        commands=[
            "end",
            "config",
            f"no route static ipv4 {BASE_ROUTE4} {ROUTE_PREFIX4} interface null0",
            f"no route static ipv6 {BASE_ROUTE6} {ROUTE_PREFIX6} interface null0",
            "no bgp",
            f"no if loop {BASE_LOOP_ID}",
            f"if loop {CURRENT_LOOP_ID}",
            f"ip address {CURRENT_LOOP_IP4} {LOOP_PREFIX4}",
            f"ipv6 address {CURRENT_LOOP_IP6} {LOOP_PREFIX6}",
            "exit",
            f"route static ipv4 {CURRENT_ROUTE4} {ROUTE_PREFIX4} interface null0",
            f"route static ipv6 {CURRENT_ROUTE6} {ROUTE_PREFIX6} interface null0",
            f"bgp {CURRENT_BGP_AS}",
            f"router-id {CURRENT_BGP_ROUTER_ID}",
            "end",
        ],
    )


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup CLI show-conf-diff case config")
    _set_sysname(rt, BASE_SYSNAME, strict=False)
    _cleanup_config(rt, strict=False)
    _remove_snapshot(rt)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, (DEV,))

    try:
        _cleanup(rt)

        step("Prepare baseline running configuration")
        _configure_base(rt)
        _wait_config_lines(rt, _base_lines(), label="baseline loop and static route visible before save")

        step("Save baseline cfg snapshot")
        out = _show(rt, f"save configuration {SNAPSHOT}")
        _assert_output("save baseline config", out, contains=[f"Configuration saved as '{SNAPSHOT}'"])

        step("Verify identical saved cfg reports no diff")
        out = _show(rt, f"{DIFF_CMD} {SNAPSHOT}.cfg")
        _assert_output(
            "identical saved cfg diff",
            out,
            contains=["No configuration difference."],
            not_regex=[r"(?m)^[+-](?:if loop| ip address| ipv6 address|route static)\b"],
        )

        step("Change current loop and static route configuration after save")
        _configure_current(rt)
        _wait_config_lines(rt, _current_lines(), label="changed loop and static route visible before diff")

        step("Verify saved cfg additions and current removals are marked")
        out = _show(rt, f"{DIFF_CMD} {SNAPSHOT}.cfg")
        _assert_output(
            "changed current vs saved cfg diff",
            out,
            not_contains=["No configuration difference."],
            regex=[_marked_line_regex("+", line) for line in _base_lines()]
            + [_marked_line_regex("-", line) for line in _current_lines()],
        )

        step("Verify missing cfg returns a read error")
        out = _show(rt, f"{DIFF_CMD} ci_diff_missing.cfg")
        _assert_output("missing cfg diff", out, contains=["Error: Unable to read configuration file"])

        print("CLI show-conf-diff check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
