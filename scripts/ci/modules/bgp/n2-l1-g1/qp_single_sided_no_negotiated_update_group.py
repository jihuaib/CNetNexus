#!/usr/bin/env python3
"""
BGP single-sided IPv4-QP negotiation guard (IPv4 transport).

Topology:
  r1 --- r2

Goal:
1. r1/r2 first establish normal IPv4 unicast BGP over IPv4 neighbors.
2. Only r2 enables `af ipv4-qp` for the same neighbor.
3. AF membership change triggers a normal session re-negotiation.
4. After re-establishment:
   - r2's `ipv4-qp` peer state must be `NoNegotiated`
   - neither r1 nor r2 may have any `ipv4-qp` update-group
"""

from __future__ import annotations

import re

from module_api import cmd, g_top, hold_check, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


AS_R1 = "65001"
AS_R2 = "65002"


def _cleanup(rt: TopologyRuntime) -> None:
    for dev in ("r1", "r2"):
        run_cmds(rt=rt, device=dev, strict=False, commands=["config", "no bgp", "end"])


def _peer_state_regex(peer_ip: str, state: str) -> str:
    return rf"(?im)^\s*{re.escape(peer_ip)}\s+\S+\s+\S+\s+{state}\s*$"


def _config_base(rt: TopologyRuntime, r1_peer_v4: str, r2_peer_v4: str) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"bgp {AS_R1}",
            "router-id 1.1.1.1",
            f"neighbor {r1_peer_v4} as {AS_R2}",
            "af ipv4-unicast",
            f"neighbor {r1_peer_v4} enable",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"bgp {AS_R2}",
            "router-id 2.2.2.2",
            f"neighbor {r2_peer_v4} as {AS_R1}",
            "af ipv4-unicast",
            f"neighbor {r2_peer_v4} enable",
            "exit",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)
    r2_peer_v4 = str(g_top.r2.GE_1.peer_ip)

    try:
        step("Configure baseline IPv4 unicast BGP on r1/r2")
        _config_base(rt, r1_peer_v4, r2_peer_v4)

        step("Wait baseline IPv4 unicast sessions established")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_peer_state_regex(r1_peer_v4, "Established")],
                    "label": "r1 ipv4-unicast Established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_peer_state_regex(r2_peer_v4, "Established")],
                    "label": "r2 ipv4-unicast Established",
                },
            ],
            timeout=40,
        )

        step("Enable ipv4-qp only on r2 for the IPv4 neighbor")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {AS_R2}",
                "af ipv4-qp",
                f"neighbor {r2_peer_v4} enable",
                "exit",
                "end",
            ],
        )

        step("Wait renegotiated IPv4 unicast session back to Established and QP peer to NoNegotiated")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_peer_state_regex(r1_peer_v4, "Established")],
                    "label": "r1 ipv4-unicast re-Established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_peer_state_regex(r2_peer_v4, "Established")],
                    "label": "r2 ipv4-unicast re-Established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-qp",
                    "contains": [r2_peer_v4],
                    "regex": [_peer_state_regex(r2_peer_v4, "NoNegotiated")],
                    "not_regex": [_peer_state_regex(r2_peer_v4, "Established")],
                    "label": "r2 ipv4-qp NoNegotiated",
                },
            ],
            timeout=60,
        )

        step("r1 must not have any ipv4-qp peer configured locally")
        r1_qp_neighbors = cmd(rt, "r1", "show bgp neighbor af ipv4-qp", strict=False)
        if "(no neighbors configured)" not in r1_qp_neighbors:
            raise RuntimeError(f"expected r1 to have no ipv4-qp peer configured, got:\n{r1_qp_neighbors}")

        step("Hold-check: no ipv4-qp update-group may appear on either side")
        hold_check(
            rt,
            device="r1",
            command="show bgp update-group af ipv4-qp",
            duration=10,
            interval=2,
            contains=["(no update-groups)"],
            label="r1 has no ipv4-qp update-group",
        )
        hold_check(
            rt,
            device="r2",
            command="show bgp update-group af ipv4-qp",
            duration=10,
            interval=2,
            contains=["(no update-groups)"],
            label="r2 has no ipv4-qp update-group",
        )

        step("Hold-check: r2 ipv4-qp peer must stay NoNegotiated")
        hold_check(
            rt,
            device="r2",
            command="show bgp neighbor af ipv4-qp",
            duration=10,
            interval=2,
            regex=[_peer_state_regex(r2_peer_v4, "NoNegotiated")],
            not_regex=[_peer_state_regex(r2_peer_v4, "Established")],
            label="r2 ipv4-qp remains NoNegotiated",
        )

        print("BGP single-sided ipv4-qp no-negotiated update-group check passed.")
    finally:
        step("Final cleanup")
        _cleanup(rt)


def main(rt: TopologyRuntime, top: dict[str, object]) -> None:
    run(rt, top)
