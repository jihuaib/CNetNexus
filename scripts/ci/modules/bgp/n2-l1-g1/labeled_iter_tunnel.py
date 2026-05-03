#!/usr/bin/env python3
"""
BGP labeled-unicast tunnel programming check (IPv4).

Topology: r1 --- r2

Goal:
- r1 imports a static route into IPv4 labeled-unicast and allocates a label.
- r2 receives the LU route, resolves its FEC through the LU tunnel, and
  programs Route/FIB/OS with an MPLS tunnel nexthop.
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


AS_R1 = "65001"
AS_R2 = "65002"

TEST_PREFIX_ADDR = "10.100.100.0"
TEST_PREFIX_LEN = "24"
TEST_PREFIX = f"{TEST_PREFIX_ADDR}/{TEST_PREFIX_LEN}"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _cleanup(rt: TopologyRuntime, *, r1_to_r2_peer: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no route ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN} {r1_to_r2_peer}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(rt=rt, device="r2", strict=False, commands=["end", "config", "no bgp", "end"])


def _wait_r2_lu_route(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    wait_check(
        rt,
        device="r2",
        command=f"show bgp route af ipv4-labeled {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        contains=[f"BGP Route Detail: {TEST_PREFIX}", f"NextHop  : {lu_nexthop}"],
        regex=[
            r"(?im)^\s*Paths\s*:\s*1\s*$",
            r"(?im)^>v\s+From Peer\s*:",
            r"(?im)^\s*Valid\s*:\s*Yes\s*$",
            r"(?im)^\s*IterState:\s*Resolved\s*$",
            r"(?im)^\s*Tunnel-ID:\s*[1-9]\d*\s*$",
        ],
        not_contains=[f"Route {TEST_PREFIX} not found"],
        label="r2 LU route is valid and resolved through a tunnel",
    )


def _wait_r2_tunnel_fec(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    wait_check(
        rt,
        device="r2",
        command="show tunnel",
        timeout=timeout,
        interval=2,
        contains=[f"endpoint {TEST_PREFIX_ADDR}", f"nh {lu_nexthop}", f"relay {lu_nexthop}", "src bgp-lu"],
        regex=[
            rf"(?im)^\s*id\s+[1-9]\d*\s+endpoint\s+{re.escape(TEST_PREFIX_ADDR)}\s+relay\s+"
            rf"{re.escape(lu_nexthop)}\s+oif\s+\d+\s+src\s+bgp-lu\s+labels\s+\[[0-9,]+\]\s*$",
            rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+fec\s+{re.escape(TEST_PREFIX_ADDR)}/{TEST_PREFIX_LEN}\s+"
            r"->\s+nhlfe\s+[1-9]\d*\s+src\s+bgp-lu\s+state\s+up\s*$",
        ],
        label="r2 tunnel RIB builds LU NHLFE/FTN for the FEC",
    )


def _wait_r2_route_rib_tunnel(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    wait_check(
        rt,
        device="r2",
        command=f"show route ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {TEST_PREFIX}", f"Nexthop   : {lu_nexthop}"],
        regex=[
            r"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Iter NH\s*:\s*"
            + re.escape(lu_nexthop)
            + r"\b.*?NH-Type\s*:\s*tunnel\b.*?Tunnel-ID\s*:\s*[1-9]\d*",
        ],
        label="r2 Route RIB installs LU path as tunnel nexthop",
    )


def _wait_r2_fib_tunnel(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    wait_check(
        rt,
        device="r2",
        command=f"show fib ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}",
        timeout=timeout,
        interval=2,
        contains=[f"FIB Route Detail: {TEST_PREFIX}"],
        regex=[
            r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
            r"(?im)^\s*Tunnel-ID\s*:\s*[1-9]\d*\s*$",
            r"(?im)^\s*Installed\s*:\s*yes\s*$",
            r"(?im)^\s*Skip OS\s*:\s*no\s*$",
            rf"(?im)^\s*Tunnel\s*:\s*state=up\s+relay={re.escape(lu_nexthop)}\s+oif=\d+\s+labels=\[[0-9,]+\]\s*$",
        ],
        label="r2 FIB route joins LU tunnel state and is installed",
    )


def _wait_r2_os_route(rt: TopologyRuntime, *, lu_nexthop: str, timeout: int) -> None:
    row_regex = rf"(?im)^\s*main\s+unicast\s+{re.escape(TEST_PREFIX)}\s+{re.escape(lu_nexthop)}\s+\S+\s+bgp\s+\d+\s*$"
    wait_check(
        rt,
        device="r2",
        command="show fib ipv4 os",
        timeout=timeout,
        interval=2,
        regex=[row_regex],
        label="r2 OS route table has LU tunnel route",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_to_r2_peer = str(g_top.r1.GE_1.peer_ip)  # r2 GE-1
    r2_to_r1_peer = str(g_top.r2.GE_1.peer_ip)  # r1 GE-1

    try:
        _cleanup(rt, r1_to_r2_peer=r1_to_r2_peer)

        step("Configure r1 static FEC for BGP LU import")
        run_cmds(
            rt=rt,
            device="r1",
            commands=["config", f"route ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN} {r1_to_r2_peer}", "end"],
        )
        wait_check(
            rt,
            device="r1",
            command="show route ipv4 static",
            timeout=30,
            contains=[TEST_PREFIX, r1_to_r2_peer],
            label="r1 static FEC route exists for LU import",
        )

        step("Configure BGP labeled-unicast")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {AS_R1}",
                "router-id 1.1.1.1",
                "timer connect-retry 5",
                f"neighbor {r1_to_r2_peer} as {AS_R2}",
                "af ipv4-labeled",
                f"neighbor {r1_to_r2_peer} enable",
                "import-route static",
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
                "timer connect-retry 5",
                f"neighbor {r2_to_r1_peer} as {AS_R1}",
                "af ipv4-labeled",
                f"neighbor {r2_to_r1_peer} enable",
                "exit",
                "end",
            ],
        )

        step("Wait BGP LU session")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-labeled",
                    "regex": [_established_regex(r1_to_r2_peer)],
                    "label": "r1->r2 IPv4 LU established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-labeled",
                    "regex": [_established_regex(r2_to_r1_peer)],
                    "label": "r2->r1 IPv4 LU established",
                },
            ],
            timeout=70,
        )

        step("Verify r1 allocates label and r2 receives LU route")
        wait_check(
            rt,
            device="r1",
            command="show tunnel label",
            timeout=40,
            interval=2,
            regex=[rf"(?im)^\s*\d+\s+0\s+ipv4\s+{re.escape(TEST_PREFIX)}\s+bgp:\d+\s+bgp-lu\s*$"],
            label="r1 tunnel label allocated for imported LU FEC",
        )
        _wait_r2_lu_route(rt, lu_nexthop=r2_to_r1_peer, timeout=50)

        step("Verify r2 programs LU tunnel route")
        _wait_r2_tunnel_fec(rt, lu_nexthop=r2_to_r1_peer, timeout=50)
        _wait_r2_route_rib_tunnel(rt, lu_nexthop=r2_to_r1_peer, timeout=50)
        _wait_r2_fib_tunnel(rt, lu_nexthop=r2_to_r1_peer, timeout=50)
        _wait_r2_os_route(rt, lu_nexthop=r2_to_r1_peer, timeout=50)

        print("BGP labeled-unicast tunnel programming check passed.")
    finally:
        _cleanup(rt, r1_to_r2_peer=r1_to_r2_peer)
