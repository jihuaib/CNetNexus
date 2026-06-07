#!/usr/bin/env python3
"""
BGP VRF export-RT rebuild scale check.

This targets the export-RT change path specifically:
- r1/r2 run a VRF ipv4-unicast eBGP session.
- r1 imports 10000 VRF static-batch routes and advertises them to r2.
- r2 has a local VRF export RT, so received routes get that RT in effective attrs.
- r2 removes the old local export RT, configures a new export RT, then deletes
  the new export RT immediately while r1 withdraws the static-batch.
- The peer table must drain cleanly, then recover to 10000 routes without stale
  Ext-Comm RT attributes after the static-batch is re-added.
- The final no-export-RT state is saved and both nodes reboot; after restore,
  the 10000 routes must come back without resurrecting stale ERT.
"""

from __future__ import annotations

import ipaddress
import os
import re
import threading
import time
from contextlib import contextmanager
from typing import Iterator

from module_api import g_top, cmd, reboot_device, require_devices, run_cmds, should_skip_cleanup, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
GE_IF = "GE-1"

R1_V4 = "10.99.0.1"
R2_V4 = "10.99.0.2"
V4_LEN = 30

R1_AS = 65201
R2_AS = 65202
R1_RD = "65201:99"
R2_RD = "65202:99"
OLD_EXPORT_RT = "65202:100"
NEW_EXPORT_RT = "65202:200"
OLD_EXPORT_RT_TEXT = f"rt:{OLD_EXPORT_RT}"
NEW_EXPORT_RT_TEXT = f"rt:{NEW_EXPORT_RT}"

BATCH_NAME = "vrfert10k"
START_ADDR = "10.252.0.1"
PREFIX_LEN = 32
COUNT = 10000
FIRST_PREFIX = f"{START_ADDR}/{PREFIX_LEN}"
LAST_ADDR = str(ipaddress.IPv4Address(int(ipaddress.IPv4Address(START_ADDR)) + COUNT - 1))
LAST_PREFIX = f"{LAST_ADDR}/{PREFIX_LEN}"

DEFAULT_CMD_TIMEOUT = int(os.environ.get("NN_BGP_VRF_ERT_10K_CMD_TIMEOUT", "120"))
BGP_WAIT_TIMEOUT = int(os.environ.get("NN_BGP_VRF_ERT_10K_WAIT_TIMEOUT", "240"))
BGP_SHOW_TIMEOUT = int(os.environ.get("NN_BGP_VRF_ERT_10K_SHOW_TIMEOUT", "120"))

EXPECTED_PREFIXES = frozenset(
    f"{ipaddress.IPv4Address(int(ipaddress.IPv4Address(START_ADDR)) + idx)}/{PREFIX_LEN}" for idx in range(COUNT)
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
    return "\n".join(body[-lines:]) if body else "(empty)"


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
    with _cli_command_logging(rt, device, not quiet):
        out = cmd(rt, device, command, strict=strict, timeout=timeout)
    elapsed = time.monotonic() - started
    clean = out.replace("\r", "")
    print(f"<<< done in {elapsed:.3f}s, bytes={len(clean)}, lines={len(clean.splitlines())}", flush=True)
    if "% command engine timeout or unavailable" in clean or "Error: Module timed out" in clean:
        raise AssertionError(f"{device}: {command!r} returned timeout/error:\n{_tail(clean)}")
    return clean, elapsed


def _extract_expected(output: str) -> set[str]:
    found: set[str] = set()
    for match in BGP_ROW_RE.finditer(output):
        prefix = match.group(1)
        if prefix in EXPECTED_PREFIXES:
            found.add(prefix)
    return found


def _wait_bgp_count(rt: TopologyRuntime, expected_count: int, label: str) -> str:
    deadline = time.monotonic() + BGP_WAIT_TIMEOUT
    last_count = -1
    last_output = ""
    command = f"show bgp route af ipv4-unicast vrf {VRF_NAME}"
    while time.monotonic() < deadline:
        out, elapsed = _run_timed(rt, "r2", command, timeout=BGP_SHOW_TIMEOUT)
        last_output = out
        last_count = len(_extract_expected(out))
        print(f"{label}: matched {last_count}/{COUNT} expected prefixes; show elapsed={elapsed:.3f}s", flush=True)
        if last_count == expected_count:
            return out
        time.sleep(3)
    raise AssertionError(f"{label}: matched {last_count}, expected {expected_count}; tail:\n{_tail(last_output)}")


def _cleanup(rt: TopologyRuntime, base: dict[str, str | int]) -> None:
    for dev, local_v4 in (("r1", R1_V4), ("r2", R2_V4)):
        commands = [
            "end",
            "config",
            f"no route static-batch {BATCH_NAME}",
            "no bgp",
            f"if {GE_IF}",
            "no shutdown",
            f"no ip address {local_v4} {V4_LEN}",
            "no vrf forwarding",
            f"ip address {base[f'{dev}_v4']} {base[f'{dev}_v4_len']}",
            f"ipv6 address {base[f'{dev}_v6']} {base[f'{dev}_v6_len']}",
            "exit",
            f"no vrf {VRF_NAME}",
            "end",
        ]
        run_cmds(rt=rt, device=dev, strict=False, timeout=DEFAULT_CMD_TIMEOUT, commands=commands)


def _setup_vrf_if(rt: TopologyRuntime, device: str, local_v4: str, rd: str, *, export_rt: str | None) -> None:
    commands = [
        "config",
        f"vrf {VRF_NAME}",
        "af ipv4-unicast",
        f"route-distinguisher {rd}",
    ]
    if export_rt:
        commands.append(f"vpn-target {export_rt} export")
    commands.extend(
        [
            "exit",
            "exit",
            f"if {GE_IF}",
            "no shutdown",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {local_v4} {V4_LEN}",
            "exit",
            "end",
        ]
    )
    run_cmds(rt=rt, device=device, timeout=DEFAULT_CMD_TIMEOUT, commands=commands)


def _setup_bgp(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        timeout=DEFAULT_CMD_TIMEOUT,
        commands=[
            "config",
            f"bgp {R1_AS}",
            f"vrf {VRF_NAME}",
            "router-id 1.1.1.1",
            f"neighbor {R2_V4} as {R2_AS}",
            "af ipv4-unicast",
            f"neighbor {R2_V4} enable",
            "import-route static",
            "exit",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        timeout=DEFAULT_CMD_TIMEOUT,
        commands=[
            "config",
            f"bgp {R2_AS}",
            f"vrf {VRF_NAME}",
            "router-id 2.2.2.2",
            f"neighbor {R1_V4} as {R1_AS}",
            "af ipv4-unicast",
            f"neighbor {R1_V4} enable",
            "exit",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                "regex": [rf"(?im)^\s*{re.escape(R2_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1 VRF session established",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                "regex": [rf"(?im)^\s*{re.escape(R1_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2 VRF session established",
            },
        ],
        timeout=60,
        interval=2,
    )


def _install_static_batch(rt: TopologyRuntime) -> None:
    _run_timed(rt, "r1", "config", timeout=20, quiet=False)
    out, elapsed = _run_timed(
        rt,
        "r1",
        f"route static-batch {BATCH_NAME} ipv4 vrf {VRF_NAME} {START_ADDR} {PREFIX_LEN} {R2_V4} count {COUNT}",
        timeout=DEFAULT_CMD_TIMEOUT,
        quiet=False,
    )
    _run_timed(rt, "r1", "end", timeout=20, quiet=False)
    expect = f"Added {COUNT} static-batch IPv4 route(s) for '{BATCH_NAME}'"
    if expect not in out:
        raise AssertionError(f"batch add did not confirm {COUNT} routes; elapsed={elapsed:.3f}s tail:\n{_tail(out)}")


def _remove_static_batch(rt: TopologyRuntime) -> None:
    _run_timed(rt, "r1", "config", timeout=20, quiet=False)
    out, elapsed = _run_timed(
        rt,
        "r1",
        f"no route static-batch {BATCH_NAME}",
        timeout=DEFAULT_CMD_TIMEOUT,
        quiet=False,
    )
    _run_timed(rt, "r1", "end", timeout=20, quiet=False)
    expect = f"Cleared {COUNT} static-batch route(s) for '{BATCH_NAME}'"
    if expect not in out:
        raise AssertionError(f"batch delete did not confirm {COUNT} routes; elapsed={elapsed:.3f}s tail:\n{_tail(out)}")


def _add_then_delete_export_rt(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r2",
        timeout=DEFAULT_CMD_TIMEOUT,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"no vpn-target {OLD_EXPORT_RT} export",
            f"vpn-target {NEW_EXPORT_RT} export",
            f"no vpn-target {NEW_EXPORT_RT} export",
            "exit",
            "exit",
            "end",
        ],
    )


def _run_export_rt_add_delete_during_batch_withdraw(rt: TopologyRuntime) -> None:
    errors: list[BaseException] = []

    def run_checked(label: str, fn) -> None:
        try:
            print(f"{label}: start", flush=True)
            fn()
            print(f"{label}: done", flush=True)
        except BaseException as exc:  # noqa: BLE001
            errors.append(AssertionError(f"{label} failed: {exc}"))

    export_thread = threading.Thread(
        target=run_checked,
        args=("r2 export-RT add/delete rebuild", lambda: _add_then_delete_export_rt(rt)),
        daemon=True,
    )
    withdraw_thread = threading.Thread(
        target=run_checked,
        args=("r1 static-batch withdraw", lambda: _remove_static_batch(rt)),
        daemon=True,
    )

    export_thread.start()
    time.sleep(0.02)
    withdraw_thread.start()
    export_thread.join(DEFAULT_CMD_TIMEOUT + 30)
    withdraw_thread.join(DEFAULT_CMD_TIMEOUT + 30)
    if export_thread.is_alive() or withdraw_thread.is_alive():
        raise AssertionError("concurrent export-RT rebuild/static-batch withdraw did not finish")
    if errors:
        raise errors[0]


def _route_detail_check(addr: str, prefix: str, want_rt: str, reject_rt: str, label: str) -> dict[str, object]:
    return {
        "device": "r2",
        "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} {addr} {PREFIX_LEN}",
        "contains": [
            f"BGP Route Detail: {prefix}",
            f"From Peer  : {R1_V4}",
            f"Ext-Comm : {want_rt}",
        ],
        "not_contains": [f"Ext-Comm : {reject_rt}"],
        "regex": [rf"(?im)^\s*AS-Path\s*:\s*{R1_AS}\s*$"],
        "label": label,
    }


def _route_detail_without_export_rt_check(addr: str, prefix: str, label: str) -> dict[str, object]:
    return {
        "device": "r2",
        "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} {addr} {PREFIX_LEN}",
        "contains": [
            f"BGP Route Detail: {prefix}",
            f"From Peer  : {R1_V4}",
        ],
        "not_contains": [
            "Ext-Comm",
            OLD_EXPORT_RT_TEXT,
            NEW_EXPORT_RT_TEXT,
        ],
        "regex": [rf"(?im)^\s*AS-Path\s*:\s*{R1_AS}\s*$"],
        "label": label,
    }


def _wait_sample_routes(rt: TopologyRuntime, want_rt: str, reject_rt: str, phase: str) -> None:
    wait_checks(
        rt,
        [
            _route_detail_check(START_ADDR, FIRST_PREFIX, want_rt, reject_rt, f"{phase}: first route RT rebuilt"),
            _route_detail_check(LAST_ADDR, LAST_PREFIX, want_rt, reject_rt, f"{phase}: last route RT rebuilt"),
        ],
        timeout=BGP_WAIT_TIMEOUT,
        interval=3,
    )


def _wait_sample_routes_without_export_rt(rt: TopologyRuntime, phase: str) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show vrf name {VRF_NAME}",
                "not_contains": ["Export-RT", OLD_EXPORT_RT, NEW_EXPORT_RT],
                "label": f"{phase}: r2 local export RT removed from VRF",
            },
            _route_detail_without_export_rt_check(
                START_ADDR,
                FIRST_PREFIX,
                f"{phase}: first route has no stale export RT",
            ),
            _route_detail_without_export_rt_check(
                LAST_ADDR,
                LAST_PREFIX,
                f"{phase}: last route has no stale export RT",
            ),
        ],
        timeout=BGP_WAIT_TIMEOUT,
        interval=3,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    base = {
        "r1_v4": str(g_top.r1.GE_1.ip),
        "r1_v4_len": int(g_top.r1.GE_1.prefix),
        "r1_v6": str(g_top.r1.GE_1.ip6),
        "r1_v6_len": int(g_top.r1.GE_1.prefix6),
        "r2_v4": str(g_top.r2.GE_1.ip),
        "r2_v4_len": int(g_top.r2.GE_1.prefix),
        "r2_v6": str(g_top.r2.GE_1.ip6),
        "r2_v6_len": int(g_top.r2.GE_1.prefix6),
    }

    try:
        step("Reset previous BGP VRF export-RT 10k state")
        _cleanup(rt, base)

        step("Create VRF red on both nodes; r2 starts with old export RT")
        _setup_vrf_if(rt, "r1", R1_V4, R1_RD, export_rt=None)
        _setup_vrf_if(rt, "r2", R2_V4, R2_RD, export_rt=OLD_EXPORT_RT)

        step("Configure VRF ipv4-unicast eBGP and import-route static on r1")
        _setup_bgp(rt)

        step("Install 10000 private VRF static routes on r1")
        _install_static_batch(rt)

        step("Verify r2 received all 10000 private BGP routes")
        _wait_bgp_count(rt, COUNT, "r2 VRF BGP table before export RT change")

        step("Verify sampled received routes carry the old r2 export RT")
        _wait_sample_routes(rt, OLD_EXPORT_RT_TEXT, NEW_EXPORT_RT_TEXT, "before change")

        step("Add then immediately delete r2 export RT while r1 withdraws the 10000-route static-batch")
        _run_export_rt_add_delete_during_batch_withdraw(rt)

        step("Verify r2 withdraws all 10000 routes after overlapping export-RT add/delete and batch withdraw")
        _wait_bgp_count(rt, 0, "r2 VRF BGP table after overlapping export RT add/delete and batch withdraw")

        step("Re-add 10000 routes after churn and verify no stale export RT remains")
        _install_static_batch(rt)
        _wait_bgp_count(rt, COUNT, "r2 VRF BGP table after static-batch re-add")
        _wait_sample_routes_without_export_rt(rt, "after immediate export RT delete")

        step("Save final no-export-RT state, reboot both nodes, and verify no ERT resurrection")
        reboot_device(rt, "r1", timeout=180, save_config=True)
        reboot_device(rt, "r2", timeout=180, save_config=True)
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                    "regex": [rf"(?im)^\s*{re.escape(R2_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1 VRF session restored after reboot",
                },
                {
                    "device": "r2",
                    "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                    "regex": [rf"(?im)^\s*{re.escape(R1_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2 VRF session restored after reboot",
                },
            ],
            timeout=120,
            interval=3,
        )
        _wait_bgp_count(rt, COUNT, "r2 VRF BGP table after saved-config reboot")
        _wait_sample_routes_without_export_rt(rt, "after saved-config reboot")

        print("BGP VRF export-RT immediate delete, reboot, and static-batch churn check passed.", flush=True)
    finally:
        if should_skip_cleanup():
            return
        step("Cleanup BGP VRF export-RT 10k state")
        _cleanup(rt, base)
