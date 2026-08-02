#!/usr/bin/env python3
"""
ISIS cost-style (narrow / wide) verification.

Coverage:
- Default cost-style is narrow; reflected in `show isis summary ipv4` output
- Under narrow:
    * IPv4 metric > 63 rejected (cli surfaces apply errmsg)
    * Enabling `af ipv6` rejected
    * IPv6 interface enable rejected
    * Metric <= 63 accepted; SPF/LSP uses RFC 1195 TLV 2 + TLV 128
- Switching narrow -> wide allowed; then IPv4 metric > 63 OK, IPv6 OK
- Wide cost-style emits TLV 22 + TLV 135 + TLV 236
- Switching wide -> narrow rejected when IPv4 metric > 63 or IPv6 enabled
- After lowering metric and disabling IPv6, wide -> narrow succeeds
- Adapts to the new show command order: `show isis <topic> ipv4|ipv6 <tag>`
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    g_top,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime, execCmd  # noqa: E402


TAG = 200
GE_IF = "GE-1"

R1_NET = "49.0002.0000.0000.0001.00"
R2_NET = "49.0002.0000.0000.0002.00"

R1_LOOP_ID = 41
R2_LOOP_ID = 42

R1_LOOP_V4 = "10.255.41.1"
R2_LOOP_V4 = "10.255.42.2"
R1_LOOP_V4_LEN = 32
R2_LOOP_V4_LEN = 32

R1_LOOP_V6 = "2001:db8:255:41::1"
R2_LOOP_V6 = "2001:db8:255:42::2"
R1_LOOP_V6_LEN = 128
R2_LOOP_V6_LEN = 128

R1_LOOP_V4_PREFIX = f"{R1_LOOP_V4}/{R1_LOOP_V4_LEN}"
R2_LOOP_V4_PREFIX = f"{R2_LOOP_V4}/{R2_LOOP_V4_LEN}"
R1_LOOP_V6_PREFIX = f"{R1_LOOP_V6}/{R1_LOOP_V6_LEN}"
R2_LOOP_V6_PREFIX = f"{R2_LOOP_V6}/{R2_LOOP_V6_LEN}"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup cost-style case config")
    for dev, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"no isis {TAG}",
                f"no if loop {loop_id}",
                f"if {GE_IF}",
                "no shutdown",
                "exit",
                "end",
            ],
        )


def _expect_apply_error(
    rt: TopologyRuntime,
    device: str,
    *,
    navigate: list[str],
    failing: str,
    expect_tokens: list[str],
    label: str,
) -> None:
    """Run navigation commands (must succeed), then send a single command expected to fail.

    After the failing command, always issue 'end' to return to top-level view.
    """
    sess = execCmd(rt, device)
    for nav in navigate:
        sess.exec(nav, strict=True, timeout=15)
    out = sess.exec(failing, strict=False, timeout=15)
    # Always return to top to keep CLI session clean for subsequent steps
    sess.exec("end", strict=False, timeout=15)
    missing = [tok for tok in expect_tokens if tok not in out]
    if missing:
        raise RuntimeError(
            f"[{label}] expected apply rejection containing {missing!r}\n"
            f"command: {failing}\n"
            f"output:\n{out}"
        )


def _exec_show(rt: TopologyRuntime, device: str, command: str) -> str:
    return execCmd(rt, device).exec(command, strict=False, timeout=15)


def _seed_loopback(rt: TopologyRuntime) -> None:
    step("Configure loopback prefixes on both routers")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if loop {R1_LOOP_ID}",
            f"ip address {R1_LOOP_V4} {R1_LOOP_V4_LEN}",
            f"ipv6 address {R1_LOOP_V6} {R1_LOOP_V6_LEN}",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"if loop {R2_LOOP_ID}",
            f"ip address {R2_LOOP_V4} {R2_LOOP_V4_LEN}",
            f"ipv6 address {R2_LOOP_V6} {R2_LOOP_V6_LEN}",
            "exit",
            "end",
        ],
    )


def _seed_isis_instance(rt: TopologyRuntime) -> None:
    step("Configure ISIS instance + IPv4 AF on both routers (default cost-style=narrow)")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"isis {TAG}",
            f"net {R1_NET}",
            "is-type level-1-2",
            "af ipv4",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"isis {TAG}",
            f"net {R2_NET}",
            "is-type level-1-2",
            "af ipv4",
            "end",
        ],
    )


def _verify_default_narrow_summary(rt: TopologyRuntime) -> None:
    step("Verify default cost-style is narrow in summary")
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis summary ipv4 {TAG}",
                "contains": ["ISIS Summary"],
                "regex": [
                    rf"(?im)^\s*{TAG}\s+public\s+level-1-2\s+narrow\b",
                ],
                "label": "r1 default narrow in summary",
            },
            {
                "device": "r2",
                "command": f"show isis summary ipv4 {TAG}",
                "contains": ["ISIS Summary"],
                "regex": [
                    rf"(?im)^\s*{TAG}\s+public\s+level-1-2\s+narrow\b",
                ],
                "label": "r2 default narrow in summary",
            },
        ],
        timeout=20,
        interval=2,
    )


def _enable_isis_on_interfaces(rt: TopologyRuntime, *, metric: int) -> None:
    step(f"Enable ISIS IPv4 on GE-1 + loopback (metric={metric}) on both routers")
    for dev, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=dev,
            commands=[
                "config",
                f"if {GE_IF}",
                f"isis enable {TAG}",
                f"isis metric {TAG} {metric}",
                f"isis hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                "exit",
                f"if loop {loop_id}",
                f"isis enable {TAG}",
                f"isis passive {TAG}",
                "exit",
                "end",
            ],
        )


def _wait_isis_adj_up(rt: TopologyRuntime) -> None:
    step("Wait ISIS IPv4 adjacency up")
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis neighbor {TAG}",
                "contains": ["ISIS Neighbors", GE_IF],
                "regex": [
                    rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\b",
                ],
                "label": "r1 isis adj up",
            },
            {
                "device": "r2",
                "command": f"show isis neighbor {TAG}",
                "contains": ["ISIS Neighbors", GE_IF],
                "regex": [
                    rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\b",
                ],
                "label": "r2 isis adj up",
            },
        ],
        timeout=80,
        interval=2,
    )


def _verify_narrow_tlvs(rt: TopologyRuntime) -> None:
    step("Verify LSDB carries RFC 1195 narrow TLV 2 / TLV 128 under narrow cost-style")
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis lsdb ipv4 {TAG}",
                "contains": ["ISIS LSDB"],
                "not_contains": ["(no entries)"],
                "regex": [
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=2\b",
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=128\b",
                ],
                "not_regex": [
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=135\b",
                ],
                "label": "r1 lsdb has narrow TLVs only",
            },
            {
                "device": "r2",
                "command": f"show isis lsdb ipv4 {TAG}",
                "contains": ["ISIS LSDB"],
                "not_contains": ["(no entries)"],
                "regex": [
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=2\b",
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=128\b",
                ],
                "not_regex": [
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=135\b",
                ],
                "label": "r2 lsdb has narrow TLVs only",
            },
        ],
        timeout=80,
        interval=2,
    )


def _verify_wide_tlvs(rt: TopologyRuntime) -> None:
    step("Verify LSDB carries RFC 5305 wide TLV 22 / TLV 135 (+ 236 when IPv6) under wide")
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis lsdb ipv4 {TAG}",
                "contains": ["ISIS LSDB"],
                "not_contains": ["(no entries)"],
                "regex": [
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=135\b",
                ],
                "not_regex": [
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=2\b",
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=128\b",
                ],
                "label": "r1 lsdb has wide TLVs only",
            },
            {
                "device": "r2",
                "command": f"show isis lsdb ipv4 {TAG}",
                "contains": ["ISIS LSDB"],
                "not_contains": ["(no entries)"],
                "regex": [
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                    r"(?im)^\s*TLV\[\d+\]\s*:\s*type=135\b",
                ],
                "label": "r2 lsdb has wide TLVs",
            },
        ],
        timeout=80,
        interval=2,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    try:
        _cleanup(rt)
        _seed_loopback(rt)
        _seed_isis_instance(rt)
        _verify_default_narrow_summary(rt)

        # --- Negative cases under narrow ---
        step("Negative: under narrow, configuring IPv4 metric > 63 must be rejected")
        # First enable ISIS on GE-1 with valid metric so the interface block has v4.enabled=1
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {GE_IF}",
                f"isis enable {TAG}",
                f"isis metric {TAG} 10",
                "exit",
                "end",
            ],
        )
        _expect_apply_error(
            rt,
            device="r1",
            navigate=["config", f"if {GE_IF}"],
            failing=f"isis metric {TAG} 100",
            expect_tokens=["ISIS Error", "exceeds narrow max", "cost-style wide"],
            label="narrow rejects metric=100",
        )

        step("Negative: under narrow, enabling 'af ipv6' must be rejected")
        _expect_apply_error(
            rt,
            device="r1",
            navigate=["config", f"isis {TAG}"],
            failing="af ipv6",
            expect_tokens=["ISIS Error", "IPv6 AF requires", "cost-style wide"],
            label="narrow rejects af ipv6",
        )

        step("Negative: under narrow, enabling IPv6 on an interface must be rejected")
        _expect_apply_error(
            rt,
            device="r1",
            navigate=["config", f"if {GE_IF}"],
            failing=f"isis ipv6 enable {TAG}",
            expect_tokens=["ISIS Error"],
            label="narrow rejects isis ipv6 enable",
        )

        # --- Positive: narrow happy path, bring up adjacency + verify TLV types ---
        _enable_isis_on_interfaces(rt, metric=10)
        _wait_isis_adj_up(rt)

        step("Verify remote IPv4 loopback learned via narrow ISIS")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis route ipv4 {TAG}",
                    "contains": [f"ISIS ipv4 Routes (tag {TAG})", R2_LOOP_V4_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r1 narrow learned r2 loop",
                },
                {
                    "device": "r2",
                    "command": f"show isis route ipv4 {TAG}",
                    "contains": [f"ISIS ipv4 Routes (tag {TAG})", R1_LOOP_V4_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r2 narrow learned r1 loop",
                },
            ],
            timeout=80,
            interval=2,
        )
        _verify_narrow_tlvs(rt)

        # --- Switch to wide on both routers ---
        step("Switch cost-style to wide on both routers")
        run_cmds(
            rt=rt,
            device="r1",
            commands=["config", f"isis {TAG}", "cost-style wide", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=["config", f"isis {TAG}", "cost-style wide", "end"],
        )

        step("Verify summary reflects wide cost-style")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis summary ipv4 {TAG}",
                    "regex": [rf"(?im)^\s*{TAG}\s+public\s+level-1-2\s+wide\b"],
                    "label": "r1 wide in summary",
                },
                {
                    "device": "r2",
                    "command": f"show isis summary ipv4 {TAG}",
                    "regex": [rf"(?im)^\s*{TAG}\s+public\s+level-1-2\s+wide\b"],
                    "label": "r2 wide in summary",
                },
            ],
            timeout=20,
            interval=2,
        )

        step("Positive: under wide, IPv4 metric > 63 must succeed")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {GE_IF}",
                f"isis metric {TAG} 1000",
                "exit",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            timeout=15,
            interval=2,
            contains=[f"isis metric {TAG} 1000"],
            label="r1 wide accepts metric=1000",
        )

        step("Positive: under wide, enabling af ipv6 and ipv6 interface must succeed")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"isis {TAG}",
                "af ipv6",
                "exit",
                "exit",
                f"if {GE_IF}",
                f"isis ipv6 enable {TAG}",
                "exit",
                f"if loop {R1_LOOP_ID}",
                f"isis ipv6 enable {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"isis {TAG}",
                "af ipv6",
                "exit",
                "exit",
                f"if {GE_IF}",
                f"isis ipv6 enable {TAG}",
                "exit",
                f"if loop {R2_LOOP_ID}",
                f"isis ipv6 enable {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )

        step("Verify wide LSDB has TLV 22 / 135 (and 236 since IPv6 active)")
        _verify_wide_tlvs(rt)

        wait_check(
            rt,
            device="r1",
            command=f"show isis lsdb ipv4 {TAG}",
            timeout=80,
            interval=2,
            regex=[r"(?im)^\s*TLV\[\d+\]\s*:\s*type=236\b"],
            label="r1 wide lsdb has TLV 236 once IPv6 enabled",
        )

        step("Verify IPv6 learned route present (wide only)")
        wait_check(
            rt,
            device="r1",
            command=f"show isis route ipv6 {TAG}",
            timeout=80,
            interval=2,
            contains=[f"ISIS ipv6 Routes (tag {TAG})", R2_LOOP_V6_PREFIX],
            not_contains=["(no routes)"],
            label="r1 wide learned r2 ipv6 loop",
        )

        # --- Switch back to narrow with violation, expect rejection ---
        # apply 先检查 af_ipv6（narrow 不支持），再检查接口 metric。
        # 所以第一步：IPv6 还启用时切 narrow，应被 IPv6-blocking 消息拒绝。
        step("Negative: wide -> narrow while IPv6 still enabled must be rejected")
        _expect_apply_error(
            rt,
            device="r1",
            navigate=["config", f"isis {TAG}"],
            failing="cost-style narrow",
            expect_tokens=["ISIS Error", "af ipv6"],
            label="wide->narrow rejected: IPv6 still enabled",
        )

        step("Disable IPv6 on r1; metric=1000 still violates narrow")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {GE_IF}",
                f"no isis ipv6 enable {TAG}",
                "exit",
                f"if loop {R1_LOOP_ID}",
                f"no isis ipv6 enable {TAG}",
                "exit",
                f"isis {TAG}",
                "no af ipv6",
                "end",
            ],
        )
        _expect_apply_error(
            rt,
            device="r1",
            navigate=["config", f"isis {TAG}"],
            failing="cost-style narrow",
            expect_tokens=["ISIS Error", "metric 1000", "exceeds narrow max"],
            label="wide->narrow rejected: GE-1 metric 1000",
        )

        step("Lower interface metric to 10, then cost-style narrow should succeed")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {GE_IF}",
                f"isis metric {TAG} 10",
                "exit",
                f"isis {TAG}",
                "cost-style narrow",
                "end",
            ],
        )

        wait_check(
            rt,
            device="r1",
            command=f"show isis summary ipv4 {TAG}",
            timeout=20,
            interval=2,
            regex=[rf"(?im)^\s*{TAG}\s+public\s+level-1-2\s+narrow\b"],
            label="r1 narrow restored",
        )

        print("ISIS cost-style verification passed.")
    finally:
        _cleanup(rt)
