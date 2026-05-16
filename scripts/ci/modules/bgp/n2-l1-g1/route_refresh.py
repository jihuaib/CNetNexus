#!/usr/bin/env python3
"""
BGP Route Refresh (RFC 2918) CI check.

Goal:
- Bring up an eBGP session r1<->r2 with route-refresh capability negotiated
- Have r2 export static routes into BGP and announce them to r1
- Issue `refresh bgp <peer> import af ipv4-unicast` on r1 -> verify r2 receives
  a ROUTE-REFRESH and the rx counter on r2 / tx counter on r1 increments
- Issue `refresh bgp <peer> export af ipv4-unicast` on r2 -> verify the local
  Adj-RIB-Out is re-advertised (UPDATE tx counter on r2 increments)
- Repeat for ipv6-unicast
- Negative path: disable route-refresh capability on r1 then attempt
  `refresh bgp <peer> import` -> command must fail with the negotiation error.
"""

from __future__ import annotations

import re
import time

from module_api import cmd, g_top, mark_step_failed, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TEST_V4_PREFIX_ADDR = "10.30.30.0"
TEST_V4_PREFIX_LEN = "24"
TEST_V4_PREFIX = f"{TEST_V4_PREFIX_ADDR}/{TEST_V4_PREFIX_LEN}"

TEST_V6_PREFIX_ADDR = "2001:db8:3030::"
TEST_V6_PREFIX_LEN = "64"
TEST_V6_PREFIX = f"{TEST_V6_PREFIX_ADDR}/{TEST_V6_PREFIX_LEN}"


def _cleanup(rt: TopologyRuntime, r2_route_nh: str, r2_route_nh6: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN} {r2_route_nh}",
            f"no route ipv6 {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN} {r2_route_nh6}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["config", "no bgp", "end"],
    )


def _parse_counter(output: str, section: str, msg_name: str) -> int:
    """Return the counter value for a given section ("Received Messages" / "Sent Messages")."""
    section_re = re.search(
        rf"{re.escape(section)}:\s*(.*?)(?:\r?\n\s*\r?\n|\Z)", output, re.DOTALL
    )
    if not section_re:
        return -1
    block = section_re.group(1)
    line = re.search(rf"^\s*{re.escape(msg_name)}\s*:\s*(\d+)\s*$", block, re.MULTILINE)
    if not line:
        return -1
    return int(line.group(1))


def _counters(rt: TopologyRuntime, device: str, neighbor_ip: str, af: str) -> dict[str, int]:
    output = cmd(rt, device, f"show bgp neighbor af {af} {neighbor_ip}")
    return {
        "rx_refresh": _parse_counter(output, "Received Messages", "ROUTE-REFRESH"),
        "tx_refresh": _parse_counter(output, "Sent Messages", "ROUTE-REFRESH"),
        "rx_update": _parse_counter(output, "Received Messages", "UPDATE"),
        "tx_update": _parse_counter(output, "Sent Messages", "UPDATE"),
    }


def _wait_counters(
    rt: TopologyRuntime,
    device: str,
    neighbor_ip: str,
    af: str,
    *,
    expectations: dict[str, int],
    timeout: int = 20,
    label: str = "",
) -> None:
    """Poll counters until each key in `expectations` reaches at least the target value."""
    deadline = time.time() + timeout
    last: dict[str, int] = {}
    while time.time() < deadline:
        cur = _counters(rt, device, neighbor_ip, af)
        last = cur
        if all(cur.get(k, -1) >= v for k, v in expectations.items()):
            return
        time.sleep(1)
    mark_step_failed()
    raise RuntimeError(
        f"counter expectations not met on {device}/{neighbor_ip}/{af} ({label}); "
        f"want={expectations} got={last}"
    )


def _session_checks(r1_peer_ip: str, r2_peer_ip: str, r1_peer_ip6: str, r2_peer_ip6: str) -> list[dict]:
    return [
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv4-unicast",
            "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r1->r2 v4 established",
        },
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv6-unicast",
            "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r1->r2 v6 established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv4-unicast",
            "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r2->r1 v4 established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv6-unicast",
            "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip6)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r2->r1 v6 established",
        },
    ]


def _refresh(
    rt: TopologyRuntime,
    device: str,
    peer_ip: str,
    direction: str,
    af: str,
    *,
    strict: bool = True,
) -> str:
    """Issue `refresh bgp neighbor ...` from user view (the command is only available there)."""
    cmd(rt, device, "end", strict=False)
    return cmd(rt, device, f"refresh bgp neighbor {peer_ip} {direction} af {af}", strict=strict)


def _assert_rr_negotiated(rt: TopologyRuntime, device: str, peer_ip: str, af: str) -> None:
    out = cmd(rt, device, f"show bgp neighbor af {af} {peer_ip}")
    line = re.search(r"(?m)^\s*Route-Refresh\s+\S+\s+\S+\s+(\S+)\s*$", out)
    if not line or line.group(1).strip() != "Yes":
        raise RuntimeError(
            f"{device}/{peer_ip}/{af}: Route-Refresh not negotiated; "
            f"got: {line.group(0) if line else '<missing>'}"
        )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r1_peer_ip6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_peer_ip6 = str(g_top.r2.GE_1.peer_ip6)
    r2_route_nh = str(g_top.r2.GE_1.peer_ip)
    r2_route_nh6 = str(g_top.r2.GE_1.peer_ip6)

    try:
        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

        step("Configure dual-stack neighbors and import-route static on r2")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_ip} as 65002",
                f"neighbor {r1_peer_ip6} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r1_peer_ip6} enable",
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
                f"neighbor {r2_peer_ip6} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_peer_ip6} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Inject static routes on r2")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"route ipv4 {TEST_V4_PREFIX_ADDR} {TEST_V4_PREFIX_LEN} {r2_route_nh}",
                f"route ipv6 {TEST_V6_PREFIX_ADDR} {TEST_V6_PREFIX_LEN} {r2_route_nh6}",
                "end",
            ],
        )

        step("Wait sessions established")
        wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip, r1_peer_ip6, r2_peer_ip6), timeout=40)

        step("Wait r1 learns prefixes from r2")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [TEST_V4_PREFIX],
                    "label": "r1 has 10.30.30.0/24",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv6-unicast",
                    "contains": [TEST_V6_PREFIX],
                    "label": "r1 has 2001:db8:3030::/64",
                },
            ],
            timeout=40,
        )

        step("Verify Route-Refresh capability negotiated both ways")
        _assert_rr_negotiated(rt, "r1", r1_peer_ip, "ipv4-unicast")
        _assert_rr_negotiated(rt, "r2", r2_peer_ip, "ipv4-unicast")

        step("ipv4 import: r1 sends ROUTE-REFRESH, r2 receives it and re-announces")
        c0_r1 = _counters(rt, "r1", r1_peer_ip, "ipv4-unicast")
        c0_r2 = _counters(rt, "r2", r2_peer_ip, "ipv4-unicast")
        _refresh(rt, "r1", r1_peer_ip, "import", "ipv4-unicast")
        _wait_counters(
            rt,
            "r1",
            r1_peer_ip,
            "ipv4-unicast",
            expectations={"tx_refresh": c0_r1["tx_refresh"] + 1},
            label="r1 tx_refresh",
        )
        _wait_counters(
            rt,
            "r2",
            r2_peer_ip,
            "ipv4-unicast",
            expectations={
                "rx_refresh": c0_r2["rx_refresh"] + 1,
                "tx_update": c0_r2["tx_update"] + 1,
            },
            label="r2 rx_refresh + tx_update",
        )

        step("ipv4 export on r2: re-advertise from Adj-RIB-Out (UPDATE tx incr)")
        before = _counters(rt, "r2", r2_peer_ip, "ipv4-unicast")
        _refresh(rt, "r2", r2_peer_ip, "export", "ipv4-unicast")
        _wait_counters(
            rt,
            "r2",
            r2_peer_ip,
            "ipv4-unicast",
            expectations={"tx_update": before["tx_update"] + 1},
            label="r2 ipv4 export tx_update",
        )

        step("ipv6 import: r1 sends ROUTE-REFRESH, r2 receives it and re-announces")
        c0_r1v6 = _counters(rt, "r1", r1_peer_ip6, "ipv6-unicast")
        c0_r2v6 = _counters(rt, "r2", r2_peer_ip6, "ipv6-unicast")
        _refresh(rt, "r1", r1_peer_ip6, "import", "ipv6-unicast")
        _wait_counters(
            rt,
            "r1",
            r1_peer_ip6,
            "ipv6-unicast",
            expectations={"tx_refresh": c0_r1v6["tx_refresh"] + 1},
            label="r1 ipv6 tx_refresh",
        )
        _wait_counters(
            rt,
            "r2",
            r2_peer_ip6,
            "ipv6-unicast",
            expectations={
                "rx_refresh": c0_r2v6["rx_refresh"] + 1,
                "tx_update": c0_r2v6["tx_update"] + 1,
            },
            label="r2 ipv6 rx_refresh + tx_update",
        )

        step("ipv6 export on r2")
        before_v6 = _counters(rt, "r2", r2_peer_ip6, "ipv6-unicast")
        _refresh(rt, "r2", r2_peer_ip6, "export", "ipv6-unicast")
        _wait_counters(
            rt,
            "r2",
            r2_peer_ip6,
            "ipv6-unicast",
            expectations={"tx_update": before_v6["tx_update"] + 1},
            label="r2 ipv6 export tx_update",
        )

        step("Negative: disable Route-Refresh on r1, import command must fail")
        # Toggling the OPEN capability sends NOTIFICATION + Admin-Reset and tears down
        # the session — wait for the new TCP/OPEN handshake to finish AND verify the
        # negotiated Route-Refresh column flipped to No before testing the refresh command.
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                "bgp 65001",
                f"no neighbor {r1_peer_ip} open-capability route-refresh",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1 session listed as Established after toggle",
                },
                {
                    "device": "r1",
                    "command": f"show bgp neighbor af ipv4-unicast {r1_peer_ip}",
                    "regex": [r"(?m)^\s*Route-Refresh\s+No\s+\S+\s+No\s*$"],
                    "label": "r1 Route-Refresh negotiated=No",
                },
            ],
            timeout=60,
        )

        out = _refresh(rt, "r1", r1_peer_ip, "import", "ipv4-unicast", strict=False)
        if "Route Refresh capability not negotiated" not in out:
            raise RuntimeError(
                f"refresh import without negotiated capability must fail; got: {out!r}"
            )

        # Restore for clean shutdown
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_ip} open-capability route-refresh",
                "end",
            ],
        )

        print("BGP route-refresh check passed.")
    finally:
        _cleanup(rt, r2_route_nh, r2_route_nh6)
