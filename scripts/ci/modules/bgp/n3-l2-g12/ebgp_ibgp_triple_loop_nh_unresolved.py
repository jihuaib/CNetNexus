#!/usr/bin/env python3
"""
eBGP + iBGP triple-loopback nexthop-unresolved check (IPv4).

Topology: r1(a) --- r2(b) --- r3(c)

Goal:
- r1 and r2 build one direct eBGP session (different AS) over the r1-r2 link
- r1 imports one local static route and advertises it via eBGP to r2
- r2 and r3 bring up ISIS on the r2-r3 link and on three loopback pairs,
  so each side can reach the other's three loopback addresses
- r2 and r3 build three iBGP sessions (same AS), each bound to a different
  loopback pair via source-interface
- r2 re-advertises the eBGP-learned route to r3 on all three iBGP sessions
  with nexthop unchanged (preserved eBGP nexthop = r1's r1-r2 link address)
- r3 therefore learns three BGP paths for the same prefix, each with
  NextHop = r1's interface address; none of them can be best-selected
  because the nexthop is not reachable on r3 (the r1-r2 link subnet is
  not advertised into ISIS)
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    cmd,
    g_top,
    hold_check,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


AS_A = "65001"
AS_BC = "65002"

ISIS_TAG = 300
R2_NET = "49.0003.0000.0000.0002.00"
R3_NET = "49.0003.0000.0000.0003.00"

GE_A_B = "GE-1"   # r1<->r2 link
GE_B_A = "GE-1"
GE_B_C = "GE-2"   # r2<->r3 link
GE_C_B = "GE-1"

LOOP_HOST_MASK = 32
LOOP_COUNT = 3

R2_LOOP_IDS = [201, 202, 203]
R3_LOOP_IDS = [301, 302, 303]
R2_LOOP_IPS = ["20.2.2.1", "20.2.2.2", "20.2.2.3"]
R3_LOOP_IPS = ["20.3.3.1", "20.3.3.2", "20.3.3.3"]

TEST_PFX_ADDR = "10.100.100.0"
TEST_PFX_MASK = "24"
TEST_PFX = f"{TEST_PFX_ADDR}/{TEST_PFX_MASK}"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _cleanup(rt: TopologyRuntime, *, r1_static_nh: str) -> None:
    step("Cleanup BGP/ISIS/loopback/static config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no route static ipv4 {TEST_PFX_ADDR} {TEST_PFX_MASK} {r1_static_nh}",
            "no bgp",
            "end",
        ],
    )

    r2_cleanup = ["end", "config", "no bgp", f"no isis {ISIS_TAG}"]
    for loop_id in R2_LOOP_IDS:
        r2_cleanup.append(f"no if loop {loop_id}")
    r2_cleanup.extend(
        [
            f"if {GE_B_C}",
            "no shutdown",
            "exit",
            "end",
        ]
    )
    run_cmds(rt=rt, device="r2", strict=False, commands=r2_cleanup)

    r3_cleanup = ["end", "config", "no bgp", f"no isis {ISIS_TAG}"]
    for loop_id in R3_LOOP_IDS:
        r3_cleanup.append(f"no if loop {loop_id}")
    r3_cleanup.extend(
        [
            f"if {GE_C_B}",
            "no shutdown",
            "exit",
            "end",
        ]
    )
    run_cmds(rt=rt, device="r3", strict=False, commands=r3_cleanup)


def _configure_loopbacks(rt: TopologyRuntime) -> None:
    step("Configure three loopbacks on r2 and r3")
    r2_cmds: list[str] = ["config"]
    for loop_id, loop_ip in zip(R2_LOOP_IDS, R2_LOOP_IPS):
        r2_cmds.extend(
            [
                f"if loop {loop_id}",
                f"ip address {loop_ip} {LOOP_HOST_MASK}",
                "exit",
            ]
        )
    r2_cmds.append("end")
    run_cmds(rt=rt, device="r2", commands=r2_cmds)

    r3_cmds: list[str] = ["config"]
    for loop_id, loop_ip in zip(R3_LOOP_IDS, R3_LOOP_IPS):
        r3_cmds.extend(
            [
                f"if loop {loop_id}",
                f"ip address {loop_ip} {LOOP_HOST_MASK}",
                "exit",
            ]
        )
    r3_cmds.append("end")
    run_cmds(rt=rt, device="r3", commands=r3_cmds)

    loop_checks: list[dict[str, object]] = []
    for loop_id, loop_ip in zip(R2_LOOP_IDS, R2_LOOP_IPS):
        loop_checks.append(
            {
                "device": "r2",
                "command": f"show if loop {loop_id}",
                "contains": [
                    f"Interface loop{loop_id} Detail:",
                    "State      : UP",
                    f"IPv4 Addr : {loop_ip}/{LOOP_HOST_MASK}",
                ],
                "label": f"r2 loop{loop_id} up",
            }
        )
    for loop_id, loop_ip in zip(R3_LOOP_IDS, R3_LOOP_IPS):
        loop_checks.append(
            {
                "device": "r3",
                "command": f"show if loop {loop_id}",
                "contains": [
                    f"Interface loop{loop_id} Detail:",
                    "State      : UP",
                    f"IPv4 Addr : {loop_ip}/{LOOP_HOST_MASK}",
                ],
                "label": f"r3 loop{loop_id} up",
            }
        )
    wait_checks(rt, loop_checks, timeout=30, interval=2)


def _configure_isis_between_b_and_c(rt: TopologyRuntime) -> None:
    step("Bring up ISIS between r2 and r3 (link + loopbacks)")
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"isis {ISIS_TAG}",
            f"net {R2_NET}",
            "is-type level-1-2",
            "af ipv4",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r3",
        commands=[
            "config",
            f"isis {ISIS_TAG}",
            f"net {R3_NET}",
            "is-type level-1-2",
            "af ipv4",
            "end",
        ],
    )

    r2_isis_if: list[str] = [
        "config",
        f"if {GE_B_C}",
        f"isis enable {ISIS_TAG}",
        f"isis hello-interval {ISIS_TAG} 3",
        f"isis hold-multiplier {ISIS_TAG} 3",
        "exit",
    ]
    for loop_id in R2_LOOP_IDS:
        r2_isis_if.extend(
            [
                f"if loop {loop_id}",
                f"isis enable {ISIS_TAG}",
                f"isis passive {ISIS_TAG}",
                "exit",
            ]
        )
    r2_isis_if.append("end")
    run_cmds(rt=rt, device="r2", commands=r2_isis_if)

    r3_isis_if: list[str] = [
        "config",
        f"if {GE_C_B}",
        f"isis enable {ISIS_TAG}",
        f"isis hello-interval {ISIS_TAG} 3",
        f"isis hold-multiplier {ISIS_TAG} 3",
        "exit",
    ]
    for loop_id in R3_LOOP_IDS:
        r3_isis_if.extend(
            [
                f"if loop {loop_id}",
                f"isis enable {ISIS_TAG}",
                f"isis passive {ISIS_TAG}",
                "exit",
            ]
        )
    r3_isis_if.append("end")
    run_cmds(rt=rt, device="r3", commands=r3_isis_if)

    step("Wait ISIS adjacency up on r2-r3 link")
    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": f"show isis neighbor {ISIS_TAG}",
                "contains": ["ISIS Neighbors", GE_B_C],
                "regex": [rf"(?im)^\s*{ISIS_TAG}\s+{re.escape(GE_B_C)}\s+L[12]\s+\S+\s+Up\s+"],
                "label": "r2 ISIS neighbor up",
            },
            {
                "device": "r3",
                "command": f"show isis neighbor {ISIS_TAG}",
                "contains": ["ISIS Neighbors", GE_C_B],
                "regex": [rf"(?im)^\s*{ISIS_TAG}\s+{re.escape(GE_C_B)}\s+L[12]\s+\S+\s+Up\s+"],
                "label": "r3 ISIS neighbor up",
            },
        ],
        timeout=80,
        interval=2,
    )

    step("Wait ISIS learns each other's three loopbacks")
    r2_reach_checks = [
        {
            "device": "r2",
            "command": f"show route ipv4 {loop_ip} {LOOP_HOST_MASK}",
            "contains": [f"Routing entry for {loop_ip}/{LOOP_HOST_MASK}"],
            "not_contains": ["(no routes)", "(no matching routes)"],
            "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*isis\b"],
            "label": f"r2 learned r3 loopback {loop_ip} via ISIS",
        }
        for loop_ip in R3_LOOP_IPS
    ]
    r3_reach_checks = [
        {
            "device": "r3",
            "command": f"show route ipv4 {loop_ip} {LOOP_HOST_MASK}",
            "contains": [f"Routing entry for {loop_ip}/{LOOP_HOST_MASK}"],
            "not_contains": ["(no routes)", "(no matching routes)"],
            "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*isis\b"],
            "label": f"r3 learned r2 loopback {loop_ip} via ISIS",
        }
        for loop_ip in R2_LOOP_IPS
    ]
    wait_checks(rt, r2_reach_checks + r3_reach_checks, timeout=90, interval=2)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    # r1 <-> r2 link
    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)     # r2 GE-1 address
    r2_to_r1_v4 = str(g_top.r2.GE_1.peer_ip)    # r1 GE-1 address
    # static route nexthop on r1: its own eBGP peer link address ensures
    # r1 has a reachable nexthop and BGP will advertise this prefix
    r1_static_nh = r1_peer_v4
    # nexthop r3 should see for the BGP route:
    expected_nexthop_on_r3 = r2_to_r1_v4

    try:
        _cleanup(rt, r1_static_nh=r1_static_nh)

        _configure_loopbacks(rt)
        _configure_isis_between_b_and_c(rt)

        step("Configure BGP base on r1/r2/r3")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {AS_A}",
                "router-id 1.1.1.1",
                "timer connect-retry 5",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {AS_BC}",
                "router-id 2.2.2.2",
                "timer connect-retry 5",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r3",
            commands=[
                "config",
                f"bgp {AS_BC}",
                "router-id 3.3.3.3",
                "timer connect-retry 5",
                "end",
            ],
        )

        step("Configure direct eBGP on r1-r2")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {AS_A}",
                f"neighbor {r1_peer_v4} as {AS_BC}",
                "af ipv4-unicast",
                f"neighbor {r1_peer_v4} enable",
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
                f"bgp {AS_BC}",
                f"neighbor {r2_to_r1_v4} as {AS_A}",
                "af ipv4-unicast",
                f"neighbor {r2_to_r1_v4} enable",
                "exit",
                "end",
            ],
        )

        step("Configure three iBGP sessions on r2-r3 over three loopback pairs")
        r2_ibgp: list[str] = ["config", f"bgp {AS_BC}"]
        for r3_loop_ip in R3_LOOP_IPS:
            r2_ibgp.append(f"neighbor {r3_loop_ip} as {AS_BC}")
        for r3_loop_ip, r2_loop_id in zip(R3_LOOP_IPS, R2_LOOP_IDS):
            r2_ibgp.append(f"neighbor {r3_loop_ip} source-interface loop{r2_loop_id}")
        r2_ibgp.append("af ipv4-unicast")
        for r3_loop_ip in R3_LOOP_IPS:
            r2_ibgp.append(f"neighbor {r3_loop_ip} enable")
        r2_ibgp.extend(["exit", "end"])
        run_cmds(rt=rt, device="r2", commands=r2_ibgp)

        r3_ibgp: list[str] = ["config", f"bgp {AS_BC}"]
        for r2_loop_ip in R2_LOOP_IPS:
            r3_ibgp.append(f"neighbor {r2_loop_ip} as {AS_BC}")
        for r2_loop_ip, r3_loop_id in zip(R2_LOOP_IPS, R3_LOOP_IDS):
            r3_ibgp.append(f"neighbor {r2_loop_ip} source-interface loop{r3_loop_id}")
        r3_ibgp.append("af ipv4-unicast")
        for r2_loop_ip in R2_LOOP_IPS:
            r3_ibgp.append(f"neighbor {r2_loop_ip} enable")
        r3_ibgp.extend(["exit", "end"])
        run_cmds(rt=rt, device="r3", commands=r3_ibgp)

        step("Wait eBGP r1<->r2 and three iBGP r2<->r3 sessions established")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_established_regex(r1_peer_v4)],
                    "label": "r1->r2 eBGP established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [
                        _established_regex(r2_to_r1_v4),
                        *[_established_regex(ip) for ip in R3_LOOP_IPS],
                    ],
                    "label": "r2 has eBGP + 3 iBGP established",
                },
                {
                    "device": "r3",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_established_regex(ip) for ip in R2_LOOP_IPS],
                    "label": "r3 has 3 iBGP established",
                },
            ],
            timeout=60,
        )

        step("Inject one static route on r1 for eBGP import")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static ipv4 {TEST_PFX_ADDR} {TEST_PFX_MASK} {r1_static_nh}",
                "end",
            ],
        )

        step("Wait r2 learns prefix from r1 via eBGP")
        wait_check(
            rt,
            device="r2",
            command=f"show bgp route af ipv4-unicast {TEST_PFX_ADDR} {TEST_PFX_MASK}",
            timeout=40,
            contains=[
                f"BGP Route Detail: {TEST_PFX}",
                f"NextHop  : {r2_to_r1_v4}",
            ],
            not_contains=["(no matching routes)", "not found"],
            regex=[r"(?im)^\s*Paths\s*:\s*[1-9]\d*\s*$"],
            label="r2 learned eBGP route",
        )

        step("Wait r3 receives three iBGP paths for the same prefix")
        wait_check(
            rt,
            device="r3",
            command=f"show bgp route af ipv4-unicast {TEST_PFX_ADDR} {TEST_PFX_MASK}",
            timeout=60,
            contains=[f"BGP Route Detail: {TEST_PFX}"],
            not_contains=["(no matching routes)", "not found"],
            regex=[r"(?im)^\s*Paths\s*:\s*3\s*$"],
            label="r3 has 3 BGP paths",
        )

        step("Verify all three paths carry NextHop equal to r1's interface address")
        wait_check(
            rt,
            device="r3",
            command=f"show bgp route af ipv4-unicast {TEST_PFX_ADDR} {TEST_PFX_MASK}",
            timeout=30,
            contains=[f"BGP Route Detail: {TEST_PFX}"],
            count={
                f"NextHop  : {expected_nexthop_on_r3}": 3,
                "From Peer  :": 3,
            },
            regex=[
                *[
                    rf"(?im)^.*From Peer\s*:\s*{re.escape(src)}\s*$"
                    for src in R2_LOOP_IPS
                ],
            ],
            label="r3 three paths nexthop=r1 link address",
        )

        step("Verify none of the three paths is selected as best (nexthop unresolved)")
        # Because r3's RIB has no route to r1's r1-r2 link subnet, every path
        # must keep IterState=Unresolved and Valid=No, which also means none
        # should carry the best-route marker '>' in the list view.
        r3_detail = cmd(
            rt,
            "r3",
            f"show bgp route af ipv4-unicast {TEST_PFX_ADDR} {TEST_PFX_MASK}",
            strict=False,
        )
        if re.search(r"(?m)^>\S?\s+From Peer", r3_detail):
            raise RuntimeError(
                "r3 unexpectedly selected an iBGP path as best "
                "even though nexthop should be unresolved:\n" + r3_detail
            )
        if re.search(r"(?im)^\s*Valid\s*:\s*Yes\s*$", r3_detail):
            raise RuntimeError(
                "r3 unexpectedly marks an iBGP path as Valid=Yes "
                "even though nexthop should be unresolved:\n" + r3_detail
            )

        hold_check(
            rt,
            device="r3",
            command=f"show bgp route af ipv4-unicast {TEST_PFX_ADDR} {TEST_PFX_MASK}",
            duration=12,
            interval=3,
            contains=[f"BGP Route Detail: {TEST_PFX}"],
            not_regex=[
                r"(?m)^>\S?\s+From Peer",
                r"(?im)^\s*Valid\s*:\s*Yes\s*$",
            ],
            count={
                "IterState: Unresolved": 3,
                "Valid    : No": 3,
                f"NextHop  : {expected_nexthop_on_r3}": 3,
            },
            label="r3 three paths remain unresolved and not-best",
        )

        step("Verify the prefix is not installed in the r3 Route RIB as BGP-best")
        r3_route_out = cmd(
            rt,
            "r3",
            f"show route ipv4 {TEST_PFX_ADDR} {TEST_PFX_MASK}",
            strict=False,
        )
        if re.search(r"(?im)^\s*Path\s*\[\d+\]\s*:\s*bgp\b", r3_route_out):
            raise RuntimeError(
                "r3 Route RIB unexpectedly has a BGP path installed for "
                f"{TEST_PFX}:\n{r3_route_out}"
            )

        print("eBGP + iBGP triple-loop nexthop-unresolved check passed.")
    finally:
        _cleanup(rt, r1_static_nh=r1_static_nh)
