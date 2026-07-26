#!/usr/bin/env python3
"""Regression for the unified configuration difference and rollback workflow."""

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
CONTEXT_SNAPSHOT = "ci_diff_context"
INVERSE_SNAPSHOT = "ci_diff_inverse"
NO_INVERSE_SNAPSHOT = "ci_diff_no_inverse"
EMPTY_AF_SNAPSHOT = "ci_diff_bgp_empty_af"
SNAPSHOTS = (SNAPSHOT, CONTEXT_SNAPSHOT, INVERSE_SNAPSHOT, NO_INVERSE_SNAPSHOT, EMPTY_AF_SNAPSHOT)
DIFF_CMD = "show configuration difference current-configuration"
CONSECUTIVE_SEPARATOR_RE = r"(?m)^[ \t]*![ \t]*\r?\n[ \t]*![ \t]*(?:\r?\n|$)"
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
CONTEXT_VRF_A = "ci-rb-a"
CONTEXT_VRF_B = "ci-rb-b"
CONTEXT_RT_BASE = "64512:101"
CONTEXT_RT_CURRENT = "64512:102"
INVERSE_ISIS_TAG = 9101
NO_INVERSE_BMP = "ci-rb-no-inverse"
NO_INVERSE_PEER = "192.0.2.101"
EMPTY_AF_BGP_AS = 65250
EMPTY_AF_VRF_A = "ci-rb-af-a"
EMPTY_AF_VRF_B = "ci-rb-af-b"
EMPTY_AF_RT = "64512:250"
EMPTY_AF_RD_A = "64512:251"
EMPTY_AF_RD_B = "64512:252"
EMPTY_AF_LOOP_ID = 250
EMPTY_AF_LOOP_IP = "172.31.250.1"
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


def _read_snapshot_cfg(rt: TopologyRuntime, name: str) -> str:
    return _container_sh(rt, f"cat /opt/netnexus/data/configs/{name}.cfg")


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


def _assert_regex_in_order(label: str, output: str, patterns: list[str]) -> None:
    offset = 0
    for pattern in patterns:
        match = re.search(pattern, output[offset:], re.MULTILINE)
        if not match:
            raise AssertionError(f"{label}: missing ordered pattern {pattern!r}\nOutput:\n{output}")
        offset += match.end()


def _remove_snapshot(rt: TopologyRuntime) -> None:
    paths = " ".join(
        f"/opt/netnexus/data/configs/{name}.db* "
        f"/opt/netnexus/data/configs/{name}.cfg* "
        f"/opt/netnexus/data/configs/{name}.meta*"
        for name in SNAPSHOTS
    )
    _container_sh(
        rt,
        f"rm -f {paths}",
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
            f"no isis {INVERSE_ISIS_TAG}",
            f"no if loop {BASE_LOOP_ID}",
            f"no if loop {CURRENT_LOOP_ID}",
            f"no if loop {EMPTY_AF_LOOP_ID}",
            "if GE-1",
            f"no ip address {BASE_LOOP_IP4} {LOOP_PREFIX4}",
            f"no ip address {CURRENT_LOOP_IP4} {LOOP_PREFIX4}",
            f"no ipv6 address {BASE_LOOP_IP6} {LOOP_PREFIX6}",
            f"no ipv6 address {CURRENT_LOOP_IP6} {LOOP_PREFIX6}",
            f"ip address {r1_ip4} {r1_prefix4}",
            f"ipv6 address {r1_ip6} {r1_prefix6}",
            "exit",
            f"no vrf {CONTEXT_VRF_A}",
            f"no vrf {CONTEXT_VRF_B}",
            f"no vrf {EMPTY_AF_VRF_A}",
            f"no vrf {EMPTY_AF_VRF_B}",
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


def _configure_context_vrfs(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=True,
        commands=[
            "end",
            "config",
            f"vrf {CONTEXT_VRF_A}",
            "af ipv4",
            f"vpn-target {CONTEXT_RT_BASE} import",
            "exit",
            "exit",
            f"vrf {CONTEXT_VRF_B}",
            "af ipv4",
            f"vpn-target {CONTEXT_RT_BASE} import",
            "exit",
            "exit",
            "end",
        ],
    )


def _change_only_context_vrf_a(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=True,
        commands=[
            "end",
            "config",
            f"vrf {CONTEXT_VRF_A}",
            "af ipv4",
            f"no vpn-target {CONTEXT_RT_BASE} import",
            f"vpn-target {CONTEXT_RT_CURRENT} import",
            "end",
        ],
    )


def _remove_context_vrfs(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=False,
        commands=[
            "end",
            "config",
            f"no vrf {CONTEXT_VRF_A}",
            f"no vrf {CONTEXT_VRF_B}",
            "end",
        ],
    )


def _configure_empty_af_snapshot_target(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=True,
        commands=[
            "end",
            "config",
            "no bgp",
            f"vrf {EMPTY_AF_VRF_A}",
            "af ipv4",
            f"route-distinguisher {EMPTY_AF_RD_A}",
            f"vpn-target {EMPTY_AF_RT} import",
            f"vpn-target {EMPTY_AF_RT} export",
            "exit",
            "exit",
            f"vrf {EMPTY_AF_VRF_B}",
            "af ipv4",
            f"route-distinguisher {EMPTY_AF_RD_B}",
            f"vpn-target {EMPTY_AF_RT} import",
            f"vpn-target {EMPTY_AF_RT} export",
            "exit",
            "exit",
            f"bgp {EMPTY_AF_BGP_AS}",
            f"vrf {EMPTY_AF_VRF_A}",
            "af ipv4-unicast",
            "import-route connected",
            "exit",
            "exit",
            f"vrf {EMPTY_AF_VRF_B}",
            "af ipv4-unicast",
            "exit",
            "exit",
            "exit",
            f"if loop {EMPTY_AF_LOOP_ID}",
            f"vrf forwarding {EMPTY_AF_VRF_A}",
            f"ip address {EMPTY_AF_LOOP_IP} {LOOP_PREFIX4}",
            "end",
        ],
    )


def _make_empty_af_runtime_only(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=True,
        commands=[
            "end",
            "config",
            f"bgp {EMPTY_AF_BGP_AS}",
            f"vrf {EMPTY_AF_VRF_B}",
            "no af ipv4-unicast",
            "end",
            "config",
            f"vrf {EMPTY_AF_VRF_B}",
            "af ipv4",
            f"no vpn-target {EMPTY_AF_RT} import",
            "end",
        ],
    )
    wait_check(
        rt,
        device=DEV,
        command=f"show bgp route af ipv4-unicast vrf {EMPTY_AF_VRF_B}",
        timeout=30,
        interval=2,
        not_contains=[f"{EMPTY_AF_LOOP_IP}/{LOOP_PREFIX4}"],
        label="leaked route removed before implicit AF recreation",
    )
    run_cmds(
        rt=rt,
        device=DEV,
        strict=True,
        commands=[
            "end",
            "config",
            f"vrf {EMPTY_AF_VRF_B}",
            "af ipv4",
            f"vpn-target {EMPTY_AF_RT} import",
            "end",
        ],
    )


def _cleanup_empty_af_case(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=DEV,
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"no if loop {EMPTY_AF_LOOP_ID}",
            f"no vrf {EMPTY_AF_VRF_A}",
            f"no vrf {EMPTY_AF_VRF_B}",
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
        out = _show(rt, "show current-configuration")
        _assert_output(
            "running config uses one separator between adjacent blocks",
            out,
            not_regex=[CONSECUTIVE_SEPARATOR_RE],
        )

        step("Save baseline cfg snapshot")
        out = _show(rt, f"save configuration {SNAPSHOT}")
        _assert_output("save baseline config", out, contains=[f"Configuration saved as '{SNAPSHOT}'"])
        _assert_output(
            "saved cfg uses one separator between adjacent blocks",
            _read_snapshot_cfg(rt, SNAPSHOT),
            not_regex=[CONSECUTIVE_SEPARATOR_RE],
        )

        step("Verify the legacy rollback-plan preview command is no longer registered")
        out = _show(rt, f"show configuration rollback-plan current-configuration {SNAPSHOT}")
        _assert_output(
            "legacy rollback-plan command removed",
            out,
            contains=["Error: Invalid command."],
            not_contains=["Rollback command plan"],
        )

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

        step("Apply rollback and verify the running BDR converges to the snapshot")
        out = _show(rt, f"rollback configuration {SNAPSHOT}")
        _assert_output(
            "apply configuration rollback",
            out,
            contains=[
                f"Configuration rolled back to '{SNAPSHOT}' successfully",
                "post-apply verification passed",
            ],
        )
        _wait_config_lines(rt, _base_lines(), label="baseline restored by configuration rollback")
        out = _show(rt, f"{DIFF_CMD} {SNAPSHOT}.cfg")
        _assert_output("post-rollback diff", out, contains=["No configuration difference."])

        step("Verify hierarchical diff stays inside the changed VRF context")
        _configure_context_vrfs(rt)
        out = _show(rt, f"save configuration {CONTEXT_SNAPSHOT}")
        _assert_output("save context snapshot", out, contains=[f"Configuration saved as '{CONTEXT_SNAPSHOT}'"])
        _change_only_context_vrf_a(rt)

        out = _show(rt, f"{DIFF_CMD} {CONTEXT_SNAPSHOT}")
        _assert_output(
            "context-scoped hierarchical diff",
            out,
            regex=[
                rf"(?m)^ vrf {re.escape(CONTEXT_VRF_A)}\s*$",
                r"(?m)^  af ipv4\s*$",
                rf"(?m)^\+\s{{2}}vpn-target {re.escape(CONTEXT_RT_BASE)} import\s*$",
                rf"(?m)^-\s{{2}}vpn-target {re.escape(CONTEXT_RT_CURRENT)} import\s*$",
            ],
            not_regex=[rf"(?m)^[ +\-]*vrf {re.escape(CONTEXT_VRF_B)}\s*$"],
        )

        out = _show(rt, f"rollback configuration {CONTEXT_SNAPSHOT}")
        _assert_output(
            "context-scoped rollback apply",
            out,
            contains=[
                f"Configuration rolled back to '{CONTEXT_SNAPSHOT}' successfully",
                "post-apply verification passed",
            ],
        )
        out = _show(rt, f"{DIFF_CMD} {CONTEXT_SNAPSHOT}")
        _assert_output("context rollback converged", out, contains=["No configuration difference."])
        _remove_context_vrfs(rt)

        step("Verify XML-declared inverse restores an ISIS semantic default")
        run_cmds(
            rt=rt,
            device=DEV,
            strict=True,
            commands=[
                "end",
                "config",
                f"isis {INVERSE_ISIS_TAG}",
                "no af ipv6",
                "end",
            ],
        )
        out = _show(rt, f"save configuration {INVERSE_SNAPSHOT}")
        _assert_output("save inverse snapshot", out, contains=[f"Configuration saved as '{INVERSE_SNAPSHOT}'"])
        run_cmds(
            rt=rt,
            device=DEV,
            strict=True,
            commands=[
                "end",
                "config",
                f"isis {INVERSE_ISIS_TAG}",
                "cost-style wide",
                "end",
            ],
        )
        out = _show(rt, f"{DIFF_CMD} {INVERSE_SNAPSHOT}")
        _assert_output(
            "declared inverse diff preview",
            out,
            regex=[
                rf"(?m)^ isis {INVERSE_ISIS_TAG}\s*$",
                r"(?m)^-\s+cost-style wide\s*$",
            ],
            not_contains=["Rollback command plan", "undo ", "add ", "view "],
        )
        out = _show(rt, f"rollback configuration {INVERSE_SNAPSHOT}")
        _assert_output(
            "declared inverse rollback",
            out,
            contains=[
                f"Configuration rolled back to '{INVERSE_SNAPSHOT}' successfully",
                "post-apply verification passed",
            ],
        )
        out = _show(rt, "show current-configuration")
        _assert_output(
            "ISIS narrow default restored",
            out,
            contains=[f"isis {INVERSE_ISIS_TAG}", "no af ipv6"],
            not_regex=[r"(?m)^\s*cost-style wide\s*$"],
        )
        run_cmds(
            rt=rt,
            device=DEV,
            strict=False,
            commands=["end", "config", f"no isis {INVERSE_ISIS_TAG}", "end"],
        )

        step("Verify diff remains readable while rollback refuses a command without a safe inverse")
        run_cmds(
            rt=rt,
            device=DEV,
            strict=True,
            commands=[
                "end",
                "config",
                f"bgp {BASE_BGP_AS}",
                f"bmp instance {NO_INVERSE_BMP}",
                f"monitor neighbor {NO_INVERSE_PEER}",
                "end",
            ],
        )
        out = _show(rt, f"save configuration {NO_INVERSE_SNAPSHOT}")
        _assert_output(
            "save no-inverse snapshot",
            out,
            contains=[f"Configuration saved as '{NO_INVERSE_SNAPSHOT}'"],
        )
        run_cmds(
            rt=rt,
            device=DEV,
            strict=True,
            commands=[
                "end",
                "config",
                f"bgp {BASE_BGP_AS}",
                f"bmp instance {NO_INVERSE_BMP}",
                "monitor neighbor all",
                "end",
            ],
        )
        out = _show(rt, f"{DIFF_CMD} {NO_INVERSE_SNAPSHOT}")
        _assert_output(
            "no-inverse structural diff preview",
            out,
            regex=[
                rf"(?m)^ bgp {BASE_BGP_AS}\s*$",
                rf"(?m)^  bmp instance {re.escape(NO_INVERSE_BMP)}\s*$",
                rf"(?m)^\+\s{{2}}monitor neighbor {re.escape(NO_INVERSE_PEER)}\s*$",
                r"(?m)^-\s{2}monitor neighbor all\s*$",
            ],
            not_contains=["Error: Rollback preflight failed", "Rollback command plan"],
        )
        out = _show(rt, f"rollback configuration {NO_INVERSE_SNAPSHOT}")
        _assert_output(
            "no-inverse apply refused",
            out,
            contains=["Error: Rollback preflight failed", "monitor neighbor all", "<inverse>"],
            not_contains=["successfully"],
        )
        out = _show(rt, "show current-configuration")
        _assert_output(
            "no-inverse refusal did not partially mutate running config",
            out,
            contains=[f"bmp instance {NO_INVERSE_BMP}", "monitor neighbor all"],
            not_contains=[f"monitor neighbor {NO_INVERSE_PEER}"],
        )
        run_cmds(
            rt=rt,
            device=DEV,
            strict=False,
            commands=[
                "end",
                "config",
                f"bgp {BASE_BGP_AS}",
                f"no bmp instance {NO_INVERSE_BMP}",
                "end",
            ],
        )

        step("Build a saved BGP target with an empty VRF AF and a local leaked route")
        _configure_empty_af_snapshot_target(rt)
        wait_check(
            rt,
            device=DEV,
            command=f"show bgp route af ipv4-unicast vrf {EMPTY_AF_VRF_B}",
            timeout=30,
            interval=2,
            contains=[f"{EMPTY_AF_LOOP_IP}/{LOOP_PREFIX4}"],
            label="connected route leaked into the target VRF",
        )
        out = _show(rt, f"save configuration {EMPTY_AF_SNAPSHOT}")
        _assert_output(
            "save empty-AF rollback target",
            out,
            contains=[f"Configuration saved as '{EMPTY_AF_SNAPSHOT}'"],
        )
        saved_empty_af_cfg = _read_snapshot_cfg(rt, EMPTY_AF_SNAPSHOT)
        _assert_output(
            "empty-AF snapshot is canonical",
            saved_empty_af_cfg,
            not_regex=[CONSECUTIVE_SEPARATOR_RE],
        )
        _assert_regex_in_order(
            "empty-AF snapshot keeps both BGP VRF views",
            saved_empty_af_cfg,
            [
                rf"^bgp {EMPTY_AF_BGP_AS}\s*$",
                rf"^ vrf {re.escape(EMPTY_AF_VRF_A)}\s*$",
                r"^  af ipv4-unicast\s*$",
                r"^   import-route connected\s*$",
                rf"^ vrf {re.escape(EMPTY_AF_VRF_B)}\s*$",
                r"^  af ipv4-unicast\s*$",
            ],
        )

        step("Force VRF B AF to exist only in BGP runtime through route leaking")
        _make_empty_af_runtime_only(rt)
        wait_check(
            rt,
            device=DEV,
            command=f"show bgp route af ipv4-unicast vrf {EMPTY_AF_VRF_B}",
            timeout=30,
            interval=2,
            contains=[f"{EMPTY_AF_LOOP_IP}/{LOOP_PREFIX4}"],
            label="route leaking implicitly recreated the runtime AF",
        )
        out = _show(rt, f"{DIFF_CMD} {EMPTY_AF_SNAPSHOT}")
        _assert_output(
            "runtime-only AF is absent from running BDR",
            out,
            regex=[
                rf"(?m)^ bgp {EMPTY_AF_BGP_AS}\s*$",
                rf"(?m)^  vrf {re.escape(EMPTY_AF_VRF_B)}\s*$",
                r"(?m)^\+  af ipv4-unicast\s*$",
            ],
            not_regex=[
                rf"(?m)^[ +\-]*vrf {re.escape(EMPTY_AF_VRF_A)}\s*$",
                r"(?m)^[+-]\s*!\s*$",
            ],
        )

        step("Rollback the empty AF while its runtime instance already exists")
        out = _show(rt, f"rollback configuration {EMPTY_AF_SNAPSHOT}")
        _assert_output(
            "runtime-only empty-AF rollback apply",
            out,
            contains=[
                f"Configuration rolled back to '{EMPTY_AF_SNAPSHOT}' successfully",
                "post-apply verification passed",
            ],
        )
        out = _show(rt, f"{DIFF_CMD} {EMPTY_AF_SNAPSHOT}")
        _assert_output("runtime-only empty-AF rollback converged", out, contains=["No configuration difference."])

        step("Rollback the complete BGP tree after no bgp, matching the reported failure")
        run_cmds(rt=rt, device=DEV, strict=True, commands=["end", "config", "no bgp", "end"])
        out = _show(rt, f"{DIFF_CMD} {EMPTY_AF_SNAPSHOT}")
        _assert_output(
            "complete BGP rollback diff",
            out,
            not_contains=["Rollback command plan", "undo ", "add ", "view "],
            not_regex=[r"(?m)^[+-]\s*!\s*$"],
        )
        _assert_regex_in_order(
            "complete BGP rollback diff order",
            out,
            [
                rf"^\+bgp {EMPTY_AF_BGP_AS}\s*$",
                rf"^\+ vrf {re.escape(EMPTY_AF_VRF_A)}\s*$",
                r"^\+  af ipv4-unicast\s*$",
                r"^\+   import-route connected\s*$",
                rf"^\+ vrf {re.escape(EMPTY_AF_VRF_B)}\s*$",
                r"^\+  af ipv4-unicast\s*$",
            ],
        )
        out = _show(rt, f"rollback configuration {EMPTY_AF_SNAPSHOT}")
        _assert_output(
            "complete BGP rollback apply",
            out,
            contains=[
                f"Configuration rolled back to '{EMPTY_AF_SNAPSHOT}' successfully",
                "post-apply verification passed",
            ],
        )
        out = _show(rt, f"{DIFF_CMD} {EMPTY_AF_SNAPSHOT}")
        _assert_output("complete BGP rollback converged", out, contains=["No configuration difference."])
        out = _show(rt, "show current-configuration")
        _assert_output(
            "post-rollback running config remains canonical",
            out,
            not_regex=[CONSECUTIVE_SEPARATOR_RE],
        )
        _assert_regex_in_order(
            "complete rollback keeps the empty VRF B AF",
            out,
            [
                rf"^bgp {EMPTY_AF_BGP_AS}\s*$",
                rf"^ vrf {re.escape(EMPTY_AF_VRF_B)}\s*$",
                r"^  af ipv4-unicast\s*$",
            ],
        )
        _cleanup_empty_af_case(rt)

        step("Verify missing cfg returns a read error")
        out = _show(rt, f"{DIFF_CMD} ci_diff_missing.cfg")
        _assert_output("missing cfg diff", out, contains=["Error: Unable to read configuration file"])

        step("Verify diff cannot read files outside the named snapshot directory")
        out = _show(rt, f"{DIFF_CMD} /etc/passwd")
        _assert_output(
            "snapshot path traversal rejected",
            out,
            contains=["Error: Invalid snapshot name"],
            not_contains=["root:x:"],
        )

        print("CLI show-conf-diff check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
