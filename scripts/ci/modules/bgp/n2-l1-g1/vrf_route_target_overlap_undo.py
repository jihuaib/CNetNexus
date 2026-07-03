#!/usr/bin/env python3
"""
Verify CE-PE VRF unicast route-origin RT survives local export-RT undo.

Scenario:
- r1 and r2 run a VRF ipv4-unicast eBGP session.
- r1 imports a static route into BGP while its VRF export RT is 1:1, so the
  route itself is advertised to r2 with Ext-Comm rt:1:1.
- r2 initially has no local VRF export RT. After the received route already
  exists, r2 configures multiple export RTs including 1:1, creating an overlap
  between route-origin RT and local VRF export RT.
- After undoing only some of r2's export RTs, including 1:1, r2's received
  route must still carry Ext-Comm rt:1:1 because that RT belongs to the
  received route.
"""

from __future__ import annotations

import re
import time

from module_api import g_top, require_devices, run_cmds, should_skip_cleanup, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
GE_IF = "GE-1"

R1_V4 = "10.98.0.1"
R2_V4 = "10.98.0.2"
V4_LEN = 30

R1_AS = 65101
R2_AS = 65102

R1_RD = "65101:98"
R2_RD = "65102:98"
RT = "1:1"
RT_TEXT = "rt:1:1"
R2_EXPORT_RTS = ["1:1", "1:2", "1:3", "1:4"]
R2_UNDO_EXPORT_RTS = ["1:1", "1:3"]
R2_REMAIN_EXPORT_RTS = ["1:2", "1:4"]

PREFIX_ADDR = "10.251.1.0"
PREFIX_LEN = 24
PREFIX = f"{PREFIX_ADDR}/{PREFIX_LEN}"
STATIC_NH = R2_V4


def _cleanup(rt: TopologyRuntime, base: dict[str, str | int]) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no route static ipv4 vrf {VRF_NAME} {PREFIX_ADDR} {PREFIX_LEN} {STATIC_NH}",
            "no bgp",
            f"if {GE_IF}",
            "no shutdown",
            f"no ip address {R1_V4} {V4_LEN}",
            "no vrf forwarding",
            f"ip address {base['r1_v4']} {base['r1_v4_len']}",
            f"ipv6 address {base['r1_v6']} {base['r1_v6_len']}",
            "exit",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"if {GE_IF}",
            "no shutdown",
            f"no ip address {R2_V4} {V4_LEN}",
            "no vrf forwarding",
            f"ip address {base['r2_v4']} {base['r2_v4_len']}",
            f"ipv6 address {base['r2_v6']} {base['r2_v6_len']}",
            "exit",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )


def _setup_vrf_and_if(rt: TopologyRuntime, device: str, local_ip: str, rd: str, *, export_rt: bool) -> None:
    commands = [
        "config",
        f"vrf {VRF_NAME}",
        "af ipv4",
        f"route-distinguisher {rd}",
    ]
    if export_rt:
        commands.append(f"vpn-target {RT} export")
    commands.extend(
        [
            "exit",
            "exit",
            f"if {GE_IF}",
            "no shutdown",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {local_ip} {V4_LEN}",
            "exit",
            "end",
        ]
    )
    run_cmds(rt=rt, device=device, commands=commands)


def _setup_bgp(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
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


def _session_check(device: str, peer: str, label: str) -> dict[str, object]:
    return {
        "device": device,
        "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
        "contains": [peer, "AF: ipv4-unicast"],
        "regex": [rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"],
        "label": label,
    }


def _r2_route_has_rt(label: str) -> dict[str, object]:
    return {
        "device": "r2",
        "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} {PREFIX_ADDR} {PREFIX_LEN}",
        "contains": [
            f"BGP Route Detail: {PREFIX}",
            f"From Peer  : {R1_V4}",
            f"Ext-Comm : {RT_TEXT}",
        ],
        "regex": [rf"(?im)^\s*AS-Path\s*:\s*{R1_AS}\s*$"],
        "label": label,
    }


def _r1_advertise_route(label: str) -> dict[str, object]:
    return {
        "device": "r1",
        "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} peer {R2_V4} advertise-routes {PREFIX_ADDR} {PREFIX_LEN}",
        "contains": [
            f"BGP Peer Adj-RIB-Out (AF: ipv4-unicast, VRF: {VRF_NAME})",
            f"Peer   : {R2_V4}",
            PREFIX,
        ],
        "regex": [r"(?im)^Total:\s*1\s+advertised routes\s*$"],
        "label": label,
    }


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
        _cleanup(rt, base)

        step("Create CE-PE VRFs; only r1 has export RT 1:1 initially")
        _setup_vrf_and_if(rt, "r1", R1_V4, R1_RD, export_rt=True)
        _setup_vrf_and_if(rt, "r2", R2_V4, R2_RD, export_rt=False)
        time.sleep(2)

        step("Bring up VRF ipv4-unicast eBGP and advertise a static route from r1")
        _setup_bgp(rt)
        wait_checks(
            rt,
            [
                _session_check("r1", R2_V4, "r1 VRF session established"),
                _session_check("r2", R1_V4, "r2 VRF session established"),
            ],
            timeout=60,
        )
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv4 vrf {VRF_NAME} {PREFIX_ADDR} {PREFIX_LEN} {STATIC_NH}",
                "end",
            ],
        )

        step("Verify r2 received CE route carries route-origin rt:1:1 before local export RT exists")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show vrf name {VRF_NAME}",
                    "not_contains": ["Export-RT"],
                    "label": "r2 has no local export RT 1:1 before add",
                },
                _r2_route_has_rt("r2 received route has route-origin rt:1:1 before local export add"),
                _r1_advertise_route("r1 advertise-routes supports explicit VRF and IPv4 prefix filter"),
            ],
            timeout=40,
        )

        step("Configure multiple r2 local VRF export RTs after the route already exists")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"vrf {VRF_NAME}",
                "af ipv4",
                *[f"vpn-target {rt_value} export" for rt_value in R2_EXPORT_RTS],
                "exit",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show vrf name {VRF_NAME}",
                    "contains": ["Export-RT", *R2_EXPORT_RTS],
                    "label": "r2 local export RTs added after route exists",
                },
                _r2_route_has_rt("r2 received route still has rt:1:1 after local export adds"),
            ],
            timeout=30,
        )

        step("Undo some r2 local VRF export RTs, including overlapping RT 1:1")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"vrf {VRF_NAME}",
                "af ipv4",
                *[f"no vpn-target {rt_value} export" for rt_value in R2_UNDO_EXPORT_RTS],
                "exit",
                "exit",
                "end",
            ],
        )

        step("Verify original route RT is still present after local export undo")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show vrf name {VRF_NAME}",
                    "contains": ["Export-RT", *R2_REMAIN_EXPORT_RTS],
                    "not_contains": R2_UNDO_EXPORT_RTS,
                    "label": "r2 removed selected local export RTs and kept the rest",
                },
                _r2_route_has_rt("r2 received route keeps route-origin rt:1:1 after local export undo"),
            ],
            timeout=30,
        )

        print("BGP CE-PE route-target overlap undo check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt, base)


def main(rt: TopologyRuntime, top: dict[str, object]) -> None:
    run(rt, top)
