#!/usr/bin/env python3
"""
BGP update-group switch matrix check (dual-stack).

Topology: r1 --- r2 --- r3

Goal:
- Build dual-stack eBGP sessions on r2 (r1<->r2 and r2<->r3)
- Verify update-group membership is peer-based and AF-independent
- Exercise multiple switch scenarios:
  1) baseline: IPv4/IPv6 each has two peers in update-group
  2) disable one IPv4 peer only, verify IPv6 group unaffected
  3) disable one IPv6 peer only, verify IPv4 group unaffected
  4) re-enable all peers, verify both AFs recover
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


AS_R1 = "65101"
AS_R2 = "65102"
AS_R3 = "65103"

V4_PREFIX_ADDR = "10.77.88.0"
V4_PREFIX_LEN = "24"
V4_PREFIX = f"{V4_PREFIX_ADDR}/{V4_PREFIX_LEN}"

V6_PREFIX_ADDR = "2001:db8:7788::"
V6_PREFIX_LEN = "64"
V6_PREFIX = f"{V6_PREFIX_ADDR}/{V6_PREFIX_LEN}"


SUMMARY_ROW_RE = re.compile(
    r"(?im)^\s*(\d+)\s+(?:iBGP|eBGP|Unknown)\s+\d+\s+\d+\s+\d+\s+\d+\s+\d+\s+0x[0-9A-Fa-f]+\s*$"
)


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _summary_group_ids(summary: str) -> list[int]:
    return [int(m.group(1)) for m in SUMMARY_ROW_RE.finditer(summary)]


def _collect_update_group_text(rt: TopologyRuntime, af: str) -> tuple[str, str]:
    summary = cmd(rt, "r2", f"show bgp update-group af {af}", strict=False)
    details: list[str] = []
    for gid in _summary_group_ids(summary):
        details.append(cmd(rt, "r2", f"show bgp update-group af {af} {gid}", strict=False))
    return summary, "\n".join(details)


def _wait_update_group_state(
    rt: TopologyRuntime,
    *,
    af: str,
    expected_neighbors: int,
    must_have: list[str],
    must_not_have: list[str],
    timeout: int = 40,
    interval: int = 2,
) -> None:
    wait_check(
        rt,
        device="r2",
        command=f"show bgp update-group af {af}",
        timeout=timeout,
        regex=[
            rf"(?im)^Total:\s*\d+\s+groups,\s*\d+\s+subgroups,\s*{expected_neighbors}\s+neighbors,\s*\d+\s+adj-rib-out entries\s*$"
        ],
        label=f"r2 update-group total neighbors ({af}) = {expected_neighbors}",
    )

    deadline = time.time() + timeout
    last_summary = ""
    last_detail = ""
    while time.time() < deadline:
        summary, detail = _collect_update_group_text(rt, af)
        text = summary + "\n" + detail
        ok_have = all(token in text for token in must_have)
        ok_not = all(token not in text for token in must_not_have)
        if ok_have and ok_not:
            return
        last_summary = summary
        last_detail = detail
        time.sleep(interval)

    raise RuntimeError(
        f"update-group detail mismatch (af={af})\n"
        f"must_have={must_have}\n"
        f"must_not_have={must_not_have}\n\n"
        f"last summary:\n{last_summary}\n\n"
        f"last details:\n{last_detail}"
    )


def _cleanup(
    rt: TopologyRuntime,
    *,
    r2_nh4: str,
    r2_nh6: str,
) -> None:
    step("Cleanup update-group switch config")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {V4_PREFIX_ADDR} {V4_PREFIX_LEN} {r2_nh4}",
            f"no route static ipv6 {V6_PREFIX_ADDR} {V6_PREFIX_LEN} {r2_nh6}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(rt=rt, device="r1", strict=False, commands=["config", "no bgp", "end"])
    run_cmds(rt=rt, device="r3", strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    # r1 <-> r2
    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)    # r2 GE-1 v4
    r1_peer_v6 = str(g_top.r1.GE_1.peer_ip6)   # r2 GE-1 v6
    r2_to_r1_v4 = str(g_top.r2.GE_1.peer_ip)   # r1 GE-1 v4
    r2_to_r1_v6 = str(g_top.r2.GE_1.peer_ip6)  # r1 GE-1 v6

    # r2 <-> r3
    r2_to_r3_v4 = str(g_top.r2.GE_2.peer_ip)   # r3 GE-1 v4
    r2_to_r3_v6 = str(g_top.r2.GE_2.peer_ip6)  # r3 GE-1 v6
    r3_peer_v4 = str(g_top.r3.GE_1.peer_ip)    # r2 GE-2 v4
    r3_peer_v6 = str(g_top.r3.GE_1.peer_ip6)   # r2 GE-2 v6

    try:
        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", strict=False, commands=["config", f"bgp {AS_R1}", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", strict=False, commands=["config", f"bgp {AS_R2}", "router-id 2.2.2.2", "end"])
        run_cmds(rt=rt, device="r3", strict=False, commands=["config", f"bgp {AS_R3}", "router-id 3.3.3.3", "end"])

        step("Configure dual-stack peers and AF enable")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"bgp {AS_R1}",
                f"neighbor {r1_peer_v4} as {AS_R2}",
                f"neighbor {r1_peer_v6} as {AS_R2}",
                "af ipv4-unicast",
                f"neighbor {r1_peer_v4} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r1_peer_v6} enable",
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
                f"bgp {AS_R2}",
                f"neighbor {r2_to_r1_v4} as {AS_R1}",
                f"neighbor {r2_to_r1_v6} as {AS_R1}",
                f"neighbor {r2_to_r3_v4} as {AS_R3}",
                f"neighbor {r2_to_r3_v6} as {AS_R3}",
                "af ipv4-unicast",
                f"neighbor {r2_to_r1_v4} enable",
                f"neighbor {r2_to_r3_v4} enable",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_to_r1_v6} enable",
                f"neighbor {r2_to_r3_v6} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r3",
            strict=False,
            commands=[
                "config",
                f"bgp {AS_R3}",
                f"neighbor {r3_peer_v4} as {AS_R2}",
                f"neighbor {r3_peer_v6} as {AS_R2}",
                "af ipv4-unicast",
                f"neighbor {r3_peer_v4} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r3_peer_v6} enable",
                "exit",
                "end",
            ],
        )

        step("Wait baseline dual-stack sessions established")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_established_regex(r2_to_r1_v4), _established_regex(r2_to_r3_v4)],
                    "label": "r2 ipv4 both peers established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "regex": [_established_regex(r2_to_r1_v6), _established_regex(r2_to_r3_v6)],
                    "label": "r2 ipv6 both peers established",
                },
            ],
            timeout=50,
        )

        step("Inject static routes on r2 (IPv4 + IPv6)")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route static ipv4 {V4_PREFIX_ADDR} {V4_PREFIX_LEN} {r2_to_r1_v4}",
                f"route static ipv6 {V6_PREFIX_ADDR} {V6_PREFIX_LEN} {r2_to_r1_v6}",
                "end",
            ],
        )

        step("Wait r1/r3 learn baseline routes")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [V4_PREFIX],
                    "label": "r1 learned IPv4 route",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [V6_PREFIX],
                    "label": "r1 learned IPv6 route",
                },
                {
                    "device": "r3",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [V4_PREFIX],
                    "label": "r3 learned IPv4 route",
                },
                {
                    "device": "r3",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [V6_PREFIX],
                    "label": "r3 learned IPv6 route",
                },
            ],
            timeout=50,
        )

        step("Scenario-1: baseline update-group members (v4/v6 both 2 peers)")
        _wait_update_group_state(
            rt,
            af="ipv4-unicast",
            expected_neighbors=2,
            must_have=[r2_to_r1_v4, r2_to_r3_v4],
            must_not_have=[],
        )
        _wait_update_group_state(
            rt,
            af="ipv6-unicast",
            expected_neighbors=2,
            must_have=[r2_to_r1_v6, r2_to_r3_v6],
            must_not_have=[],
        )

        step("Scenario-2: disable r2<->r3 IPv4 peer only")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"bgp {AS_R2}", "af ipv4-unicast", f"no neighbor {r2_to_r3_v4} enable", "exit", "end"],
        )
        run_cmds(
            rt=rt,
            device="r3",
            strict=False,
            commands=["config", f"bgp {AS_R3}", "af ipv4-unicast", f"no neighbor {r3_peer_v4} enable", "exit", "end"],
        )

        wait_check(
            rt,
            device="r2",
            command="show bgp neighbor af ipv4-unicast",
            timeout=40,
            regex=[_established_regex(r2_to_r1_v4)],
            not_regex=[_established_regex(r2_to_r3_v4)],
            label="r2 ipv4 only r1 peer remains",
        )

        _wait_update_group_state(
            rt,
            af="ipv4-unicast",
            expected_neighbors=1,
            must_have=[r2_to_r1_v4],
            must_not_have=[r2_to_r3_v4],
        )
        _wait_update_group_state(
            rt,
            af="ipv6-unicast",
            expected_neighbors=2,
            must_have=[r2_to_r1_v6, r2_to_r3_v6],
            must_not_have=[],
        )

        wait_check(
            rt,
            device="r3",
            command="show bgp route af ipv4-unicast",
            timeout=40,
            not_contains=[V4_PREFIX],
            label="r3 IPv4 route withdrawn after ipv4 peer disable",
        )
        wait_check(
            rt,
            device="r3",
            command="show bgp route af ipv6-unicast",
            timeout=40,
            contains=[V6_PREFIX],
            label="r3 IPv6 route still present",
        )

        step("Scenario-3: disable r2<->r1 IPv6 peer only")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"bgp {AS_R2}", "af ipv6-unicast", f"no neighbor {r2_to_r1_v6} enable", "exit", "end"],
        )
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", f"bgp {AS_R1}", "af ipv6-unicast", f"no neighbor {r1_peer_v6} enable", "exit", "end"],
        )

        wait_check(
            rt,
            device="r2",
            command="show bgp neighbor af ipv6-unicast",
            timeout=40,
            regex=[_established_regex(r2_to_r3_v6)],
            not_regex=[_established_regex(r2_to_r1_v6)],
            label="r2 ipv6 only r3 peer remains",
        )

        _wait_update_group_state(
            rt,
            af="ipv6-unicast",
            expected_neighbors=1,
            must_have=[r2_to_r3_v6],
            must_not_have=[r2_to_r1_v6],
        )
        _wait_update_group_state(
            rt,
            af="ipv4-unicast",
            expected_neighbors=1,
            must_have=[r2_to_r1_v4],
            must_not_have=[r2_to_r3_v4],
        )

        wait_check(
            rt,
            device="r1",
            command="show bgp route af ipv6-unicast",
            timeout=40,
            not_contains=[V6_PREFIX],
            label="r1 IPv6 route withdrawn after ipv6 peer disable",
        )
        wait_check(
            rt,
            device="r1",
            command="show bgp route af ipv4-unicast",
            timeout=40,
            contains=[V4_PREFIX],
            label="r1 IPv4 route still present",
        )

        step("Scenario-4: re-enable removed peers and verify full recovery")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"bgp {AS_R2}",
                "af ipv4-unicast",
                f"neighbor {r2_to_r3_v4} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_to_r1_v6} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r3",
            strict=False,
            commands=["config", f"bgp {AS_R3}", "af ipv4-unicast", f"neighbor {r3_peer_v4} enable", "exit", "end"],
        )
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", f"bgp {AS_R1}", "af ipv6-unicast", f"neighbor {r1_peer_v6} enable", "exit", "end"],
        )

        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_established_regex(r2_to_r1_v4), _established_regex(r2_to_r3_v4)],
                    "label": "r2 ipv4 recovered both peers",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv6-unicast",
                    "regex": [_established_regex(r2_to_r1_v6), _established_regex(r2_to_r3_v6)],
                    "label": "r2 ipv6 recovered both peers",
                },
            ],
            timeout=50,
        )

        _wait_update_group_state(
            rt,
            af="ipv4-unicast",
            expected_neighbors=2,
            must_have=[r2_to_r1_v4, r2_to_r3_v4],
            must_not_have=[],
        )
        _wait_update_group_state(
            rt,
            af="ipv6-unicast",
            expected_neighbors=2,
            must_have=[r2_to_r1_v6, r2_to_r3_v6],
            must_not_have=[],
        )

        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [V6_PREFIX],
                    "label": "r1 IPv6 route recovered",
                },
                {
                    "device": "r3",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [V4_PREFIX],
                    "label": "r3 IPv4 route recovered",
                },
            ],
            timeout=50,
        )

        print("BGP update-group dual-stack switch matrix check passed.")
    finally:
        _cleanup(rt, r2_nh4=r2_to_r1_v4, r2_nh6=r2_to_r1_v6)

