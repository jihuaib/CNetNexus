#!/usr/bin/env python3
"""
BGP update-group two-peer nexthop check (IPv4).

Goal:
- build two eBGP peers on r2 (r1<->r2 and r2<->r3)
- import one local static route on r2
- verify r2 update-group state shows two eBGP groups (one neighbor each)
- verify r1/r3 learned route nexthop equals each peer's neighbor address
"""

from __future__ import annotations

import re

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


AS_R1 = "65001"
AS_R2 = "65002"
AS_R3 = "65003"

TEST_PFX_ADDR = "10.77.77.0"
TEST_PFX_MASK = "24"
TEST_PFX = f"{TEST_PFX_ADDR}/{TEST_PFX_MASK}"


def _established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"


def _ug_detail_peer_established_regex(peer: str) -> str:
    return rf"(?im)^\s*{re.escape(peer)}\s+\d+\s+Established\s+\S+\s*$"


def _cleanup(rt: TopologyRuntime, *, route_nh: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route static ipv4 {TEST_PFX_ADDR} {TEST_PFX_MASK} {route_nh}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(rt=rt, device="r1", strict=False, commands=["config", "no bgp", "end"])
    run_cmds(rt=rt, device="r3", strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    # r1 <-> r2 (GE-1 <-> GE-1)
    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)     # 10.12.0.2 (r2 GE-1)
    r2_to_r1_v4 = str(g_top.r2.GE_1.peer_ip)    # 10.12.0.1 (r1 GE-1)

    # r2 <-> r3 (GE-2 <-> GE-1)
    r2_to_r3_v4 = str(g_top.r2.GE_2.peer_ip)    # 10.23.0.2 (r3 GE-1)
    r3_peer_v4 = str(g_top.r3.GE_1.peer_ip)     # 10.23.0.1 (r2 GE-2)

    summary_row_re = r"(?im)^\s*(\d+)\s+eBGP\s+\d+\s+1\s+\d+\s+\d+\s+\d+\s+0x[0-9A-Fa-f]+\s*$"
    route_nh = r2_to_r1_v4

    try:
        step("Configure BGP base")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", f"bgp {AS_R1}", "router-id 1.1.1.1", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"bgp {AS_R2}", "router-id 2.2.2.2", "end"],
        )
        run_cmds(
            rt=rt,
            device="r3",
            strict=False,
            commands=["config", f"bgp {AS_R3}", "router-id 3.3.3.3", "end"],
        )

        step("Configure two eBGP peers on r2 and enable IPv4 AF")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"bgp {AS_R1}",
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
            strict=False,
            commands=[
                "config",
                f"bgp {AS_R2}",
                f"neighbor {r2_to_r1_v4} as {AS_R1}",
                f"neighbor {r2_to_r3_v4} as {AS_R3}",
                "af ipv4-unicast",
                f"neighbor {r2_to_r1_v4} enable",
                f"neighbor {r2_to_r3_v4} enable",
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
                "af ipv4-unicast",
                f"neighbor {r3_peer_v4} enable",
                "exit",
                "end",
            ],
        )

        step("Wait two eBGP sessions on r2 established")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_established_regex(r2_to_r1_v4), _established_regex(r2_to_r3_v4)],
                    "label": "r2 has two established ipv4 peers",
                },
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_established_regex(r1_peer_v4)],
                    "label": "r1->r2 established",
                },
                {
                    "device": "r3",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [_established_regex(r3_peer_v4)],
                    "label": "r3->r2 established",
                },
            ],
            timeout=40,
        )

        step("Inject one static route on r2 for local import-route")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"route static ipv4 {TEST_PFX_ADDR} {TEST_PFX_MASK} {route_nh}", "end"],
        )

        step("Wait r1/r3 learn the route from r2")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [TEST_PFX],
                    "label": "r1 learned route",
                },
                {
                    "device": "r3",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [TEST_PFX],
                    "label": "r3 learned route",
                },
            ],
            timeout=40,
        )

        step("Verify r1/r3 learned route nexthop equals neighbor address")
        wait_check(
            rt,
            device="r1",
            command=f"show bgp route af ipv4-unicast {TEST_PFX_ADDR} {TEST_PFX_MASK}",
            timeout=30,
            regex=[
                rf"(?im)^BGP Route Detail:\s*{re.escape(TEST_PFX)}\s+\(AF:\s*ipv4-unicast\)\s*$",
                rf"(?im)^\s*NextHop\s*:\s*{re.escape(r1_peer_v4)}\s*$",
                rf"(?im)^.*From Peer\s*:\s*{re.escape(r1_peer_v4)}\s*$",
            ],
            label="r1 route nexthop=neighbor(r2)",
        )
        wait_check(
            rt,
            device="r3",
            command=f"show bgp route af ipv4-unicast {TEST_PFX_ADDR} {TEST_PFX_MASK}",
            timeout=30,
            regex=[
                rf"(?im)^BGP Route Detail:\s*{re.escape(TEST_PFX)}\s+\(AF:\s*ipv4-unicast\)\s*$",
                rf"(?im)^\s*NextHop\s*:\s*{re.escape(r3_peer_v4)}\s*$",
                rf"(?im)^.*From Peer\s*:\s*{re.escape(r3_peer_v4)}\s*$",
            ],
            label="r3 route nexthop=neighbor(r2)",
        )

        step("Verify r2 update-group summary has two eBGP groups (one neighbor each)")
        wait_check(
            rt,
            device="r2",
            command="show bgp update-group af ipv4-unicast",
            timeout=30,
            contains=["BGP Update-Groups (AF: ipv4-unicast)", "Group-ID", "eBGP"],
            regex=[
                summary_row_re,
                r"(?im)^Total:\s*2\s+groups,\s*2\s+subgroups,\s*2\s+neighbors,\s*2\s+adj-rib-out entries\s*$",
            ],
            label="r2 update-group summary (2 groups x 1 neighbor)",
        )

        summary_out = cmd(rt, "r2", "show bgp update-group af ipv4-unicast", strict=False)
        group_ids = [int(m.group(1)) for m in re.finditer(summary_row_re, summary_out, flags=re.MULTILINE)]
        if len(group_ids) != 2:
            raise RuntimeError(
                f"expected exactly 2 eBGP groups with 1 neighbor each, got ids={group_ids}\n{summary_out}"
            )

        step(f"Verify r2 update-group detail for each group ({group_ids})")
        seen_peers: set[str] = set()
        target_peers = (r2_to_r1_v4, r2_to_r3_v4)

        for group_id in group_ids:
            detail_out = cmd(rt, "r2", f"show bgp update-group af ipv4-unicast {group_id}", strict=False)

            for pattern in (
                rf"(?im)^BGP Update-Group Detail\s+\(AF:\s*ipv4-unicast,\s*Group-ID:\s*{group_id}\)\s*$",
                r"(?im)^\s*Session-Type\s*:\s*eBGP\s*$",
                r"(?im)^\s*Neighbors\s*:\s*1\s*$",
                r"(?im)^\s*Subgroups\s*:\s*[1-9]\d*\s*$",
                r"(?im)^\s*Adj-RIB-Out\s*:\s*[1-9]\d*\s*$",
            ):
                if re.search(pattern, detail_out, flags=re.MULTILINE) is None:
                    raise RuntimeError(f"group-id={group_id} detail mismatch, missing /{pattern}/:\n{detail_out}")

            matched_peers = [p for p in target_peers if re.search(_ug_detail_peer_established_regex(p), detail_out)]
            if len(matched_peers) != 1:
                raise RuntimeError(
                    f"group-id={group_id} expected exactly one established target peer, got {matched_peers}:\n{detail_out}"
                )
            seen_peers.add(matched_peers[0])

        if seen_peers != set(target_peers):
            raise RuntimeError(f"expected both target peers in group details, got {sorted(seen_peers)}")

        print("BGP update-group two-peer nexthop check passed.")
    finally:
        _cleanup(rt, route_nh=route_nh)
