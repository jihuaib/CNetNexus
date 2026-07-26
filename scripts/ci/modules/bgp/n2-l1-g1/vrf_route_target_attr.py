#!/usr/bin/env python3
"""
BGP VRF route-target extended-community validation.

Covers:
- A PE-local VRF route imported with import-route static gets the VRF export RT.
- A CE-learned VRF BGP route gets the local PE VRF export RT after Adj-RIB-In
  processing, even when the CE side did not attach an RT.
"""

from __future__ import annotations

import re
import time

from module_api import (  # noqa: E402
    cmd,
    g_top,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
GE_IF = "GE-1"

R1_V4 = "10.97.0.1"
R2_V4 = "10.97.0.2"
V4_LEN = 30

R1_V6 = "2001:db8:97::1"
R2_V6 = "2001:db8:97::2"
V6_LEN = 64

R1_AS = 65100
R2_AS = 65002

R2_EXPORT_RT = "rt:65002:777"
R2_VPN_TARGET = "65002:777"

CE_PREFIX_ADDR = "10.249.10.0"
CE_PREFIX_LEN = 24
CE_PREFIX = f"{CE_PREFIX_ADDR}/{CE_PREFIX_LEN}"
CE_V6_PREFIX_ADDR = "2001:db8:249:10::"
CE_V6_PREFIX_LEN = 64
CE_V6_PREFIX = f"{CE_V6_PREFIX_ADDR}/{CE_V6_PREFIX_LEN}"

LOCAL_PREFIX_ADDR = "10.249.20.0"
LOCAL_PREFIX_LEN = 24
LOCAL_PREFIX = f"{LOCAL_PREFIX_ADDR}/{LOCAL_PREFIX_LEN}"
LOCAL_V6_PREFIX_ADDR = "2001:db8:249:20::"
LOCAL_V6_PREFIX_LEN = 64
LOCAL_V6_PREFIX = f"{LOCAL_V6_PREFIX_ADDR}/{LOCAL_V6_PREFIX_LEN}"


def _module_ipc_up(rt: TopologyRuntime, device: str, module_name: str) -> bool:
    out = cmd(rt, device, "show dev modules", strict=False, timeout=10)
    # Rows end with a PID (or '-' for a down module):
    #   id name phase port ipc pid
    pattern = rf"(?im)^\s*\d+\s+{re.escape(module_name)}\s+\S+\s+\d+\s+up\s+\S+\s*$"
    return re.search(pattern, out) is not None


def _cleanup(rt: TopologyRuntime, base: dict[str, dict[str, str | int]]) -> None:
    for dev in ("r1", "r2"):
        b = base[dev]
        local_v4 = R1_V4 if dev == "r1" else R2_V4
        local_v6 = R1_V6 if dev == "r1" else R2_V6
        commands = [
            "end",
            "config",
            f"no route static ipv4 vrf {VRF_NAME} {CE_PREFIX_ADDR} {CE_PREFIX_LEN} {R2_V4}",
            f"no route static ipv4 vrf {VRF_NAME} {LOCAL_PREFIX_ADDR} {LOCAL_PREFIX_LEN} {R1_V4}",
            f"no route static ipv6 vrf {VRF_NAME} {CE_V6_PREFIX_ADDR} {CE_V6_PREFIX_LEN} {R2_V6}",
            f"no route static ipv6 vrf {VRF_NAME} {LOCAL_V6_PREFIX_ADDR} {LOCAL_V6_PREFIX_LEN} {R1_V6}",
        ]
        if _module_ipc_up(rt, dev, "bgp"):
            commands.append("no bgp")
        commands.extend(
            [
                f"if {GE_IF}",
                "no shutdown",
                f"no ip address {local_v4} {V4_LEN}",
                f"no ipv6 address {local_v6} {V6_LEN}",
                "no vrf forwarding",
                f"ip address {b['v4']} {b['v4_len']}",
                f"ipv6 address {b['v6']} {b['v6_len']}",
                "exit",
                f"no vrf {VRF_NAME}",
                "end",
            ]
        )
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=commands,
        )


def _setup_vrf_interface(
    rt: TopologyRuntime,
    *,
    device: str,
    local_v4: str,
    local_v6: str,
    rd_v4: str,
    rd_v6: str,
    export_rt: str | None = None,
) -> None:
    commands = [
        "config",
        f"vrf {VRF_NAME}",
        "af ipv4",
        f"route-distinguisher {rd_v4}",
    ]
    if export_rt:
        commands.append(f"vpn-target {export_rt} export")
    commands.extend(["exit", "af ipv6", f"route-distinguisher {rd_v6}"])
    if export_rt:
        commands.append(f"vpn-target {export_rt} export")
    commands.extend(["exit", "exit", "end"])
    run_cmds(rt=rt, device=device, commands=commands)

    wait_check(
        rt,
        device=device,
        command=f"show vrf name {VRF_NAME}",
        timeout=10,
        interval=1,
        contains=["VRF Detail:", f"Name           : {VRF_NAME}"],
        label=f"{device} vrf {VRF_NAME} ready",
    )
    time.sleep(2)

    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {local_v4} {V4_LEN}",
            f"ipv6 address {local_v6} {V6_LEN}",
            "exit",
            "end",
        ],
    )


def _wait_vrf_link_routes(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": dev,
                "command": f"show route ipv4 vrf {VRF_NAME} 10.97.0.0 {V4_LEN}",
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                "label": f"{dev} vrf ipv4 link route ready",
            }
            for dev in ("r1", "r2")
        ]
        + [
            {
                "device": dev,
                "command": f"show route ipv6 vrf {VRF_NAME} 2001:db8:97:: {V6_LEN}",
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
                "label": f"{dev} vrf ipv6 link route ready",
            }
            for dev in ("r1", "r2")
        ],
        timeout=20,
        interval=2,
    )


def _configure_bgp(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"bgp {R1_AS}",
            f"vrf {VRF_NAME}",
            "router-id 1.1.1.1",
            f"neighbor {R2_V4} as {R2_AS}",
            f"neighbor {R2_V6} as {R2_AS}",
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
            f"neighbor {R1_V6} as {R1_AS}",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"bgp {R1_AS}",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"neighbor {R2_V4} enable",
            "import-route static",
            "exit",
            "af ipv6-unicast",
            f"neighbor {R2_V6} enable",
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
            "af ipv4-unicast",
            f"neighbor {R1_V4} enable",
            "import-route static",
            "exit",
            "af ipv6-unicast",
            f"neighbor {R1_V6} enable",
            "import-route static",
            "exit",
            "exit",
            "end",
        ],
    )


def _configure_bgp_base(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"bgp {R1_AS}",
            "router-id 1.1.1.1",
            "end",
        ],
        timeout=30,
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"bgp {R2_AS}",
            "router-id 2.2.2.2",
            "end",
        ],
        timeout=30,
    )


def _wait_session(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                "contains": [R2_V4, "AF: ipv4-unicast"],
                "regex": [rf"(?im)^\s*{re.escape(R2_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1 vrf ipv4 session up",
            },
            {
                "device": "r1",
                "command": f"show bgp neighbor af ipv6-unicast vrf {VRF_NAME}",
                "contains": [R2_V6, "AF: ipv6-unicast"],
                "regex": [rf"(?im)^\s*{re.escape(R2_V6)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1 vrf ipv6 session up",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv4-unicast vrf {VRF_NAME}",
                "contains": [R1_V4, "AF: ipv4-unicast"],
                "regex": [rf"(?im)^\s*{re.escape(R1_V4)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2 vrf ipv4 session up",
            },
            {
                "device": "r2",
                "command": f"show bgp neighbor af ipv6-unicast vrf {VRF_NAME}",
                "contains": [R1_V6, "AF: ipv6-unicast"],
                "regex": [rf"(?im)^\s*{re.escape(R1_V6)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r2 vrf ipv6 session up",
            },
        ],
        timeout=60,
        interval=2,
    )


def _install_static_sources(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"route static ipv4 vrf {VRF_NAME} {CE_PREFIX_ADDR} {CE_PREFIX_LEN} {R2_V4}",
            f"route static ipv6 vrf {VRF_NAME} {CE_V6_PREFIX_ADDR} {CE_V6_PREFIX_LEN} {R2_V6}",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"route static ipv4 vrf {VRF_NAME} {LOCAL_PREFIX_ADDR} {LOCAL_PREFIX_LEN} {R1_V4}",
            f"route static ipv6 vrf {VRF_NAME} {LOCAL_V6_PREFIX_ADDR} {LOCAL_V6_PREFIX_LEN} {R1_V6}",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show route ipv4 vrf {VRF_NAME} {CE_PREFIX_ADDR} {CE_PREFIX_LEN}",
                "contains": [CE_PREFIX, R2_V4],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
                "label": "r1 CE static source route",
            },
            {
                "device": "r1",
                "command": f"show route ipv6 vrf {VRF_NAME} {CE_V6_PREFIX_ADDR} {CE_V6_PREFIX_LEN}",
                "contains": [CE_V6_PREFIX, R2_V6],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
                "label": "r1 CE static ipv6 source route",
            },
            {
                "device": "r2",
                "command": f"show route ipv4 vrf {VRF_NAME} {LOCAL_PREFIX_ADDR} {LOCAL_PREFIX_LEN}",
                "contains": [LOCAL_PREFIX, R1_V4],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
                "label": "r2 local PE static source route",
            },
            {
                "device": "r2",
                "command": f"show route ipv6 vrf {VRF_NAME} {LOCAL_V6_PREFIX_ADDR} {LOCAL_V6_PREFIX_LEN}",
                "contains": [LOCAL_V6_PREFIX, R1_V6],
                "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
                "label": "r2 local PE static ipv6 source route",
            },
        ],
        timeout=20,
        interval=2,
    )


def _wait_bgp_routes(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} {LOCAL_PREFIX_ADDR} {LOCAL_PREFIX_LEN}",
                "contains": [
                    f"BGP Route Detail: {LOCAL_PREFIX}",
                    "Imported",
                    f"NextHop  : {R1_V4}",
                    f"Ext-Comm : {R2_EXPORT_RT}",
                ],
                "label": "r2 local imported VRF route carries export RT",
            },
            {
                "device": "r2",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME} {LOCAL_V6_PREFIX_ADDR} {LOCAL_V6_PREFIX_LEN}",
                "contains": [
                    f"BGP Route Detail: {LOCAL_V6_PREFIX}",
                    "Imported",
                    f"NextHop  : {R1_V6}",
                    f"Ext-Comm : {R2_EXPORT_RT}",
                ],
                "label": "r2 local imported IPv6 VRF route carries export RT",
            },
            {
                "device": "r2",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} {CE_PREFIX_ADDR} {CE_PREFIX_LEN}",
                "contains": [
                    f"BGP Route Detail: {CE_PREFIX}",
                    f"From Peer  : {R1_V4}",
                    f"NextHop  : {R1_V4}",
                    f"Ext-Comm : {R2_EXPORT_RT}",
                ],
                "regex": [rf"(?im)^\s*AS-Path\s*:\s*{R1_AS}\s*$"],
                "label": "r2 CE-learned VRF route carries local export RT",
            },
            {
                "device": "r2",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME} {CE_V6_PREFIX_ADDR} {CE_V6_PREFIX_LEN}",
                "contains": [
                    f"BGP Route Detail: {CE_V6_PREFIX}",
                    f"From Peer  : {R1_V6}",
                    f"NextHop  : {R1_V6}",
                    f"Ext-Comm : {R2_EXPORT_RT}",
                ],
                "regex": [rf"(?im)^\s*AS-Path\s*:\s*{R1_AS}\s*$"],
                "label": "r2 CE-learned IPv6 VRF route carries local export RT",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME} {CE_PREFIX_ADDR} {CE_PREFIX_LEN}",
                "contains": [f"BGP Route Detail: {CE_PREFIX}", "Imported"],
                "not_contains": ["Ext-Comm"],
                "label": "r1 CE source route has no pre-existing RT",
            },
            {
                "device": "r1",
                "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME} {CE_V6_PREFIX_ADDR} {CE_V6_PREFIX_LEN}",
                "contains": [f"BGP Route Detail: {CE_V6_PREFIX}", "Imported"],
                "not_contains": ["Ext-Comm"],
                "label": "r1 CE IPv6 source route has no pre-existing RT",
            },
        ],
        timeout=40,
        interval=2,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    base = {
        "r1": {
            "v4": str(g_top.r1.GE_1.ip),
            "v4_len": int(g_top.r1.GE_1.prefix),
            "v6": str(g_top.r1.GE_1.ip6),
            "v6_len": int(g_top.r1.GE_1.prefix6),
        },
        "r2": {
            "v4": str(g_top.r2.GE_1.ip),
            "v4_len": int(g_top.r2.GE_1.prefix),
            "v6": str(g_top.r2.GE_1.ip6),
            "v6_len": int(g_top.r2.GE_1.prefix6),
        },
    }

    try:
        _cleanup(rt, base)

        step("Start BGP modules and configure public router IDs")
        _configure_bgp_base(rt)

        step("Create VRF red; only r2 PE VRF has an export RT")
        _setup_vrf_interface(
            rt, device="r1", local_v4=R1_V4, local_v6=R1_V6, rd_v4="65100:97", rd_v6="65100:98"
        )
        _setup_vrf_interface(
            rt,
            device="r2",
            local_v4=R2_V4,
            local_v6=R2_V6,
            rd_v4="65002:97",
            rd_v6="65002:98",
            export_rt=R2_VPN_TARGET,
        )
        _wait_vrf_link_routes(rt)

        step("Configure VRF eBGP and import-route static on both sides")
        _configure_bgp(rt)
        _wait_session(rt)

        step("Install one CE-side static route and one PE-local VRF static route")
        _install_static_sources(rt)

        step("Verify VRF BGP routes carry r2 export RT in effective attributes")
        _wait_bgp_routes(rt)

        step("Verify public BGP RIB remains isolated from VRF routes")
        public_v4_out = cmd(rt, "r2", "show bgp route af ipv4-unicast", strict=False)
        for prefix in (CE_PREFIX, LOCAL_PREFIX):
            if prefix in public_v4_out:
                raise AssertionError(f"r2 public IPv4 BGP RIB unexpectedly contains {prefix}:\n{public_v4_out}")
        public_v6_out = cmd(rt, "r2", "show bgp route af ipv6-unicast", strict=False)
        for prefix in (CE_V6_PREFIX, LOCAL_V6_PREFIX):
            if prefix in public_v6_out:
                raise AssertionError(f"r2 public IPv6 BGP RIB unexpectedly contains {prefix}:\n{public_v6_out}")

        print("BGP VRF route-target attr check passed.")
    finally:
        _cleanup(rt, base)
