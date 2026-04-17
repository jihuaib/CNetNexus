#!/usr/bin/env python3
"""
BGP packed-UPDATE bulk routes check.

Goal:
- r1 and r2 establish one eBGP session, r1 importing static routes
- r1 injects a batch of ~50 static routes while there is no subgroup drain yet
  (done before the session catchup fires / while peer is ready to receive)
- the subgroup packer (bgp_pkt_build_packed_update) must pack all 50 NLRIs
  sharing the same (attr, nh) into ONE UPDATE message
- r2 must therefore see its RX UPDATE counter grow by exactly 1, and learn
  all 50 prefixes in its BGP RIB

RX UPDATE counter is surfaced by the recently-added bgp_msg_rx_stats_t via
`show bgp neighbor af ipv4-unicast <ip>`.
"""

from __future__ import annotations

import re

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


ROUTE_COUNT = 50


def _prefix_octets(i: int) -> tuple[int, int]:
    """Return (second, third) octets for the i-th test /24 in 10.x.y.0/24."""
    # 0-based index i mapped into (second, third) inside 10.?.?.0/24; avoid
    # 10.12.0.0/30 (inter-router link). Start from 10.90.0.0/24 and go on.
    base_second = 90
    second = base_second + (i // 200)
    third = i % 200
    return second, third


def _prefix_str(i: int) -> str:
    second, third = _prefix_octets(i)
    return f"10.{second}.{third}.0/24"


def _prefix_cli_args(i: int) -> tuple[str, str]:
    second, third = _prefix_octets(i)
    return f"10.{second}.{third}.0", "24"


RX_UPDATE_RE = re.compile(r"(?im)^\s*UPDATE\s*:\s*(\d+)\s*$")
RX_TOTAL_RE = re.compile(r"(?im)^\s*Total\s*:\s*(\d+)\s*$")
RX_NOTIF_RE = re.compile(r"(?im)^\s*NOTIFICATION\s*:\s*(\d+)\s*$")
RX_UNKNOWN_RE = re.compile(r"(?im)^\s*Unknown\s*:\s*(\d+)\s*$")


def _get_rx_update_count(rt: TopologyRuntime, *, device: str, peer_ip: str) -> int:
    output = cmd(
        rt,
        device,
        f"show bgp neighbor af ipv4-unicast {peer_ip}",
        strict=False,
    )
    if "Received Messages:" not in output:
        raise RuntimeError(f"{device} neighbor detail missing Received Messages section:\n{output}")
    m_notif = RX_NOTIF_RE.search(output)
    m_unk = RX_UNKNOWN_RE.search(output)
    if not m_notif or not m_unk:
        raise RuntimeError(f"{device} neighbor detail missing NOTIFICATION/Unknown lines:\n{output}")
    if int(m_notif.group(1)) != 0:
        raise RuntimeError(f"{device} unexpected NOTIFICATION RX:\n{output}")
    if int(m_unk.group(1)) != 0:
        raise RuntimeError(f"{device} unexpected Unknown RX:\n{output}")
    m = RX_UPDATE_RE.search(output)
    if not m:
        raise RuntimeError(f"{device} neighbor detail missing UPDATE line:\n{output}")
    return int(m.group(1))


def _cleanup(rt: TopologyRuntime, *, r1_nh: str) -> None:
    step("Cleanup BGP/static config")
    r1_cmds: list[str] = ["config"]
    for i in range(ROUTE_COUNT):
        addr, mask = _prefix_cli_args(i)
        r1_cmds.append(f"no route ipv4 {addr} {mask} {r1_nh}")
    r1_cmds.extend(["no bgp", "end"])
    run_cmds(rt=rt, device="r1", strict=False, commands=r1_cmds)
    run_cmds(rt=rt, device="r2", strict=False, commands=["config", "no bgp", "end"])


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r1_static_nh = r1_peer_ip

    try:
        step(f"Configure eBGP base (r1-r2) and preload {ROUTE_COUNT} static routes on r1 before peering")

        # Configure BGP instances but do NOT enable the eBGP neighbor yet,
        # so the subgroup has no ready session to drain — all imported NLRIs
        # will accumulate in the subgroup's adj-rib-out and get flushed to the
        # peer as a single packed UPDATE right after ESTABLISHED.
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                "timer connect-retry 5",
                "af ipv4-unicast",
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
                "bgp 65002",
                "router-id 2.2.2.2",
                "timer connect-retry 5",
                "af ipv4-unicast",
                "exit",
                "end",
            ],
        )

        step(f"Inject {ROUTE_COUNT} static routes on r1 (under single config block)")
        inject_cmds: list[str] = ["config"]
        for i in range(ROUTE_COUNT):
            addr, mask = _prefix_cli_args(i)
            inject_cmds.append(f"route ipv4 {addr} {mask} {r1_static_nh}")
        inject_cmds.append("end")
        run_cmds(rt=rt, device="r1", commands=inject_cmds)

        step(f"Wait all {ROUTE_COUNT} prefixes appear in r1 BGP RIB")
        # Spot-check a handful of prefixes (first/middle/last) for speed.
        sampled_prefixes = [
            _prefix_str(0),
            _prefix_str(ROUTE_COUNT // 2),
            _prefix_str(ROUTE_COUNT - 1),
        ]
        wait_check(
            rt,
            device="r1",
            command="show bgp route af ipv4-unicast",
            timeout=40,
            interval=2,
            contains=sampled_prefixes,
            regex=[rf"(?im)^\s*Total\s*:\s*{ROUTE_COUNT}\s+networks\b"],
            label=f"r1 BGP RIB has {ROUTE_COUNT} imported prefixes",
        )

        step("Now enable eBGP neighbor — session comes up with full RIB waiting to be packed")
        run_cmds(
            rt=rt,
            device="r1",
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
            commands=[
                "config",
                "bgp 65002",
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "exit",
                "end",
            ],
        )

        step("Wait eBGP session established")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1->r2 established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r2->r1 established",
                },
            ],
            timeout=40,
        )

        step(f"Wait r2 to learn all {ROUTE_COUNT} prefixes via one packed UPDATE")
        wait_check(
            rt,
            device="r2",
            command="show bgp route af ipv4-unicast",
            timeout=40,
            interval=2,
            contains=sampled_prefixes,
            regex=[rf"(?im)^\s*Total\s*:\s*{ROUTE_COUNT}\s+networks\b"],
            label=f"r2 learned {ROUTE_COUNT} prefixes",
        )

        step("Verify r2 received exactly ONE UPDATE message (packed advertisement)")
        rx_updates = _get_rx_update_count(rt, device="r2", peer_ip=r2_peer_ip)
        print(f"r2 RX UPDATE count = {rx_updates}")
        if rx_updates != 1:
            # Dump both sides' details to aid debugging.
            r2_detail = cmd(
                rt, "r2", f"show bgp neighbor af ipv4-unicast {r2_peer_ip}", strict=False
            )
            r1_ug = cmd(rt, "r1", "show bgp update-group af ipv4-unicast", strict=False)
            raise RuntimeError(
                f"expected r2 RX UPDATE == 1 (packed advertisement), got {rx_updates}.\n"
                f"--- r2 neighbor detail ---\n{r2_detail}\n"
                f"--- r1 update-group ---\n{r1_ug}\n"
            )

        step("Spot-check every injected prefix is present on r2")
        all_routes_output = cmd(rt, "r2", "show bgp route af ipv4-unicast", strict=False)
        missing: list[str] = []
        for i in range(ROUTE_COUNT):
            pfx = _prefix_str(i)
            if pfx not in all_routes_output:
                missing.append(pfx)
        if missing:
            raise RuntimeError(
                f"r2 missing {len(missing)} prefix(es) out of {ROUTE_COUNT}: "
                f"{missing[:10]}...\n--- full output ---\n{all_routes_output}"
            )

        step("Re-check r2 UPDATE counter did not grow after verification reads")
        rx_updates_after = _get_rx_update_count(rt, device="r2", peer_ip=r2_peer_ip)
        if rx_updates_after != rx_updates:
            raise RuntimeError(
                f"r2 UPDATE RX counter changed unexpectedly during verification: "
                f"{rx_updates} -> {rx_updates_after}"
            )

        print(
            f"BGP packed-UPDATE bulk routes check passed: {ROUTE_COUNT} prefixes learned via "
            f"{rx_updates} UPDATE message(s)."
        )
    finally:
        _cleanup(rt, r1_nh=r1_static_nh)
