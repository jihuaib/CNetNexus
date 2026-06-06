#!/usr/bin/env python3
"""
BGP nexthop value refresh check on dual links.

Topology:
- device a <-> device b has two links (GE-1 and GE-2)

Scenario:
1. Build eBGP over loopbacks, so routes learned on b use a's loopback as the
   BGP nexthop.
2. On b, resolve a's loopback through two static paths. GE-1 wins initially.
3. Change only the static metrics so the same BGP nexthop resolves through GE-2.

Checks:
- The learned BGP route keeps the same NH-ID.
- ROUTE/FIB nexthop objects for that NH-ID update relay/OIF.
- The ROUTE path Updated timestamp is unchanged, proving the route path itself
  was not re-added just to refresh the nexthop value.
"""

from __future__ import annotations

import re
import time

from module_api import cmd, g_top, require_devices, run_cmds, should_skip_cleanup, step, wait_check, wait_checks
from top_runner import TopologyRuntime


AS_A = "65001"
AS_B = "65002"

A_LOOP_ID = 601
B_LOOP_ID = 602
A_LOOP_V4 = "172.31.61.1"
B_LOOP_V4 = "172.31.62.1"
HOST_LEN = "32"

TEST_PREFIX_ADDR = "198.51.61.0"
TEST_PREFIX_LEN = "24"
TEST_PREFIX = f"{TEST_PREFIX_ADDR}/{TEST_PREFIX_LEN}"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _extract_field(output: str, field: str, *, command: str) -> str:
    match = re.search(rf"(?im)^\s*{re.escape(field)}\s*:\s*(.*?)\s*$", output)
    if not match:
        raise RuntimeError(f"failed to extract {field!r} from {command} output:\n{output}")
    return match.group(1)


def _extract_nh_id(output: str, *, command: str) -> str:
    value = _extract_field(output, "NH-ID", command=command)
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise RuntimeError(f"invalid NH-ID {value!r} from {command} output:\n{output}")
    return value


def _cleanup(rt: TopologyRuntime, *, a_ge1_peer: str, a_ge2_peer: str, b_ge1_peer: str, b_ge2_peer: str) -> None:
    step("Cleanup BGP nexthop value refresh case")
    run_cmds(
        rt=rt,
        device="a",
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"no route static ipv4 {B_LOOP_V4} {HOST_LEN} {a_ge1_peer}",
            f"no route static ipv4 {B_LOOP_V4} {HOST_LEN} {a_ge2_peer}",
            f"no route static ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN} {a_ge1_peer}",
            f"no if loop {A_LOOP_ID}",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="b",
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"no route static ipv4 {A_LOOP_V4} {HOST_LEN} {b_ge1_peer}",
            f"no route static ipv4 {A_LOOP_V4} {HOST_LEN} {b_ge2_peer}",
            f"no if loop {B_LOOP_ID}",
            "end",
        ],
    )


def _configure_loopbacks(rt: TopologyRuntime) -> None:
    step("Configure loopbacks for multihop BGP")
    run_cmds(
        rt=rt,
        device="a",
        commands=[
            "config",
            f"if loop {A_LOOP_ID}",
            f"ip address {A_LOOP_V4} {HOST_LEN}",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="b",
        commands=[
            "config",
            f"if loop {B_LOOP_ID}",
            f"ip address {B_LOOP_V4} {HOST_LEN}",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "a",
                "command": f"show if loop {A_LOOP_ID}",
                "contains": [f"Interface loop{A_LOOP_ID} Detail:", "State      : UP", f"IPv4 Addr  : {A_LOOP_V4}/32"],
                "label": "a loopback for BGP source is up",
            },
            {
                "device": "b",
                "command": f"show if loop {B_LOOP_ID}",
                "contains": [f"Interface loop{B_LOOP_ID} Detail:", "State      : UP", f"IPv4 Addr  : {B_LOOP_V4}/32"],
                "label": "b loopback for BGP source is up",
            },
        ],
        timeout=30,
        interval=2,
    )


def _configure_underlay_static(
    rt: TopologyRuntime, *, a_ge1_peer: str, a_ge2_peer: str, b_ge1_peer: str, b_ge2_peer: str
) -> None:
    step("Install loopback reachability over both links")
    run_cmds(
        rt=rt,
        device="a",
        commands=[
            "config",
            f"route static ipv4 {B_LOOP_V4} {HOST_LEN} {a_ge1_peer} metric 10",
            f"route static ipv4 {B_LOOP_V4} {HOST_LEN} {a_ge2_peer} metric 20",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="b",
        commands=[
            "config",
            f"route static ipv4 {A_LOOP_V4} {HOST_LEN} {b_ge1_peer} metric 10",
            f"route static ipv4 {A_LOOP_V4} {HOST_LEN} {b_ge2_peer} metric 20",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "a",
                "command": f"show route ipv4 {B_LOOP_V4} {HOST_LEN}",
                "contains": ["Total 2 path(s)"],
                "regex": [
                    rf"(?is)Nexthop\s*:\s*{re.escape(a_ge1_peer)}\b.*?Metric\s*:\s*10\b",
                    rf"(?is)Nexthop\s*:\s*{re.escape(a_ge2_peer)}\b.*?Metric\s*:\s*20\b",
                ],
                "label": "a has two static paths to b loopback",
            },
            {
                "device": "b",
                "command": f"show route ipv4 {A_LOOP_V4} {HOST_LEN}",
                "contains": ["Total 2 path(s)"],
                "regex": [
                    rf"(?is)Nexthop\s*:\s*{re.escape(b_ge1_peer)}\b.*?Metric\s*:\s*10\b",
                    rf"(?is)Nexthop\s*:\s*{re.escape(b_ge2_peer)}\b.*?Metric\s*:\s*20\b",
                ],
                "label": "b has two static paths to a loopback",
            },
        ],
        timeout=30,
        interval=2,
    )


def _configure_bgp(rt: TopologyRuntime) -> None:
    step("Configure eBGP over loopbacks")
    run_cmds(
        rt=rt,
        device="a",
        commands=[
            "config",
            f"bgp {AS_A}",
            "router-id 1.1.1.1",
            "timer connect-retry 5",
            f"neighbor {B_LOOP_V4} as {AS_B}",
            f"neighbor {B_LOOP_V4} source-interface loop{A_LOOP_ID}",
            f"neighbor {B_LOOP_V4} ebgp-multihop 5",
            "af ipv4-unicast",
            f"neighbor {B_LOOP_V4} enable",
            "import-route static",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="b",
        commands=[
            "config",
            f"bgp {AS_B}",
            "router-id 2.2.2.2",
            "timer connect-retry 5",
            f"neighbor {A_LOOP_V4} as {AS_A}",
            f"neighbor {A_LOOP_V4} source-interface loop{B_LOOP_ID}",
            f"neighbor {A_LOOP_V4} ebgp-multihop 5",
            "af ipv4-unicast",
            f"neighbor {A_LOOP_V4} enable",
            "exit",
            "end",
        ],
    )
    wait_checks(
        rt,
        [
            {
                "device": "a",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [_established_regex(B_LOOP_V4)],
                "label": "a loopback eBGP session established",
            },
            {
                "device": "b",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [_established_regex(A_LOOP_V4)],
                "label": "b loopback eBGP session established",
            },
        ],
        timeout=70,
        interval=2,
    )


def _inject_test_route(rt: TopologyRuntime, *, a_ge1_peer: str) -> None:
    step("Inject one static route on a for BGP advertisement")
    run_cmds(
        rt=rt,
        device="a",
        commands=[
            "config",
            f"route static ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN} {a_ge1_peer}",
            "end",
        ],
    )


def _wait_bgp_route_and_get_nh_id(rt: TopologyRuntime) -> str:
    command = f"show bgp route af ipv4-unicast {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}"
    wait_check(
        rt,
        device="b",
        command=command,
        timeout=60,
        interval=2,
        contains=[f"BGP Route Detail: {TEST_PREFIX}", f"NextHop  : {A_LOOP_V4}", "Valid    : Yes"],
        regex=[r"(?im)^\s*NH-ID\s*:\s*[1-9][0-9]*\s*$"],
        label="b learned test route with a-loop BGP nexthop",
    )
    return _extract_nh_id(cmd(rt, "b", command), command=command)


def _wait_route_path(
    rt: TopologyRuntime,
    *,
    nh_id: str,
    relay: str,
    iter_oif: str,
    stale_relay: str | None = None,
    expected_updated: str | None = None,
    timeout: int,
) -> str:
    command = f"show route ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}"
    regex = [
        rf"(?is)Path\s*\[\d+\]\s*:\s*bgp\b.*?"
        rf"Nexthop\s*:\s*{re.escape(A_LOOP_V4)}\s*.*?"
        rf"NH-ID\s*:\s*{re.escape(nh_id)}\s*.*?"
        rf"Iter NH\s*:\s*{re.escape(relay)}\s*.*?"
        rf"Iter OIF\s*:\s*{re.escape(iter_oif)}\s*"
    ]
    if expected_updated is not None:
        regex.append(rf"(?im)^\s*Updated\s*:\s*{re.escape(expected_updated)}\s*$")

    not_regex = []
    if stale_relay is not None:
        not_regex.append(
            rf"(?is)Path\s*\[\d+\]\s*:\s*bgp\b.*?NH-ID\s*:\s*{re.escape(nh_id)}\s*.*?"
            rf"Iter NH\s*:\s*{re.escape(stale_relay)}\s*"
        )

    wait_check(
        rt,
        device="b",
        command=command,
        timeout=timeout,
        interval=2,
        regex=regex,
        not_regex=not_regex,
        label=f"b ROUTE path uses NH-ID {nh_id} via relay {relay}",
    )
    return _extract_field(cmd(rt, "b", command), "Updated", command=command)


def _wait_nexthop_objects(
    rt: TopologyRuntime,
    *,
    nh_id: str,
    relay: str,
    stale_relay: str | None = None,
    timeout: int,
) -> None:
    route_cmd = f"show route nexthop ipv4 id {nh_id}"
    fib_cmd = f"show fib nexthop ipv4 id {nh_id}"

    route_regex = [
        rf"(?im)^\s*{re.escape(nh_id)}\s+0\s+ipv4\s+bgp\s+{re.escape(A_LOOP_V4)}\s+"
        rf"{re.escape(relay)}\s+[1-9][0-9]*\s+[0-9]+\s+[0-9]+\s*$"
    ]
    fib_regex = [
        rf"(?im)^\s*{re.escape(nh_id)}\s+0\s+ipv4\s+ip\s+up\s+{re.escape(relay)}\s+[1-9][0-9]*\s*$"
    ]
    route_not_regex = []
    fib_not_regex = []
    if stale_relay is not None:
        route_not_regex.append(
            rf"(?im)^\s*{re.escape(nh_id)}\s+0\s+ipv4\s+bgp\s+{re.escape(A_LOOP_V4)}\s+"
            rf"{re.escape(stale_relay)}\s+"
        )
        fib_not_regex.append(
            rf"(?im)^\s*{re.escape(nh_id)}\s+0\s+ipv4\s+ip\s+up\s+{re.escape(stale_relay)}\s+"
        )

    wait_checks(
        rt,
        [
            {
                "device": "b",
                "command": route_cmd,
                "regex": route_regex,
                "not_regex": route_not_regex,
                "contains": ["Total 1 nexthop(s)"],
                "label": f"b ROUTE nexthop object {nh_id} relay {relay}",
            },
            {
                "device": "b",
                "command": fib_cmd,
                "regex": fib_regex,
                "not_regex": fib_not_regex,
                "contains": ["Total 1 nexthop(s)"],
                "label": f"b FIB nexthop object {nh_id} gateway {relay}",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_fib_route_reference(rt: TopologyRuntime, *, nh_id: str, relay: str, stale_relay: str | None, timeout: int) -> None:
    command = f"show fib ipv4 {TEST_PREFIX_ADDR} {TEST_PREFIX_LEN}"
    regex = [
        rf"(?im)^\s*NH-ID\s*:\s*{re.escape(nh_id)}\s*$",
        rf"(?im)^\s*Iter NH\s*:\s*{re.escape(relay)}\s*$",
        r"(?im)^\s*Installed\s*:\s*yes\s*$",
        r"(?im)^\s*Skip OS\s*:\s*no\s*$",
    ]
    not_regex = []
    if stale_relay is not None:
        not_regex.append(rf"(?im)^\s*Iter NH\s*:\s*{re.escape(stale_relay)}\s*$")

    wait_check(
        rt,
        device="b",
        command=command,
        timeout=timeout,
        interval=2,
        regex=regex,
        not_regex=not_regex,
        label=f"b FIB route still references NH-ID {nh_id} with relay {relay}",
    )


def _switch_b_resolution_to_ge2(rt: TopologyRuntime, *, b_ge1_peer: str, b_ge2_peer: str) -> None:
    step("Change only b's recursive static metrics so a-loop resolves through GE-2")
    run_cmds(
        rt=rt,
        device="b",
        commands=[
            "config",
            f"route static ipv4 {A_LOOP_V4} {HOST_LEN} {b_ge1_peer} metric 200",
            f"route static ipv4 {A_LOOP_V4} {HOST_LEN} {b_ge2_peer} metric 10",
            "end",
        ],
    )
    wait_check(
        rt,
        device="b",
        command=f"show route ipv4 {A_LOOP_V4} {HOST_LEN}",
        timeout=30,
        interval=2,
        regex=[
            rf"(?is)Nexthop\s*:\s*{re.escape(b_ge1_peer)}\b.*?Metric\s*:\s*200\b",
            rf"(?is)Nexthop\s*:\s*{re.escape(b_ge2_peer)}\b.*?Metric\s*:\s*10\b",
        ],
        label="b recursive static route metrics switched to GE-2",
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("a", "b"))

    a_ge1_peer = str(g_top.a.GE_1.peer_ip)
    a_ge2_peer = str(g_top.a.GE_2.peer_ip)
    b_ge1_peer = str(g_top.b.GE_1.peer_ip)
    b_ge2_peer = str(g_top.b.GE_2.peer_ip)

    try:
        _cleanup(rt, a_ge1_peer=a_ge1_peer, a_ge2_peer=a_ge2_peer, b_ge1_peer=b_ge1_peer, b_ge2_peer=b_ge2_peer)
        _configure_loopbacks(rt)
        _configure_underlay_static(
            rt, a_ge1_peer=a_ge1_peer, a_ge2_peer=a_ge2_peer, b_ge1_peer=b_ge1_peer, b_ge2_peer=b_ge2_peer
        )
        _configure_bgp(rt)
        _inject_test_route(rt, a_ge1_peer=a_ge1_peer)

        step("Verify initial learned route uses one BGP NH-ID resolved through GE-1")
        nh_id = _wait_bgp_route_and_get_nh_id(rt)
        initial_updated = _wait_route_path(rt, nh_id=nh_id, relay=b_ge1_peer, iter_oif="GE-1", timeout=60)
        _wait_nexthop_objects(rt, nh_id=nh_id, relay=b_ge1_peer, timeout=40)
        _wait_fib_route_reference(rt, nh_id=nh_id, relay=b_ge1_peer, stale_relay=None, timeout=40)

        # Make a route re-add observable in the second-resolution "Updated" field.
        time.sleep(2)

        _switch_b_resolution_to_ge2(rt, b_ge1_peer=b_ge1_peer, b_ge2_peer=b_ge2_peer)

        step("Verify nexthop value refreshes to GE-2 without re-adding the BGP route path")
        _wait_route_path(
            rt,
            nh_id=nh_id,
            relay=b_ge2_peer,
            iter_oif="GE-2",
            stale_relay=b_ge1_peer,
            expected_updated=initial_updated,
            timeout=60,
        )
        _wait_nexthop_objects(rt, nh_id=nh_id, relay=b_ge2_peer, stale_relay=b_ge1_peer, timeout=40)
        _wait_fib_route_reference(rt, nh_id=nh_id, relay=b_ge2_peer, stale_relay=b_ge1_peer, timeout=40)

    finally:
        if not should_skip_cleanup():
            _cleanup(rt, a_ge1_peer=a_ge1_peer, a_ge2_peer=a_ge2_peer, b_ge1_peer=b_ge1_peer, b_ge2_peer=b_ge2_peer)

    print("BGP nexthop value refresh check passed.")
