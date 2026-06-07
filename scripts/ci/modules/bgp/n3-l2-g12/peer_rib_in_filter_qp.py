#!/usr/bin/env python3
"""
BGP peer Adj-RIB-In filter check for IPv4-QP.

Topology:
  r1 --- r2 --- r3

Goal:
- r1 and r3 both advertise IPv4-QP routes to r2.
- `show bgp route af ipv4-qp peer X recieve-routes` on r2 shows only X's Adj-RIB-In.
- QP route-key filtering after `recieve-routes` matches only the requested NLRI.
"""

from __future__ import annotations

import re

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


AS_R1 = "65001"
AS_R2 = "65002"
AS_R3 = "65003"

R1_DQPN = 1100
R1_PFX_ADDR = "10.111.0.0"
R1_MASK = 24
R1_BID = "2001:db8:1111::1"

R3_DQPN = 3300
R3_PFX_ADDR = "10.133.0.0"
R3_MASK = 24
R3_BID = "2001:db8:3333::1"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _qp_key(dqpn: int, addr: str, mask: int) -> str:
    return f"dqpn={dqpn},ip={addr}/{mask}"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup BGP config")
    for dev in ("r1", "r2", "r3"):
        run_cmds(rt=rt, device=dev, strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)       # r2 GE-1
    r2_to_r1_v4 = str(g_top.r2.GE_1.peer_ip)      # r1 GE-1
    r2_to_r3_v4 = str(g_top.r2.GE_2.peer_ip)      # r3 GE-1
    r3_peer_v4 = str(g_top.r3.GE_1.peer_ip)       # r2 GE-2

    r1_key = _qp_key(R1_DQPN, R1_PFX_ADDR, R1_MASK)
    r3_key = _qp_key(R3_DQPN, R3_PFX_ADDR, R3_MASK)

    try:
        step("Configure IPv4-QP BGP base")
        run_cmds(rt=rt, device="r1", commands=["config", f"bgp {AS_R1}", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", commands=["config", f"bgp {AS_R2}", "router-id 2.2.2.2", "end"])
        run_cmds(rt=rt, device="r3", commands=["config", f"bgp {AS_R3}", "router-id 3.3.3.3", "end"])

        step("Configure r1/r3 as two QP route sources toward r2")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {AS_R1}",
                f"neighbor {r1_peer_v4} as {AS_R2}",
                "af ipv4-qp",
                f"neighbor {r1_peer_v4} enable",
                "route-select enable",
                f"route start-dqpn {R1_DQPN} ip {R1_PFX_ADDR} mask {R1_MASK} count 1 bid {R1_BID}",
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
                f"neighbor {r2_to_r1_v4} as {AS_R1}",
                f"neighbor {r2_to_r3_v4} as {AS_R3}",
                "af ipv4-qp",
                f"neighbor {r2_to_r1_v4} enable",
                f"neighbor {r2_to_r3_v4} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r3",
            commands=[
                "config",
                f"bgp {AS_R3}",
                f"neighbor {r3_peer_v4} as {AS_R2}",
                "af ipv4-qp",
                f"neighbor {r3_peer_v4} enable",
                "route-select enable",
                f"route start-dqpn {R3_DQPN} ip {R3_PFX_ADDR} mask {R3_MASK} count 1 bid {R3_BID}",
                "exit",
                "end",
            ],
        )

        step("Wait r2 has both QP peers established")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-qp",
                    "regex": [_established_regex(r2_to_r1_v4), _established_regex(r2_to_r3_v4)],
                    "label": "r2 both QP peers Established",
                },
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-qp",
                    "regex": [_established_regex(r1_peer_v4)],
                    "label": "r1 QP peer Established",
                },
                {
                    "device": "r3",
                    "command": "show bgp neighbor af ipv4-qp",
                    "regex": [_established_regex(r3_peer_v4)],
                    "label": "r3 QP peer Established",
                },
            ],
            timeout=50,
        )

        step("Wait r2 receives both QP routes in its BGP RIB")
        wait_check(
            rt,
            device="r2",
            command="show bgp route af ipv4-qp",
            contains=[f"dqpn={R1_DQPN}", f"{R1_PFX_ADDR}/{R1_MASK}", f"dqpn={R3_DQPN}", f"{R3_PFX_ADDR}/{R3_MASK}"],
            timeout=40,
            label="r2 global QP RIB has both sources",
        )

        step("Verify peer Adj-RIB-In filtering separates r1 and r3 routes")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show bgp route af ipv4-qp peer {r2_to_r1_v4} recieve-routes",
                    "contains": [f"dqpn={R1_DQPN}", f"{R1_PFX_ADDR}/{R1_MASK}"],
                    "not_contains": [f"dqpn={R3_DQPN}", f"{R3_PFX_ADDR}/{R3_MASK}"],
                    "label": "r2 Adj-RIB-In from r1 only",
                },
                {
                    "device": "r2",
                    "command": f"show bgp route af ipv4-qp peer {r2_to_r3_v4} recieve-routes",
                    "contains": [f"dqpn={R3_DQPN}", f"{R3_PFX_ADDR}/{R3_MASK}"],
                    "not_contains": [f"dqpn={R1_DQPN}", f"{R1_PFX_ADDR}/{R1_MASK}"],
                    "label": "r2 Adj-RIB-In from r3 only",
                },
            ],
            timeout=20,
        )

        step("Verify QP route-key filter after recieve-routes")
        r1_filtered = cmd(rt, "r2", f"show bgp route af ipv4-qp peer {r2_to_r1_v4} recieve-routes {r1_key}", strict=False)
        if f"dqpn={R1_DQPN}" not in r1_filtered or f"{R1_PFX_ADDR}/{R1_MASK}" not in r1_filtered:
            raise RuntimeError(f"r1 peer RIB-In QP key filter missed expected route:\n{r1_filtered}")
        if f"dqpn={R3_DQPN}" in r1_filtered or f"{R3_PFX_ADDR}/{R3_MASK}" in r1_filtered:
            raise RuntimeError(f"r1 peer RIB-In QP key filter leaked r3 route:\n{r1_filtered}")

        wrong_filtered = cmd(rt, "r2", f"show bgp route af ipv4-qp peer {r2_to_r1_v4} receive-routes {r3_key}", strict=False)
        if "(no received routes)" not in wrong_filtered or "Total: 0 received routes" not in wrong_filtered:
            raise RuntimeError(f"wrong QP key unexpectedly matched r1 peer RIB-In:\n{wrong_filtered}")

        print("BGP peer Adj-RIB-In QP peer filter check passed.")
    finally:
        _cleanup(rt)


def main(rt: TopologyRuntime, top: dict[str, object]) -> None:
    run(rt, top)
