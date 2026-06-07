#!/usr/bin/env python3
"""
10k IPv4 static-batch null0 scale check.

This is a formal route CI case. It verifies that a 10000-route static-batch:
- can be shown through the CLI without command-engine errors
- installs into ROUTE, FIB, and FIB OS as null0/blackhole routes
- can be imported into BGP and advertised to a peer
- withdraws cleanly from ROUTE, FIB, FIB OS, local BGP, and peer BGP while
  BGP import-route static is still enabled
"""

from __future__ import annotations

import ipaddress
import os
import re
import subprocess
import time
from contextlib import contextmanager
from typing import Iterator

from module_api import g_top, cmd, require_devices, run_cmds, should_skip_cleanup, step, wait_checks
from top_runner import TopologyRuntime, dump_thread_stacks


BATCH_NAME = "scale10k"
START_ADDR = "1.1.1.1"
PREFIX_LEN = 32
COUNT = 10000
SOURCE_DEVICE = "r1"
PEER_DEVICE = "r2"

LOCAL_AS = 65101
PEER_AS = 65102
LOCAL_ROUTER_ID = "11.11.11.11"
PEER_ROUTER_ID = "22.22.22.22"

ACCESS_ENGINE_ERROR = "% command engine timeout or unavailable"
CLI_MODULE_TIMEOUT = "Error: Module timed out or failed to respond."

DEFAULT_CMD_TIMEOUT = int(os.environ.get("NN_STATIC_BATCH_10K_CMD_TIMEOUT", "120"))
FULL_SHOW_TIMEOUT = int(os.environ.get("NN_STATIC_BATCH_10K_FULL_SHOW_TIMEOUT", str(DEFAULT_CMD_TIMEOUT)))
BGP_WAIT_TIMEOUT = int(os.environ.get("NN_STATIC_BATCH_10K_BGP_WAIT_TIMEOUT", "180"))

EXPECTED_PREFIXES = frozenset(
    f"{ipaddress.IPv4Address(int(ipaddress.IPv4Address(START_ADDR)) + idx)}/{PREFIX_LEN}" for idx in range(COUNT)
)
FIRST_PREFIX = f"{START_ADDR}/{PREFIX_LEN}"
LAST_ADDR = str(ipaddress.IPv4Address(int(ipaddress.IPv4Address(START_ADDR)) + COUNT - 1))
LAST_PREFIX = f"{LAST_ADDR}/{PREFIX_LEN}"

STATIC_ROW_RE = re.compile(r"^\s*ipv4\s+(\d+(?:\.\d+){3}/32)\s+", re.MULTILINE)
RIB_ROW_RE = re.compile(r"^\s*S\s+(\d+(?:\.\d+){3}/32)\s+", re.MULTILINE)
FIB_ROW_RE = re.compile(
    r"^\s*ipv4\s+(\d+(?:\.\d+){3}/32)\s+\S+\s+blackhole\b.*\byes\s+no\s*$",
    re.MULTILINE,
)
FIB_OS_ROW_RE = re.compile(
    r"^\s*main\s+blackhole\s+(\d+(?:\.\d+){3}/32)\s+-\s+-\s+static\b",
    re.MULTILINE,
)
BGP_ROW_RE = re.compile(r"^\s*[> ]v\s+(\d+(?:\.\d+){3}/32)\s+", re.MULTILINE)


@contextmanager
def _cli_command_logging(rt: TopologyRuntime, device: str, enabled: bool) -> Iterator[None]:
    cli = rt.cli_map.get(device)
    if cli is None:
        yield
        return
    old = cli.log_commands
    cli.log_commands = enabled
    try:
        yield
    finally:
        cli.log_commands = old


def _tail(text: str, lines: int = 18) -> str:
    body = text.replace("\r", "").splitlines()
    if not body:
        return "(empty)"
    return "\n".join(body[-lines:])


def _head(text: str, lines: int = 12) -> str:
    body = text.replace("\r", "").splitlines()
    if not body:
        return "(empty)"
    return "\n".join(body[:lines])


def _safe_label(command: str) -> str:
    return re.sub(r"[^0-9A-Za-z_.-]+", "_", command).strip("_")[:60] or "command"


def _docker_probe(rt: TopologyRuntime, device: str) -> None:
    cname = rt.container_name(device)
    probes = {
        "processes": r'ps -eo pid,ppid,stat,comm,args | grep -E "netnexus|supervise" | grep -v grep',
        "tmp-log-tail": "tail -n 120 /tmp/netnexus.log 2>/dev/null || true",
        "module-logs": "ls -l /opt/netnexus/log 2>/dev/null || true",
    }
    for label, shell_cmd in probes.items():
        print(f"\n----- docker probe {device}: {label} -----", flush=True)
        proc = subprocess.run(
            ["docker", "exec", cname, "/bin/bash", "-lc", shell_cmd],
            text=True,
            capture_output=True,
            timeout=20,
        )
        out = (proc.stdout or "").strip()
        err = (proc.stderr or "").strip()
        if out:
            print(out, flush=True)
        if err:
            print("stderr:", err, flush=True)
        if not out and not err:
            print("(empty)", flush=True)


def _collect_cli_probe(rt: TopologyRuntime, reason: str) -> None:
    print(f"\n===== DIAG: collect state after {reason} =====", flush=True)
    for device in (SOURCE_DEVICE, PEER_DEVICE):
        for show_cmd in (
            "show dev modules",
            "show dev ipc cli",
            "show dev ipc route",
            "show dev ipc fib",
            "show dev ipc bgp",
            "show dev ipc access",
            "show dev subscribe",
        ):
            try:
                out = cmd(rt, device, show_cmd, strict=False, timeout=20)
                print(f"\n----- {device}: {show_cmd} -----\n{_tail(out, 40)}", flush=True)
            except Exception as exc:
                print(f"\n----- {device}: {show_cmd} failed -----\n{exc}", flush=True)
        _docker_probe(rt, device)
    dump_thread_stacks(rt, f"static-batch-10k-{reason}")


def _run_timed(
    rt: TopologyRuntime,
    device: str,
    command: str,
    *,
    timeout: int = DEFAULT_CMD_TIMEOUT,
    strict: bool = False,
    quiet: bool = True,
) -> tuple[str, float]:
    print(f"\n>>> {device}: {command} (timeout={timeout}s)", flush=True)
    started = time.monotonic()
    try:
        with _cli_command_logging(rt, device, not quiet):
            out = cmd(rt, device, command, strict=strict, timeout=timeout)
    except Exception as exc:
        elapsed = time.monotonic() - started
        print(f"<<< exception after {elapsed:.3f}s: {exc}", flush=True)
        _collect_cli_probe(rt, f"exception-{device}-{_safe_label(command)}")
        raise

    elapsed = time.monotonic() - started
    clean = out.replace("\r", "")
    print(
        f"<<< done in {elapsed:.3f}s, bytes={len(clean)}, lines={len(clean.splitlines())}",
        flush=True,
    )
    if ACCESS_ENGINE_ERROR in clean:
        print(f"ACCESS engine error seen in output:\n{_tail(clean)}", flush=True)
        _collect_cli_probe(rt, f"access-engine-error-{device}-{_safe_label(command)}")
        raise AssertionError(f"{device}: {command!r} returned ACCESS command-engine error")
    if CLI_MODULE_TIMEOUT in clean:
        print(f"CLI module timeout seen in output:\n{_tail(clean)}", flush=True)
        _collect_cli_probe(rt, f"cli-module-timeout-{device}-{_safe_label(command)}")
        raise AssertionError(f"{device}: {command!r} returned CLI module timeout")
    return clean, elapsed


def _extract_expected(output: str, row_re: re.Pattern[str]) -> set[str]:
    found: set[str] = set()
    for match in row_re.finditer(output):
        prefix = match.group(1)
        if prefix in EXPECTED_PREFIXES:
            found.add(prefix)
    return found


def _assert_patterns(label: str, output: str, patterns: list[str]) -> None:
    missing = [p for p in patterns if re.search(p, output, flags=re.MULTILINE) is None]
    if missing:
        raise AssertionError(f"{label}: regex not matched: {missing}; tail:\n{_tail(output)}")


def _run_count_once(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    row_re: re.Pattern[str],
    timeout: int,
    label: str,
) -> tuple[int, str, float]:
    output, elapsed = _run_timed(rt, device, command, timeout=timeout)
    matched = _extract_expected(output, row_re)
    print(f"{label}: matched {len(matched)}/{COUNT} expected prefixes", flush=True)
    return len(matched), output, elapsed


def _assert_prefix_count(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    row_re: re.Pattern[str],
    expected_count: int,
    timeout: int,
    label: str,
) -> str:
    count, output, elapsed = _run_count_once(
        rt,
        device=device,
        command=command,
        row_re=row_re,
        timeout=timeout,
        label=label,
    )
    if count != expected_count:
        print(f"{label} output head:", flush=True)
        print(_head(output), flush=True)
        print(f"{label} output tail:", flush=True)
        print(_tail(output), flush=True)
        _collect_cli_probe(rt, f"count-mismatch-{_safe_label(label)}")
        raise AssertionError(f"{label}: matched {count} expected prefixes, expected {expected_count}")
    print(f"{label} elapsed: {elapsed:.3f}s", flush=True)
    return output


def _wait_prefix_count(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    row_re: re.Pattern[str],
    expected_count: int,
    timeout: int,
    label: str,
    interval: int = 3,
) -> str:
    deadline = time.monotonic() + timeout
    last_output = ""
    last_count = -1
    while time.monotonic() < deadline:
        count, output, elapsed = _run_count_once(
            rt,
            device=device,
            command=command,
            row_re=row_re,
            timeout=FULL_SHOW_TIMEOUT,
            label=label,
        )
        last_output = output
        last_count = count
        if count == expected_count:
            print(f"{label} reached expected count in {elapsed:.3f}s show time", flush=True)
            return output
        time.sleep(interval)

    print(f"{label} last output head:", flush=True)
    print(_head(last_output), flush=True)
    print(f"{label} last output tail:", flush=True)
    print(_tail(last_output), flush=True)
    _collect_cli_probe(rt, f"wait-count-timeout-{_safe_label(label)}")
    raise AssertionError(f"{label}: matched {last_count} expected prefixes, expected {expected_count}")


def _cleanup(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device=SOURCE_DEVICE,
        strict=False,
        timeout=DEFAULT_CMD_TIMEOUT,
        commands=[
            "config",
            f"no route static-batch {BATCH_NAME}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device=PEER_DEVICE,
        strict=False,
        timeout=DEFAULT_CMD_TIMEOUT,
        commands=[
            "config",
            "no bgp",
            "end",
        ],
    )


def _configure_bgp(rt: TopologyRuntime) -> None:
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    run_cmds(
        rt=rt,
        device=SOURCE_DEVICE,
        strict=False,
        timeout=DEFAULT_CMD_TIMEOUT,
        commands=[
            "config",
            f"bgp {LOCAL_AS}",
            f"router-id {LOCAL_ROUTER_ID}",
            f"neighbor {r1_peer_ip} as {PEER_AS}",
            "af ipv4-unicast",
            f"neighbor {r1_peer_ip} enable",
            "import-route static",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device=PEER_DEVICE,
        strict=False,
        timeout=DEFAULT_CMD_TIMEOUT,
        commands=[
            "config",
            f"bgp {PEER_AS}",
            f"router-id {PEER_ROUTER_ID}",
            f"neighbor {r2_peer_ip} as {LOCAL_AS}",
            "af ipv4-unicast",
            f"neighbor {r2_peer_ip} enable",
            "exit",
            "end",
        ],
    )

    wait_checks(
        rt,
        [
            {
                "device": SOURCE_DEVICE,
                "command": "show route subscribe ipv4",
                "regex": [r"(?im)^\s*bgp\s+static\s+0\s+ipv4\s*$"],
                "label": "r1 route ipv4 static subscription installed",
            },
            {
                "device": SOURCE_DEVICE,
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1->r2 ipv4-unicast Established",
            },
            {
                "device": PEER_DEVICE,
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2->r1 ipv4-unicast Established",
            },
        ],
        timeout=40,
    )


def _verify_first_last_details(rt: TopologyRuntime) -> None:
    for addr, prefix in ((START_ADDR, FIRST_PREFIX), (LAST_ADDR, LAST_PREFIX)):
        route_out, _ = _run_timed(rt, SOURCE_DEVICE, f"show route ipv4 {addr} {PREFIX_LEN}", timeout=30)
        _assert_patterns(
            f"route detail {prefix}",
            route_out,
            [
                rf"(?im)^\s*Routing entry for\s+{re.escape(prefix)}\s+\(VRF:\s*public\)\s*$",
                r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\s*$",
                r"(?im)^\s*NH-Type\s*:\s*blackhole\s*$",
                r"(?im)^\s*Interface\s*:\s*Null0\s*$",
            ],
        )

        fib_out, _ = _run_timed(rt, SOURCE_DEVICE, f"show fib ipv4 {addr} {PREFIX_LEN}", timeout=30)
        _assert_patterns(
            f"fib detail {prefix}",
            fib_out,
            [
                rf"(?im)^\s*Routing entry for\s+{re.escape(prefix)}\s+\(VRF:\s*public\)\s*$",
                r"(?im)^\s*NH-Type\s*:\s*blackhole\s*$",
                r"(?im)^\s*Installed\s*:\s*yes\s*$",
                r"(?im)^\s*Skip OS\s*:\s*no\s*$",
            ],
        )


def _verify_first_last_bgp_details(rt: TopologyRuntime) -> None:
    for device in (SOURCE_DEVICE, PEER_DEVICE):
        for addr, prefix in ((START_ADDR, FIRST_PREFIX), (LAST_ADDR, LAST_PREFIX)):
            out, _ = _run_timed(rt, device, f"show bgp route af ipv4-unicast {addr} {PREFIX_LEN}", timeout=30)
            _assert_patterns(
                f"{device} bgp detail {prefix}",
                out,
                [
                    rf"(?im)^BGP Route Detail:\s+{re.escape(prefix)}\s+\(AF:\s*ipv4-unicast\)\s*$",
                    r"(?im)^\s*Valid\s*:\s*Yes\s*$",
                    r"(?im)^\s*Origin\s*:\s*INCOMPLETE\s*$",
                ],
            )


def _configure_static_batch(rt: TopologyRuntime) -> None:
    _run_timed(rt, SOURCE_DEVICE, "config", timeout=20, quiet=False)
    out, add_elapsed = _run_timed(
        rt,
        SOURCE_DEVICE,
        (
            f"route static-batch {BATCH_NAME} ipv4 {START_ADDR} {PREFIX_LEN} "
            f"interface null0 count {COUNT}"
        ),
        timeout=DEFAULT_CMD_TIMEOUT,
        quiet=False,
    )
    _run_timed(rt, SOURCE_DEVICE, "end", timeout=20, quiet=False)
    expect = f"Added {COUNT} static-batch IPv4 route(s) for '{BATCH_NAME}'"
    if expect not in out:
        raise AssertionError(f"batch add did not confirm {COUNT} routes; tail:\n{_tail(out)}")
    print(f"batch add elapsed: {add_elapsed:.3f}s", flush=True)


def _withdraw_static_batch(rt: TopologyRuntime) -> None:
    _run_timed(rt, SOURCE_DEVICE, "config", timeout=20, quiet=False)
    out, del_elapsed = _run_timed(
        rt,
        SOURCE_DEVICE,
        f"no route static-batch {BATCH_NAME}",
        timeout=DEFAULT_CMD_TIMEOUT,
        quiet=False,
    )
    _run_timed(rt, SOURCE_DEVICE, "end", timeout=20, quiet=False)
    expect = f"Cleared {COUNT} static-batch route(s) for '{BATCH_NAME}'"
    if expect not in out:
        raise AssertionError(f"batch delete did not confirm {COUNT} routes; tail:\n{_tail(out)}")
    print(f"batch delete elapsed: {del_elapsed:.3f}s", flush=True)


def _verify_route_plane_installed(rt: TopologyRuntime, phase: str) -> None:
    step(f"{phase}: Verify ROUTE candidate, RIB, FIB, and FIB OS contain all {COUNT} routes")
    _assert_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show route static ipv4",
        row_re=STATIC_ROW_RE,
        expected_count=COUNT,
        timeout=FULL_SHOW_TIMEOUT,
        label=f"{phase} static candidate table",
    )
    _assert_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show route ipv4",
        row_re=RIB_ROW_RE,
        expected_count=COUNT,
        timeout=FULL_SHOW_TIMEOUT,
        label=f"{phase} route RIB table",
    )
    _assert_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show fib ipv4",
        row_re=FIB_ROW_RE,
        expected_count=COUNT,
        timeout=FULL_SHOW_TIMEOUT,
        label=f"{phase} FIB table",
    )
    _assert_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show fib os ipv4",
        row_re=FIB_OS_ROW_RE,
        expected_count=COUNT,
        timeout=FULL_SHOW_TIMEOUT,
        label=f"{phase} FIB OS table",
    )
    _verify_first_last_details(rt)


def _verify_bgp_plane_count(rt: TopologyRuntime, phase: str, expected_count: int, detail: bool) -> None:
    _wait_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show bgp route af ipv4-unicast",
        row_re=BGP_ROW_RE,
        expected_count=expected_count,
        timeout=BGP_WAIT_TIMEOUT,
        label=f"{phase} r1 local BGP table",
    )
    _wait_prefix_count(
        rt,
        device=PEER_DEVICE,
        command="show bgp route af ipv4-unicast",
        row_re=BGP_ROW_RE,
        expected_count=expected_count,
        timeout=BGP_WAIT_TIMEOUT,
        label=f"{phase} r2 peer BGP table",
    )
    if detail:
        _verify_first_last_bgp_details(rt)


def _verify_all_withdrawn(rt: TopologyRuntime, phase: str) -> None:
    step(f"{phase}: Verify ROUTE, FIB, FIB OS, local BGP, and peer BGP withdraw all {COUNT} routes")
    _wait_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show route static ipv4",
        row_re=STATIC_ROW_RE,
        expected_count=0,
        timeout=60,
        label=f"{phase} static candidate table after withdraw",
    )
    _wait_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show route ipv4",
        row_re=RIB_ROW_RE,
        expected_count=0,
        timeout=60,
        label=f"{phase} route RIB table after withdraw",
    )
    _wait_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show fib ipv4",
        row_re=FIB_ROW_RE,
        expected_count=0,
        timeout=60,
        label=f"{phase} FIB table after withdraw",
    )
    _wait_prefix_count(
        rt,
        device=SOURCE_DEVICE,
        command="show fib os ipv4",
        row_re=FIB_OS_ROW_RE,
        expected_count=0,
        timeout=60,
        label=f"{phase} FIB OS table after withdraw",
    )
    _verify_bgp_plane_count(rt, f"{phase} after withdraw", expected_count=0, detail=False)


def _run_order_phase(rt: TopologyRuntime, phase: str, *, bgp_before_routes: bool) -> None:
    step(f"{phase}: Reset stale static-batch and BGP config")
    _cleanup(rt)

    if bgp_before_routes:
        step(f"{phase}: Configure IPv4 BGP peer and import-route static before static-batch")
        _configure_bgp(rt)

        step(f"{phase}: Verify BGP starts with no imported 10k routes")
        _verify_bgp_plane_count(rt, phase, expected_count=0, detail=False)

    step(f"{phase}: Configure 10000 IPv4 static-batch null0 routes")
    _configure_static_batch(rt)

    _verify_route_plane_installed(rt, phase)

    if not bgp_before_routes:
        step(f"{phase}: Configure IPv4 BGP peer and import-route static after static-batch")
        _configure_bgp(rt)

    step(f"{phase}: Verify BGP local import and peer advertisement contain all {COUNT} routes")
    _verify_bgp_plane_count(rt, phase, expected_count=COUNT, detail=True)

    step(f"{phase}: Withdraw static-batch while BGP import-route static remains enabled")
    _withdraw_static_batch(rt)
    _verify_all_withdrawn(rt, phase)

    step(f"{phase}: Reset BGP before next ordering")
    _cleanup(rt)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, (SOURCE_DEVICE, PEER_DEVICE))

    try:
        step("Baseline command-engine health")
        _cleanup(rt)
        _run_timed(rt, SOURCE_DEVICE, "show dev modules", timeout=20)
        _run_timed(rt, SOURCE_DEVICE, "show route summary ipv4", timeout=30)

        _run_order_phase(rt, "Phase A routes-before-BGP full replay", bgp_before_routes=False)
        _run_order_phase(rt, "Phase B BGP-before-routes incremental update", bgp_before_routes=True)

        print("10k static-batch ROUTE/FIB/FIB-OS/BGP import and withdraw ordering checks passed.", flush=True)
    finally:
        if should_skip_cleanup():
            return
        step("Cleanup 10k static-batch and BGP config")
        _cleanup(rt)
