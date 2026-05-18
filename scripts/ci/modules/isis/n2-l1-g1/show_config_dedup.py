#!/usr/bin/env python3
"""
show current-configuration 去重验证 (Config Anchor 框架)

覆盖场景:
- 同一接口既由 IF 模块配置了 IP, 又被 ISIS 模块 enable
- 聚合后每个接口只出现一次, 且 IP 与 isis enable 行都在同一个 "if <name>" 块内
- loopback 接口同样只出现一次, 格式为 "if loop <N>"
- "isis <tag>" 顶层块仍保留
- 不存在孤儿的 "\\x01" 标志字节(聚合器泄漏保护)

不使用 AF-specific isis 命令验证, 仅检查 show current-configuration 的结构。
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check  # noqa: E402
from top_runner import TopologyRuntime, execCmd  # noqa: E402


TAG = 101
GE_IF = "GE-1"
LOOP_ID = 31


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup show_config_dedup case config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no isis {TAG}",
            f"no if loop {LOOP_ID}",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )


def _split_blocks(config_text: str) -> list[list[str]]:
    """按 '!' 分隔符将 show current-configuration 输出拆分为逻辑块列表, 每块为行列表(已去掉 '!' 行)。"""
    blocks: list[list[str]] = []
    cur: list[str] = []
    for raw in config_text.splitlines():
        line = raw.rstrip()
        if line.strip() == "!":
            if cur:
                blocks.append(cur)
                cur = []
            continue
        if line.strip() == "":
            continue
        cur.append(line)
    if cur:
        blocks.append(cur)
    return blocks


def _collect_iface_blocks(config_text: str) -> tuple[dict[str, list[str]], dict[str, int]]:
    """
    扫描 'if <name>' / 'if loop <N>' 块。

    返回 (merged_bodies, occurrences):
      - merged_bodies: {接口名: 全部出现块的子行合并列表}
      - occurrences:   {接口名: 出现次数}
    """
    raw: dict[str, list[list[str]]] = {}
    for block in _split_blocks(config_text):
        if not block:
            continue
        head = block[0].strip()
        m = re.match(r"^if\s+loop\s+(\d+)\s*$", head)
        if m:
            name = f"loop{m.group(1)}"
        else:
            m = re.match(r"^if\s+(\S+)\s*$", head)
            if not m:
                continue
            name = m.group(1)
        raw.setdefault(name, []).append(list(block[1:]))

    merged: dict[str, list[str]] = {}
    occurrences: dict[str, int] = {}
    for name, groups in raw.items():
        flat: list[str] = []
        for g in groups:
            flat.extend(g)
        merged[name] = flat
        occurrences[name] = len(groups)
    return merged, occurrences


def _assert_dedup(device: str, config_text: str, expected_ifaces: dict[str, dict[str, object]]) -> None:
    """
    expected_ifaces: { ifname: {"contains": [...], "min_body": int} }
    """
    # 聚合器泄漏保护: 输出中不应含 \x01
    if "\x01" in config_text:
        raise RuntimeError(f"{device}: output contains raw \\x01 anchor delimiter, aggregator leaked markers")

    bodies, occurrences = _collect_iface_blocks(config_text)

    violations: list[str] = []
    for name, spec in expected_ifaces.items():
        got_count = occurrences.get(name, 0)
        if got_count != 1:
            violations.append(f"{device}: interface '{name}' appeared {got_count} time(s), want 1")
            continue

        body = bodies.get(name, [])
        tokens = list(spec.get("contains", []))  # type: ignore[arg-type]
        for tok in tokens:
            if not any(tok in ln for ln in body):
                violations.append(
                    f"{device}: interface '{name}' block missing '{tok}'; got body:\n  " + "\n  ".join(body)
                )

    if violations:
        raise RuntimeError("show_config_dedup violations:\n" + "\n".join(violations))


def _show_config(rt: TopologyRuntime, device: str) -> str:
    return execCmd(rt, device).exec("show current-configuration", strict=False, timeout=30)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))

    r1_ip4 = str(g_top.r1.GE_1.ip)
    r1_prefix4 = int(g_top.r1.GE_1.prefix)
    loop_v4 = f"10.255.{LOOP_ID}.1"
    loop_v4_len = 32
    loop_v6 = f"2001:db8:255:{LOOP_ID}::1"
    loop_v6_len = 128

    try:
        _cleanup(rt)

        step("Seed baseline: GE-1 IP already present from topology setup")
        wait_check(
            rt,
            device="r1",
            command=f"show if {GE_IF}",
            timeout=20,
            interval=2,
            contains=[
                f"Interface {GE_IF} Detail:",
                "State      : UP",
                f"IPv4 Addr  : {r1_ip4}/{r1_prefix4}",
            ],
            label="r1 GE-1 baseline up",
        )

        step("Configure loopback with dual-stack IP")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {LOOP_ID}",
                f"ip address {loop_v4} {loop_v4_len}",
                f"ipv6 address {loop_v6} {loop_v6_len}",
                "exit",
                "end",
            ],
        )

        step("Configure ISIS instance + enable on GE-1 and loopback")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"isis {TAG}",
                "net 49.0001.0000.0000.0f01.00",
                "is-type level-2",
                "cost-style wide",
                "af ipv4",
                "af ipv6",
                "end",
                "config",
                f"if {GE_IF}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis metric {TAG} 77",
                "exit",
                f"if loop {LOOP_ID}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis passive {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )

        step("Wait aggregated show current-configuration to reflect ISIS + IF config")
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            timeout=15,
            interval=2,
            contains=[
                f"isis {TAG}",
                "net 49.0001.0000.0000.0f01.00",
                f"if loop {LOOP_ID}",
                f"isis enable {TAG}",
                f"isis metric {TAG} 77",
            ],
            label="r1 show current-configuration reflects config",
        )

        step("Collect show current-configuration and assert dedup")
        cfg = _show_config(rt, "r1")
        _assert_dedup(
            "r1",
            cfg,
            {
                GE_IF: {
                    "contains": [
                        f"ip address {r1_ip4} {r1_prefix4}",
                        f"isis enable {TAG}",
                        f"isis ipv6 enable {TAG}",
                        f"isis metric {TAG} 77",
                    ],
                },
                f"loop{LOOP_ID}": {
                    "contains": [
                        f"ip address {loop_v4} {loop_v4_len}",
                        f"ipv6 address {loop_v6} {loop_v6_len}",
                        f"isis enable {TAG}",
                        f"isis ipv6 enable {TAG}",
                        f"isis passive {TAG}",
                        f"isis ipv6 passive {TAG}",
                    ],
                },
            },
        )

        step("Sanity: top-level isis instance block retained exactly once")
        isis_heads = re.findall(rf"(?m)^isis\s+{TAG}\s*$", cfg)
        if len(isis_heads) != 1:
            raise RuntimeError(
                f"r1: top-level 'isis {TAG}' appeared {len(isis_heads)} time(s), want 1\n"
                f"full config:\n{cfg}"
            )

        step("Negative case: every interface in output appears at most once")
        _, occurrences = _collect_iface_blocks(cfg)
        for name, count in occurrences.items():
            if count != 1:
                raise RuntimeError(
                    f"r1: interface '{name}' appeared {count} time(s) in aggregated output, want 1\n"
                    f"full config:\n{cfg}"
                )

        step("Idempotency: replay collected config should produce identical output")
        # 简单回放式用例: 先清理, 再按原顺序配置, 比对关键三元组(ip address / isis enable / 出现次数)
        _cleanup(rt)
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"isis {TAG}",
                "net 49.0001.0000.0000.0f01.00",
                "is-type level-2",
                "cost-style wide",
                "af ipv4",
                "af ipv6",
                "end",
                "config",
                f"if loop {LOOP_ID}",
                f"ip address {loop_v4} {loop_v4_len}",
                f"ipv6 address {loop_v6} {loop_v6_len}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis passive {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                f"if {GE_IF}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis metric {TAG} 77",
                "exit",
                "end",
            ],
        )
        cfg2 = _show_config(rt, "r1")
        _assert_dedup(
            "r1",
            cfg2,
            {
                GE_IF: {
                    "contains": [
                        f"ip address {r1_ip4} {r1_prefix4}",
                        f"isis enable {TAG}",
                        f"isis metric {TAG} 77",
                    ],
                },
                f"loop{LOOP_ID}": {
                    "contains": [
                        f"ip address {loop_v4} {loop_v4_len}",
                        f"isis enable {TAG}",
                        f"isis passive {TAG}",
                    ],
                },
            },
        )

        print("show_config_dedup: passed.")
    finally:
        _cleanup(rt)
