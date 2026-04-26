#!/usr/bin/env python3
"""
BGP neighbor guard regressions (IPv4 + IPv6).

Covers three recently-fixed issues:
  1. AS mismatch: peer OPEN 携带的 AS 与本端配置 remote-as 不一致必须拒绝，
     session 不得进入 Established。
  2. 单边未使能地址族：一端使能 af ipv4/ipv6-unicast，对端未使能；
     session 建起后该 AF 下 peer 状态必须为 NoNegotiated，不能是 Established。
  3. router-id 未配置：禁止建连；配上 router-id 后自动恢复到 Established。
"""

from __future__ import annotations

import re

from module_api import cmd, g_top, hold_check, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def _cleanup(rt: TopologyRuntime) -> None:
    for dev in ("r1", "r2"):
        run_cmds(rt=rt, device=dev, strict=False, commands=["config", "no bgp", "end"])


def _peer_af_regex(peer_ip: str, state: str) -> str:
    """'Neighbor Remote-AS Router-ID State' 行匹配；state 可为 Established / NoNegotiated / Idle / Active / ..."""
    return rf"(?im)^\s*{re.escape(peer_ip)}\s+\S+\s+\S+\s+{state}\s*$"


def _session_checks(r1_peer: str, r2_peer: str, af: str, state: str) -> list[dict[str, object]]:
    return [
        {
            "device": "r1",
            "command": f"show bgp neighbor af {af}",
            "contains": [r1_peer],
            "regex": [_peer_af_regex(r1_peer, state)],
            "label": f"r1->r2 {af} {state}",
        },
        {
            "device": "r2",
            "command": f"show bgp neighbor af {af}",
            "contains": [r2_peer],
            "regex": [_peer_af_regex(r2_peer, state)],
            "label": f"r2->r1 {af} {state}",
        },
    ]


def _config_base(rt: TopologyRuntime, *, r1_rid: str = "1.1.1.1", r2_rid: str = "2.2.2.2") -> None:
    run_cmds(rt=rt, device="r1", commands=["config", "bgp 65001", f"router-id {r1_rid}", "end"])
    run_cmds(rt=rt, device="r2", commands=["config", "bgp 65002", f"router-id {r2_rid}", "end"])


def _as_mismatch_case(rt: TopologyRuntime, r1_peer: str, r2_peer: str, af: str) -> None:
    step(f"[{af}] AS mismatch: r1 configures remote-as=65999 (真实=65002), session 必须不 Established")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            "bgp 65001",
            "timer connect-retry 3",
            f"neighbor {r1_peer} as 65999",  # wrong AS on purpose
            f"af {af}",
            f"neighbor {r1_peer} enable",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            "bgp 65002",
            "timer connect-retry 3",
            f"neighbor {r2_peer} as 65001",
            f"af {af}",
            f"neighbor {r2_peer} enable",
            "exit",
            "end",
        ],
    )

    step(f"[{af}] 等待约 15s，验证 session 一直无法 Established")
    hold_check(
        rt,
        device="r1",
        command=f"show bgp neighbor af {af}",
        duration=15,
        interval=3,
        not_regex=[_peer_af_regex(r1_peer, "Established")],
        label=f"r1 {af} must-not Established under AS mismatch",
    )
    hold_check(
        rt,
        device="r2",
        command=f"show bgp neighbor af {af}",
        duration=5,
        interval=3,
        not_regex=[_peer_af_regex(r2_peer, "Established")],
        label=f"r2 {af} must-not Established under AS mismatch",
    )

    step(f"[{af}] 直接修改 r1 AS -> 65002 应报错")
    outputs = run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "bgp 65001",
            f"neighbor {r1_peer} as 65002",
            "end",
        ],
    )
    if "remote-as cannot be modified in place" not in outputs[2]:
        raise AssertionError(f"{af}: expected direct remote-as change to be rejected, got:\n{outputs[2]}")

    step(f"[{af}] 删除 r1 错误邻居后按 AS 65002 重配，应恢复 Established")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            "bgp 65001",
            f"no neighbor {r1_peer}",
            f"neighbor {r1_peer} as 65002",
            f"af {af}",
            f"neighbor {r1_peer} enable",
            "exit",
            "end",
        ],
    )
    wait_checks(rt, _session_checks(r1_peer, r2_peer, af, "Established"), timeout=45)


def _single_sided_af_case(rt: TopologyRuntime, r1_peer: str, r2_peer: str, enabled_af: str, disabled_af: str) -> None:
    step(f"[single-sided] r1 使能 {disabled_af}，r2 只使能 {enabled_af}")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            "bgp 65001",
            f"neighbor {r1_peer} as 65002",
            f"af {enabled_af}",
            f"neighbor {r1_peer} enable",
            "exit",
            f"af {disabled_af}",
            f"neighbor {r1_peer} enable",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            "bgp 65002",
            f"neighbor {r2_peer} as 65001",
            f"af {enabled_af}",
            f"neighbor {r2_peer} enable",
            "exit",
            "end",
        ],
    )

    step(f"[single-sided] 双端 {enabled_af} 应 Established")
    wait_checks(rt, _session_checks(r1_peer, r2_peer, enabled_af, "Established"), timeout=45)

    step(f"[single-sided] r1 {disabled_af} peer 状态必须 NoNegotiated（对端未协商该 AF）")
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af {disabled_af}",
                "contains": [r1_peer],
                "regex": [_peer_af_regex(r1_peer, "NoNegotiated")],
                "not_regex": [_peer_af_regex(r1_peer, "Established")],
                "label": f"r1 {disabled_af} NoNegotiated",
            },
        ],
        timeout=20,
    )


def _router_id_gate_case(rt: TopologyRuntime, r1_peer: str, r2_peer: str, af: str) -> None:
    step(f"[{af}] r1 不配置 router-id, 应无法建连")
    run_cmds(rt=rt, device="r1", commands=["config", "bgp 65001", "no router-id", "end"])
    run_cmds(rt=rt, device="r2", commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            "bgp 65001",
            "timer connect-retry 3",
            f"neighbor {r1_peer} as 65002",
            f"af {af}",
            f"neighbor {r1_peer} enable",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            "bgp 65002",
            "timer connect-retry 3",
            f"neighbor {r2_peer} as 65001",
            f"af {af}",
            f"neighbor {r2_peer} enable",
            "exit",
            "end",
        ],
    )

    step(f"[{af}] 等待 15s 确认 r1 在缺少 router-id 时不会 Established")
    hold_check(
        rt,
        device="r1",
        command=f"show bgp neighbor af {af}",
        duration=15,
        interval=3,
        not_regex=[_peer_af_regex(r1_peer, "Established")],
        label=f"r1 {af} must-not Established without router-id",
    )

    step(f"[{af}] 配置 router-id 后应自动恢复 Established")
    run_cmds(rt=rt, device="r1", commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
    wait_checks(rt, _session_checks(r1_peer, r2_peer, af, "Established"), timeout=60)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)
    r2_peer_v4 = str(g_top.r2.GE_1.peer_ip)
    r1_peer_v6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_v6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        # -------- Case 1: AS mismatch (IPv4) --------
        _config_base(rt)
        _as_mismatch_case(rt, r1_peer_v4, r2_peer_v4, "ipv4-unicast")
        _cleanup(rt)

        # -------- Case 2: AS mismatch (IPv6) --------
        _config_base(rt)
        _as_mismatch_case(rt, r1_peer_v6, r2_peer_v6, "ipv6-unicast")
        _cleanup(rt)

        # -------- Case 3: single-sided AF over IPv4 peer (enable v4, extra v6) --------
        _config_base(rt)
        _single_sided_af_case(rt, r1_peer_v4, r2_peer_v4, "ipv4-unicast", "ipv6-unicast")
        _cleanup(rt)

        # -------- Case 4: single-sided AF over IPv6 peer (enable v6, extra v4) --------
        _config_base(rt)
        _single_sided_af_case(rt, r1_peer_v6, r2_peer_v6, "ipv6-unicast", "ipv4-unicast")
        _cleanup(rt)

        # -------- Case 5: router-id gate (IPv4) --------
        _router_id_gate_case(rt, r1_peer_v4, r2_peer_v4, "ipv4-unicast")
        _cleanup(rt)

        # -------- Case 6: router-id gate (IPv6) --------
        _router_id_gate_case(rt, r1_peer_v6, r2_peer_v6, "ipv6-unicast")
        _cleanup(rt)

        print("BGP neighbor guard check passed.")
    finally:
        step("Final cleanup")
        _cleanup(rt)


def main(rt: TopologyRuntime, top: dict[str, object]) -> None:
    run(rt, top)
