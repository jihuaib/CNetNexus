#!/usr/bin/env python3
"""
BGP dual-stack mixed nexthop preference check with reboot.

Goal:
- Configure both IPv4 and IPv6 neighbors on r1/r2
- Enable both neighbors under AF ipv4-unicast and AF ipv6-unicast
- Enable import-route static on r2 for both AFs
- Inject one IPv4 static route and one IPv6 static route on r2
- Verify r1 ipv4-unicast receives two BGP paths for the IPv4 prefix:
  one nexthop IPv4 and one nexthop IPv6, and prefers IPv4 nexthop
- Verify Route RIB and OS route on r1 use IPv4 nexthop
- Reboot r1 and verify all checks still hold
"""

from __future__ import annotations

import re

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_check, wait_checks, wait_fib_ipv4_route, wait_fib_ipv6_route  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


IPV4_PREFIX_ADDR = "10.66.66.0"
IPV4_MASK = "24"
IPV4_PREFIX = f"{IPV4_PREFIX_ADDR}/{IPV4_MASK}"

IPV6_PREFIX_ADDR = "2001:db8:6666::"
IPV6_MASK = "64"
IPV6_PREFIX = f"{IPV6_PREFIX_ADDR}/{IPV6_MASK}"


def _as_6pe_nexthop(ipv4_addr: str) -> str:
    return f"::ffff:{ipv4_addr}"


def _session_checks(
    r1_peer_v4: str,
    r1_peer_v6: str,
    r2_peer_v4: str,
    r2_peer_v6: str,
) -> list[dict[str, object]]:
    def _peer_row(peer: str, state: str) -> str:
        return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+{re.escape(state)}\s*$"

    return [
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r1_peer_v4],
            "regex": [_peer_row(r1_peer_v4, "Established")],
            "label": "r1 ipv4-unicast v4-peer established",
        },
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r1_peer_v6],
            "regex": [_peer_row(r1_peer_v6, "Established")],
            "label": "r1 ipv4-unicast v6-peer established",
        },
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [r1_peer_v4],
            "regex": [_peer_row(r1_peer_v4, "Established")],
            "label": "r1 ipv6-unicast v4-peer established",
        },
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [r1_peer_v6],
            "regex": [_peer_row(r1_peer_v6, "Established")],
            "label": "r1 ipv6-unicast v6-peer established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r2_peer_v4],
            "regex": [_peer_row(r2_peer_v4, "Established")],
            "label": "r2 ipv4-unicast v4-peer established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r2_peer_v6],
            "regex": [_peer_row(r2_peer_v6, "Established")],
            "label": "r2 ipv4-unicast v6-peer established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [r2_peer_v4],
            "regex": [_peer_row(r2_peer_v4, "Established")],
            "label": "r2 ipv6-unicast v4-peer established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv6-unicast",
            "contains": [r2_peer_v6],
            "regex": [_peer_row(r2_peer_v6, "Established")],
            "label": "r2 ipv6-unicast v6-peer established",
        },
    ]


def _ext_nexthop_capability_checks(
    r1_peer_v4: str,
    r1_peer_v6: str,
    r2_peer_v4: str,
    r2_peer_v6: str,
) -> list[dict[str, object]]:
    ext_nh_yes_row = r"(?im)^\s*Extended-Nexthop\s+Yes\s+Yes\s+Yes\s*$"
    ext_nh_no_row = r"(?im)^\s*Extended-Nexthop\s+No\s+No\s+No\s*$"
    return [
        {
            "device": "r1",
            "command": f"show bgp neighbor af ipv4-unicast {r1_peer_v6}",
            "contains": [r1_peer_v6, "Capabilities:", "Extended-Nexthop"],
            "regex": [ext_nh_yes_row],
            "label": "r1 ipv4-unicast v6-peer ext-nexthop negotiated",
        },
        {
            "device": "r1",
            "command": f"show bgp neighbor af ipv6-unicast {r1_peer_v4}",
            "contains": [r1_peer_v4, "Capabilities:", "Extended-Nexthop"],
            "regex": [ext_nh_no_row],
            "label": "r1 ipv6-unicast v4-peer ext-nexthop absent",
        },
        {
            "device": "r2",
            "command": f"show bgp neighbor af ipv4-unicast {r2_peer_v6}",
            "contains": [r2_peer_v6, "Capabilities:", "Extended-Nexthop"],
            "regex": [ext_nh_yes_row],
            "label": "r2 ipv4-unicast v6-peer ext-nexthop negotiated",
        },
        {
            "device": "r2",
            "command": f"show bgp neighbor af ipv6-unicast {r2_peer_v4}",
            "contains": [r2_peer_v4, "Capabilities:", "Extended-Nexthop"],
            "regex": [ext_nh_no_row],
            "label": "r2 ipv6-unicast v4-peer ext-nexthop absent",
        },
    ]


def _wait_ipv4_bgp_dual_nexthop(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    nh4: str,
    nh6: str,
    timeout: int,
) -> None:
    row_v4 = rf"(?im)(?:^[>v ]*{re.escape(prefix)}\s+{re.escape(nh4)}\s+|^\s*[>v ]+\s+{re.escape(nh4)}\s+)"
    # 部分版本 show 表格会将第二条路径打印为“续行仅 NextHop”，不重复前缀列。
    row_v6 = rf"(?im)(?:^[>v ]*{re.escape(prefix)}\s+{re.escape(nh6)}\s+|^\s*[>v ]+\s+{re.escape(nh6)}\s+)"
    wait_check(
        rt,
        device=device,
        command="show bgp route af ipv4-unicast",
        timeout=timeout,
        interval=2,
        contains=[prefix],
        regex=[r"(?im)Paths:\s*2\b", row_v4, row_v6],
        label=f"{device} ipv4-unicast has v4/v6 nexthop paths for {prefix}",
    )


def _wait_route_best_ipv4_backup_ipv6(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    nh4: str,
    nh6: str,
    timeout: int,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {IPV4_MASK}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for"],
        regex=[
            rf"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Nexthop\s*:\s*{re.escape(nh4)}\b.*?Preference\s*:\s*200\b"
        ],
        label=f"{device} route best-v4 {destination}",
    )


def _wait_route_best_ipv4_bgp_nexthop(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    nh: str,
    timeout: int,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {IPV4_MASK}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for"],
        regex=[rf"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Nexthop\s*:\s*{re.escape(nh)}\b.*?Preference\s*:\s*200\b"],
        label=f"{device} route best-v4 {destination} via {nh}",
    )


def _wait_route_iter_ipv4(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    iter_nh: str,
    iter_oif: str,
    timeout: int,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {IPV4_MASK}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for"],
        regex=[
            rf"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Iter\s+NH\s*:\s*{re.escape(iter_nh)}\b.*?Iter\s+OIF\s*:\s*{re.escape(iter_oif)}\b"
        ],
        label=f"{device} route iter-v4 {destination} via {iter_nh}/{iter_oif}",
    )
    wait_fib_ipv4_route(
        rt,
        device=device,
        prefix_addr=destination,
        prefix_len=IPV4_MASK,
        expect_present=True,
        nexthop=iter_nh,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} fib v4 {destination}/{IPV4_MASK} via {iter_nh}",
    )


def _wait_ipv6_bgp_dual_nexthop(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    nh4: str,
    nh6: str,
    timeout: int,
) -> None:
    row_v4 = rf"(?im)(?:^[>v ]*{re.escape(prefix)}\s+{re.escape(nh4)}\s+|^\s*[>v ]+\s+{re.escape(nh4)}\s+)"
    row_v6 = rf"(?im)(?:^[>v ]*{re.escape(prefix)}\s+{re.escape(nh6)}\s+|^\s*[>v ]+\s+{re.escape(nh6)}\s+)"
    wait_check(
        rt,
        device=device,
        command="show bgp route af ipv6-unicast",
        timeout=timeout,
        interval=2,
        contains=[prefix],
        regex=[r"(?im)Paths:\s*2\b", row_v4, row_v6],
        label=f"{device} ipv6-unicast has v4/v6 nexthop paths for {prefix}",
    )


def _wait_ipv4_bgp_single_nexthop(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    nh: str,
    timeout: int,
) -> None:
    row_nh = rf"(?im)(?:^[>v ]*{re.escape(prefix)}\s+{re.escape(nh)}\s+|^\s*[>v ]+\s+{re.escape(nh)}\s+)"
    wait_check(
        rt,
        device=device,
        command="show bgp route af ipv4-unicast",
        timeout=timeout,
        interval=2,
        contains=[prefix],
        regex=[r"(?im)Paths:\s*1\b", row_nh],
        label=f"{device} ipv4-unicast single nexthop {nh} for {prefix}",
    )


def _wait_ipv6_bgp_single_nexthop(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    nh: str,
    timeout: int,
) -> None:
    row_nh = rf"(?im)(?:^[>v ]*{re.escape(prefix)}\s+{re.escape(nh)}\s+|^\s*[>v ]+\s+{re.escape(nh)}\s+)"
    wait_check(
        rt,
        device=device,
        command="show bgp route af ipv6-unicast",
        timeout=timeout,
        interval=2,
        contains=[prefix],
        regex=[r"(?im)Paths:\s*1\b", row_nh],
        label=f"{device} ipv6-unicast single nexthop {nh} for {prefix}",
    )


def _wait_os_best_ipv4_nexthop(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    gateway: str,
    timeout: int,
) -> None:
    best_row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gateway)}\s+\S+\s+bgp\s+\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show fib ipv4 os",
        timeout=timeout,
        interval=2,
        regex=[best_row_regex],
        label=f"{device} os-best {prefix} via {gateway}",
    )


def _wait_route_best_ipv6_bgp(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    timeout: int,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv6 {destination} {IPV6_MASK}",
        timeout=timeout,
        interval=2,
        contains=["Routing entry for"],
        regex=[r"(?is)Path\s*\[1\]\s*:\s*bgp\b.*?Preference\s*:\s*200\b"],
        label=f"{device} route best-v6 {destination}",
    )


def _wait_os_ipv6_bgp_route(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    timeout: int,
) -> None:
    best_row_regex = rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+\S+\s+\S+\s+bgp\s+\d+\s*$"
    wait_check(
        rt,
        device=device,
        command="show fib ipv6 os",
        timeout=timeout,
        interval=2,
        regex=[best_row_regex],
        label=f"{device} os-has-bgp-v6 {prefix}",
    )
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_fib_ipv6_route(
        rt,
        device=device,
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=True,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} fib-has-bgp-v6 {prefix}",
    )


def _af_local_and_received_checks() -> list[dict[str, object]]:
    return [
        {
            "device": "r2",
            "command": "show bgp route af ipv4-unicast",
            "contains": [IPV4_PREFIX],
            "label": "r2 af-ipv4 local imported route",
        },
        {
            "device": "r2",
            "command": "show bgp route af ipv6-unicast",
            "contains": [IPV6_PREFIX],
            "label": "r2 af-ipv6 local imported route",
        },
    ]


def _wait_peer_after_r2_v4_neighbor_removed(
    rt: TopologyRuntime,
    *,
    r1_peer_v4: str,
    r1_peer_v6: str,
    r2_peer_v4: str,
    r2_peer_v6: str,
    timeout: int,
) -> None:
    def _peer_row(peer: str, state: str) -> str:
        return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+{re.escape(state)}\s*$"

    def _peer_any_row(peer: str) -> str:
        return rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+\S+\s*$"

    wait_checks(
        rt,
        [
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv4-unicast",
                "contains": [r2_peer_v6],
                "regex": [_peer_row(r2_peer_v6, "Established")],
                "not_regex": [_peer_any_row(r2_peer_v4)],
                "label": "r2 ipv4-unicast v4-peer removed from af",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv6-unicast",
                "contains": [r2_peer_v6],
                "regex": [_peer_row(r2_peer_v6, "Established")],
                "not_regex": [_peer_any_row(r2_peer_v4)],
                "label": "r2 ipv6-unicast v4-peer removed from af",
            },
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv4-unicast",
                "contains": [r1_peer_v4, r1_peer_v6],
                "regex": [_peer_row(r1_peer_v6, "Established"), _peer_any_row(r1_peer_v4)],
                "not_regex": [rf"(?im)^\s*{re.escape(r1_peer_v4)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1 ipv4-unicast sees v4-peer not-established",
            },
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv6-unicast",
                "contains": [r1_peer_v4, r1_peer_v6],
                "regex": [_peer_row(r1_peer_v6, "Established"), _peer_any_row(r1_peer_v4)],
                "not_regex": [rf"(?im)^\s*{re.escape(r1_peer_v4)}\s+\S+\s+\S+\s+Established\s*$"],
                "label": "r1 ipv6-unicast sees v4-peer not-established",
            },
        ],
        timeout=timeout,
    )


def _cleanup_case_config(
    rt: TopologyRuntime,
    *,
    r2_nh4: str,
    r2_nh6: str,
) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {IPV4_PREFIX_ADDR} {IPV4_MASK} {r2_nh4}",
            f"no route ipv6 {IPV6_PREFIX_ADDR} {IPV6_MASK} {r2_nh6}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_peer_v4 = str(g_top.r1.GE_1.peer_ip)
    r1_peer_v6 = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_v4 = str(g_top.r2.GE_1.peer_ip)
    r2_peer_v6 = str(g_top.r2.GE_1.peer_ip6)
    r1_peer_v4_6pe = _as_6pe_nexthop(r1_peer_v4)
    r2_peer_v4_6pe = _as_6pe_nexthop(r2_peer_v4)

    session_checks = _session_checks(r1_peer_v4, r1_peer_v6, r2_peer_v4, r2_peer_v6)
    ext_nh_checks = _ext_nexthop_capability_checks(r1_peer_v4, r1_peer_v6, r2_peer_v4, r2_peer_v6)
    af_route_checks = _af_local_and_received_checks()

    try:
        step("Cleanup stale config")
        _cleanup_case_config(rt, r2_nh4=r2_peer_v4, r2_nh6=r2_peer_v6)

        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", strict=False, commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"])
        run_cmds(rt=rt, device="r2", strict=False, commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"])

        step("Configure dual-stack neighbors and enable both AFs on both neighbors")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"neighbor {r1_peer_v4} as 65002",
                f"neighbor {r1_peer_v6} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_v4} enable",
                f"neighbor {r1_peer_v6} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r1_peer_v4} enable",
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
                "bgp 65002",
                f"neighbor {r2_peer_v4} as 65001",
                f"neighbor {r2_peer_v6} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_v4} enable",
                f"neighbor {r2_peer_v6} enable",
                "import-route static",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_peer_v4} enable",
                f"neighbor {r2_peer_v6} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Wait BGP sessions for both AF and both neighbor families")
        wait_checks(rt, session_checks, timeout=60)
        step("Verify peer verbose reports Extended-Nexthop negotiated on IPv6 peers")
        wait_checks(rt, ext_nh_checks, timeout=40)

        step("Inject IPv4+IPv6 static routes on r2 for import-route")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                f"route ipv4 {IPV4_PREFIX_ADDR} {IPV4_MASK} {r2_peer_v4}",
                f"route ipv6 {IPV6_PREFIX_ADDR} {IPV6_MASK} {r2_peer_v6}",
                "end",
            ],
        )

        step("Verify r2 local routes (af-ipv4/af-ipv6)")
        wait_checks(rt, af_route_checks, timeout=40)
        step("Verify r1 af-ipv6 receives route with 6PE/v6 nexthops")
        _wait_ipv6_bgp_dual_nexthop(
            rt, device="r1", prefix=IPV6_PREFIX, nh4=r1_peer_v4_6pe, nh6=r1_peer_v6, timeout=40
        )
        step("Verify r1 Route RIB has IPv6 BGP route")
        _wait_route_best_ipv6_bgp(rt, device="r1", destination=IPV6_PREFIX_ADDR, timeout=40)
        step("Verify r1 OS table has IPv6 BGP route")
        _wait_os_ipv6_bgp_route(rt, device="r1", prefix=IPV6_PREFIX, timeout=40)

        step("Verify r1 ipv4-unicast receives IPv4 prefix with v4/v6 nexthops")
        _wait_ipv4_bgp_dual_nexthop(rt, device="r1", prefix=IPV4_PREFIX, nh4=r1_peer_v4, nh6=r1_peer_v6, timeout=40)

        step("Verify r1 Route RIB prefers IPv4 nexthop for IPv4 prefix")
        _wait_route_best_ipv4_backup_ipv6(
            rt,
            device="r1",
            destination=IPV4_PREFIX_ADDR,
            nh4=r1_peer_v4,
            nh6=r1_peer_v6,
            timeout=40,
        )
        _wait_route_iter_ipv4(
            rt,
            device="r1",
            destination=IPV4_PREFIX_ADDR,
            iter_nh=r1_peer_v4,
            iter_oif="GE-1",
            timeout=40,
        )

        step("Verify r1 OS route is installed via iterated IPv4 nexthop")
        _wait_os_best_ipv4_nexthop(
            rt,
            device="r1",
            prefix=IPV4_PREFIX,
            gateway=r1_peer_v4,
            timeout=40,
        )

        step("Before reboot, remove r2 IPv4 neighbor from both AFs")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "af ipv4-unicast",
                f"no neighbor {r2_peer_v4} enable",
                "exit",
                "af ipv6-unicast",
                f"no neighbor {r2_peer_v4} enable",
                "exit",
                "end",
            ],
        )

        step("Verify peer status after removing r2 IPv4 neighbor")
        _wait_peer_after_r2_v4_neighbor_removed(
            rt,
            r1_peer_v4=r1_peer_v4,
            r1_peer_v6=r1_peer_v6,
            r2_peer_v4=r2_peer_v4,
            r2_peer_v6=r2_peer_v6,
            timeout=40,
        )

        step("Verify r1 routes fall back to IPv6-only nexthop path")
        _wait_ipv4_bgp_single_nexthop(rt, device="r1", prefix=IPV4_PREFIX, nh=r1_peer_v6, timeout=40)
        _wait_ipv6_bgp_single_nexthop(rt, device="r1", prefix=IPV6_PREFIX, nh=r1_peer_v6, timeout=40)
        _wait_route_best_ipv4_bgp_nexthop(rt, device="r1", destination=IPV4_PREFIX_ADDR, nh=r1_peer_v6, timeout=40)
        _wait_route_iter_ipv4(
            rt,
            device="r1",
            destination=IPV4_PREFIX_ADDR,
            iter_nh=r1_peer_v6,
            iter_oif="GE-1",
            timeout=40,
        )
        _wait_os_best_ipv4_nexthop(
            rt,
            device="r1",
            prefix=IPV4_PREFIX,
            gateway=r1_peer_v6,
            timeout=40,
        )
        _wait_route_best_ipv6_bgp(rt, device="r1", destination=IPV6_PREFIX_ADDR, timeout=40)
        _wait_os_ipv6_bgp_route(rt, device="r1", prefix=IPV6_PREFIX, timeout=40)

        step("Re-configure r2 IPv4 neighbor and re-verify peer/routes")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "af ipv4-unicast",
                f"neighbor {r2_peer_v4} enable",
                "exit",
                "af ipv6-unicast",
                f"neighbor {r2_peer_v4} enable",
                "exit",
                "end",
            ],
        )
        wait_checks(rt, session_checks, timeout=60)
        _wait_ipv6_bgp_dual_nexthop(
            rt, device="r1", prefix=IPV6_PREFIX, nh4=r1_peer_v4_6pe, nh6=r1_peer_v6, timeout=40
        )
        _wait_route_best_ipv6_bgp(rt, device="r1", destination=IPV6_PREFIX_ADDR, timeout=40)
        _wait_os_ipv6_bgp_route(rt, device="r1", prefix=IPV6_PREFIX, timeout=40)
        _wait_ipv4_bgp_dual_nexthop(rt, device="r1", prefix=IPV4_PREFIX, nh4=r1_peer_v4, nh6=r1_peer_v6, timeout=40)
        _wait_route_best_ipv4_backup_ipv6(
            rt,
            device="r1",
            destination=IPV4_PREFIX_ADDR,
            nh4=r1_peer_v4,
            nh6=r1_peer_v6,
            timeout=40,
        )
        _wait_route_iter_ipv4(
            rt,
            device="r1",
            destination=IPV4_PREFIX_ADDR,
            iter_nh=r1_peer_v4,
            iter_oif="GE-1",
            timeout=40,
        )
        _wait_os_best_ipv4_nexthop(
            rt,
            device="r1",
            prefix=IPV4_PREFIX,
            gateway=r1_peer_v4,
            timeout=40,
        )

        step("Reboot r1 and wait CLI reconnect")
        reboot_device(rt, "r1", timeout=120)

        step("Wait BGP sessions after reboot")
        wait_checks(rt, session_checks, timeout=60)
        step("Re-verify peer verbose Extended-Nexthop negotiation after reboot")
        wait_checks(rt, ext_nh_checks, timeout=40)
        step("Re-verify r2 local routes after reboot")
        wait_checks(rt, af_route_checks, timeout=40)
        step("Re-verify r1 af-ipv6 receives route with 6PE/v6 nexthops after reboot")
        _wait_ipv6_bgp_dual_nexthop(
            rt, device="r1", prefix=IPV6_PREFIX, nh4=r1_peer_v4_6pe, nh6=r1_peer_v6, timeout=40
        )
        step("Re-verify r1 Route RIB has IPv6 BGP route after reboot")
        _wait_route_best_ipv6_bgp(rt, device="r1", destination=IPV6_PREFIX_ADDR, timeout=40)
        step("Re-verify r1 OS table has IPv6 BGP route after reboot")
        _wait_os_ipv6_bgp_route(rt, device="r1", prefix=IPV6_PREFIX, timeout=40)

        step("Re-verify mixed nexthop preference after reboot")
        _wait_ipv4_bgp_dual_nexthop(rt, device="r1", prefix=IPV4_PREFIX, nh4=r1_peer_v4, nh6=r1_peer_v6, timeout=40)
        _wait_route_best_ipv4_backup_ipv6(
            rt,
            device="r1",
            destination=IPV4_PREFIX_ADDR,
            nh4=r1_peer_v4,
            nh6=r1_peer_v6,
            timeout=40,
        )
        _wait_route_iter_ipv4(
            rt,
            device="r1",
            destination=IPV4_PREFIX_ADDR,
            iter_nh=r1_peer_v4,
            iter_oif="GE-1",
            timeout=40,
        )
        _wait_os_best_ipv4_nexthop(
            rt,
            device="r1",
            prefix=IPV4_PREFIX,
            gateway=r1_peer_v4,
            timeout=40,
        )

        print("BGP dual-stack IPv4 mixed-nexthop preference reboot check passed.")
    finally:
        _cleanup_case_config(rt, r2_nh4=r2_peer_v4, r2_nh6=r2_peer_v6)
