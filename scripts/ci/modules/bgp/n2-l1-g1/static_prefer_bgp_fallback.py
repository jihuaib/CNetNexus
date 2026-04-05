#!/usr/bin/env python3
"""
BGP vs static preference switch check.

Goal:
- r2 advertises a prefix to r1 via BGP (imported from r2 static route)
- r1 configures the same prefix as static route
- verify r1 prefers static route in both Route RIB and OS table
- remove r1 static route
- verify r1 falls back to BGP route in both Route RIB and OS table
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TARGET_PREFIX_ADDR = "10.40.40.0"
TARGET_MASK = "24"
TARGET_PREFIX = f"{TARGET_PREFIX_ADDR}/24"


def _wait_route_best_static_backup_bgp(rt: TopologyRuntime, *, device: str, destination: str, timeout: int) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for", "Total 2 path(s)"],
        count={"Path [": 2},
        regex=[
            r"(?is)Path\s*\[1\]\s*:\s*static\b.*?Preference\s*:\s*1\b",
            r"(?is)Path\s*\[2\]\s*:\s*bgp\b.*?Preference\s*:\s*200\b",
        ],
        label=f"{device} best=static backup=bgp {destination}",
    )


def _wait_route_best_bgp_only(rt: TopologyRuntime, *, device: str, destination: str, timeout: int) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for", "Total 1 path(s)"],
        count={"Path [": 1},
        regex=[r"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Preference\s*:\s*200\b"],
        not_regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"],
        label=f"{device} best=bgp only {destination}",
    )


def _wait_os_best_proto(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    gateway: str,
    proto: str,
    timeout: int,
) -> None:
    alt_proto = "bgp" if proto == "static" else "static"
    best_row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(proto)}\s+\d+\s*$"
    )
    alt_row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+{re.escape(alt_proto)}\s+\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show route ipv4 os",
        timeout=timeout,
        interval=2,
        regex=[best_row_regex],
        not_regex=[alt_row_regex],
        label=f"{device} os-best {prefix} proto={proto}",
    )


def _cleanup_case_config(rt: TopologyRuntime, *, r1_nh: str, r2_nh: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {r1_nh}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {r2_nh}",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_local_ip = str(g_top.r2.GE_1.ip)

    try:
        step("Cleanup stale config")
        _cleanup_case_config(rt, r1_nh=r1_peer_ip, r2_nh=r2_local_ip)

        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", strict=False, commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", strict=False, commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

        step("Configure BGP neighbors and import-route on r2")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_ip} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Wait BGP sessions")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [r1_peer_ip, "Established"],
                    "label": "r1->r2 ipv4-unicast",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [r2_peer_ip, "Established"],
                    "label": "r2->r1 ipv4-unicast",
                },
            ],
            timeout=30,
        )

        step("Inject BGP route source on r2 (static imported into BGP)")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {r2_local_ip}",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [TARGET_PREFIX],
                    "label": "r1 learned BGP route",
                }
            ],
            timeout=30,
        )

        step("Configure competing static route on r1 for same prefix")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {r1_peer_ip}",
                "end",
            ],
        )

        step("Verify route and OS both prefer static over BGP")
        _wait_route_best_static_backup_bgp(rt, device="r1", destination=TARGET_PREFIX_ADDR, timeout=30)
        _wait_os_best_proto(rt, device="r1", prefix=TARGET_PREFIX, gateway=r1_peer_ip, proto="static", timeout=30)

        step("Delete r1 static route")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"no route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {r1_peer_ip}",
                "end",
            ],
        )

        step("Verify route and OS switch to BGP after static removal")
        _wait_route_best_bgp_only(rt, device="r1", destination=TARGET_PREFIX_ADDR, timeout=30)
        _wait_os_best_proto(rt, device="r1", prefix=TARGET_PREFIX, gateway=r1_peer_ip, proto="bgp", timeout=30)

        print("BGP static-vs-bgp preference fallback check passed.")
    finally:
        _cleanup_case_config(rt, r1_nh=r1_peer_ip, r2_nh=r2_local_ip)
