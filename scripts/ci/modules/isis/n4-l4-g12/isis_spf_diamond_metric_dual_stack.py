#!/usr/bin/env python3
"""
ISIS SPF diamond best-path switch check (dual-stack).

Goal:
- build 4-node diamond ISIS domain
- verify SPF picks lower-metric branch from r1 to r4
- update interface metric and verify SPF switches to better branch
- verify consistency across ISIS route view, Route RIB view, and OS route view
"""

from __future__ import annotations

import ipaddress
import re
import time

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TAG = 140

R1_IF_R2 = "GE-1"
R1_IF_R3 = "GE-2"
R2_IF_R1 = "GE-1"
R2_IF_R4 = "GE-2"
R3_IF_R1 = "GE-1"
R3_IF_R4 = "GE-2"
R4_IF_R2 = "GE-1"
R4_IF_R3 = "GE-2"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"
R3_NET = "49.0001.0000.0000.0003.00"
R4_NET = "49.0001.0000.0000.0004.00"

R1_LOOP_ID = 11
R4_LOOP_ID = 44

R1_LOOP_V4 = "10.255.1.1"
R1_LOOP_V4_LEN = 32
R4_LOOP_V4 = "10.255.4.4"
R4_LOOP_V4_LEN = 32

R1_LOOP_V6 = "2001:db8:255:1::1"
R1_LOOP_V6_LEN = 128
R4_LOOP_V6 = "2001:db8:255:4::4"
R4_LOOP_V6_LEN = 128

R1_LOOP_V4_PREFIX = f"{R1_LOOP_V4}/{R1_LOOP_V4_LEN}"
R4_LOOP_V4_PREFIX = f"{R4_LOOP_V4}/{R4_LOOP_V4_LEN}"
R1_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R1_LOOP_V6}/{R1_LOOP_V6_LEN}", strict=False))
R4_LOOP_V6_PREFIX = str(ipaddress.ip_network(f"{R4_LOOP_V6}/{R4_LOOP_V6_LEN}", strict=False))

INIT_R1_GE1_METRIC = 10
INIT_R1_GE2_METRIC = 40
SWITCH_R1_GE1_METRIC = 80
SWITCH_R1_GE2_METRIC = 5

R2_TO_R4_METRIC = 10
R3_TO_R4_METRIC = 10
R4_LOOP_METRIC = 10
EXPECT_INIT_R1_TO_R4_METRIC = INIT_R1_GE1_METRIC + R2_TO_R4_METRIC + R4_LOOP_METRIC
EXPECT_SWITCH_R1_TO_R4_METRIC = SWITCH_R1_GE2_METRIC + R3_TO_R4_METRIC + R4_LOOP_METRIC


def _link_prefix(ip: str, prefix: int) -> str:
    net_addr = ipaddress.ip_interface(f"{ip}/{prefix}").network.network_address
    return f"{net_addr}/{prefix}"


def _net_to_sysid(net: str) -> str:
    parts = [p.strip() for p in net.split(".") if p.strip()]
    if len(parts) < 5:
        raise ValueError(f"invalid NET format: {net}")
    return ".".join(parts[2:5]).lower()


def _cleanup_case_config(rt: TopologyRuntime) -> None:
    step("Cleanup stale ISIS/loopback config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no isis {TAG}",
            f"no if loop {R1_LOOP_ID}",
            f"if {R1_IF_R2}",
            "no shutdown",
            "exit",
            f"if {R1_IF_R3}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "end",
            "config",
            f"no isis {TAG}",
            f"if {R2_IF_R1}",
            "no shutdown",
            "exit",
            f"if {R2_IF_R4}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r3",
        strict=False,
        commands=[
            "end",
            "config",
            f"no isis {TAG}",
            f"if {R3_IF_R1}",
            "no shutdown",
            "exit",
            f"if {R3_IF_R4}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r4",
        strict=False,
        commands=[
            "end",
            "config",
            f"no isis {TAG}",
            f"no if loop {R4_LOOP_ID}",
            f"if {R4_IF_R2}",
            "no shutdown",
            "exit",
            f"if {R4_IF_R3}",
            "no shutdown",
            "exit",
            "end",
        ],
    )


def _wait_route_in_rib(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    prefix: str,
    timeout: int = 90,
) -> None:
    prefix_len = prefix.split("/", 1)[1] if "/" in prefix else ("32" if afi == "ipv4" else "128")
    wait_check(
        rt,
        device=device,
        command=f"show route {afi} {destination} {prefix_len}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {prefix}"],
        not_contains=["(no routes)", "(no matching routes)"],
        regex=[r"(?im)^\s*Path\s*\[\d+\]\s*:\s*isis\b"],
        label=f"{device} {afi} {prefix} learned via ISIS",
    )


def _wait_isis_route_best(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    mask: int,
    prefix: str,
    expect_nh: str,
    expect_if: str,
    expect_metric: int,
    timeout: int = 90,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show isis {afi} route {TAG} {destination} {mask}",
        timeout=timeout,
        interval=2,
        contains=[
            f"ISIS {afi} Routes Detail (tag {TAG}, {prefix})",
            f"Prefix       : {prefix}",
        ],
        regex=[
            r"(?im)^\s*Level\s*:\s*L[12]\s*$",
            rf"(?im)^\s*Nexthop\s*:\s*{re.escape(expect_nh)}\s*$",
            rf"(?im)^\s*Out-If\s*:\s*{re.escape(expect_if)}\(\d+\)\s*$",
            rf"(?im)^\s*Metric\s*:\s*{expect_metric}\s*$",
        ],
        label=f"{device} show isis {afi} route best {prefix} via {expect_if} metric {expect_metric}",
    )


def _wait_lsdb_entry(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    remote_sysid: str,
    level: int,
    timeout: int = 60,
) -> None:
    command = f"show isis {afi} lsdb {TAG}"
    prefix_tlv = "135" if afi == "ipv4" else "236"
    deadline = time.time() + timeout
    last_out = ""

    while time.time() < deadline:
        out = cmd(rt, device, command, strict=False)
        last_out = out
        if "ISIS LSDB" not in out or "(no entries)" in out:
            time.sleep(2)
            continue

        blocks = re.split(r"(?im)^\s*LSP Entry\s+\d+\s*$", out)
        for block in blocks:
            if not block.strip():
                continue
            if not re.search(rf"(?im)^\s*Tag\s*:\s*{TAG}\s*$", block):
                continue
            if not re.search(rf"(?im)^\s*Level\s*:\s*L{level}\s*$", block):
                continue
            if not re.search(rf"(?im)^\s*System-ID\s*:\s*{re.escape(remote_sysid)}\s*$", block):
                continue
            if not re.search(r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b", block):
                continue
            if not re.search(rf"(?im)^\s*TLV\[\d+\]\s*:\s*type={prefix_tlv}\b", block):
                continue
            return

        time.sleep(2)

    raise RuntimeError(
        f"{device} show isis {afi} lsdb wait timeout after {timeout}s\n"
        f"expect block: tag={TAG}, level=L{level}, sysid={remote_sysid}, "
        f"TLV22 + TLV{prefix_tlv}\n"
        f"command: {command}\n"
        f"last output:\n{last_out}"
    )


def _wait_rib_best(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    destination: str,
    mask: int,
    prefix: str,
    expect_nh: str,
    expect_if: str,
    stale_nhs: list[str],
    timeout: int = 90,
) -> None:
    stale_regex = [rf"(?im)^\s*Nexthop\s*:\s*{re.escape(nh)}\s*$" for nh in stale_nhs]
    wait_check(
        rt,
        device=device,
        command=f"show route {afi} {destination} {mask}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {prefix}", "Total 1 path(s)"],
        regex=[
            rf"(?is)Path\s*\[1\]\s*:\s*isis\b.*?"
            rf"Nexthop\s*:\s*{re.escape(expect_nh)}\s*.*?"
            rf"Interface\s*:\s*{re.escape(expect_if)}\b"
        ],
        not_regex=stale_regex,
        label=f"{device} show route {afi} best {prefix} via {expect_if}",
    )


def _extract_connected_os_if(output: str, *, prefix: str) -> str | None:
    match = re.search(
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+-\s+(\S+)\s+kernel\s+\d+\s*$",
        output,
    )
    if match is None:
        return None
    return match.group(1)


def _wait_connected_os_if(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    prefix: str,
    timeout: int,
    interval: int = 2,
) -> str:
    deadline = time.time() + timeout
    last_out = ""
    while time.time() < deadline:
        out = cmd(rt, device, f"show route {afi} os", strict=False)
        last_out = out
        os_if = _extract_connected_os_if(out, prefix=prefix)
        if os_if is not None:
            return os_if
        time.sleep(interval)

    raise RuntimeError(
        f"{device} connected OS interface detect timeout after {timeout}s\n"
        f"expect: main unicast {prefix} - <if> kernel\n"
        f"command: show route {afi} os\n"
        f"last output:\n{last_out}"
    )


def _wait_os_best(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    prefix: str,
    expect_gateway: str,
    expect_interface: str,
    stale_gateways: list[str],
    timeout: int,
    interval: int = 2,
) -> None:
    row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+"
        rf"{re.escape(expect_interface)}\s+isis\s+\d+\s*$"
    )
    stale_regex = [
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gw)}\s+\S+\s+isis\s+\d+\s*$"
        for gw in stale_gateways
    ]
    stale_regex.append(
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+"
        rf"(?!{re.escape(expect_interface)}\b)\S+\s+isis\s+\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command=f"show route {afi} os",
        timeout=timeout,
        interval=interval,
        regex=[row_regex],
        not_regex=stale_regex,
        label=f"{device} os {afi} best {prefix} via {expect_gateway} dev {expect_interface}",
    )


def _wait_interface_metric(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    if_name: str,
    metric: int,
    timeout: int = 60,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show isis {afi} interface {TAG}",
        timeout=timeout,
        interval=2,
        contains=[if_name, afi],
        regex=[rf"(?im)^\s*{re.escape(if_name)}\s+{re.escape(afi)}\s+metric=\s*{metric}\b"],
        label=f"{device} isis {afi} interface metric {if_name}={metric}",
    )


def _set_r1_metrics(rt: TopologyRuntime, *, ge1_metric: int, ge2_metric: int) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {R1_IF_R2}",
            f"isis metric {TAG} {ge1_metric}",
            f"isis ipv6 metric {TAG} {ge1_metric}",
            "exit",
            f"if {R1_IF_R3}",
            f"isis metric {TAG} {ge2_metric}",
            f"isis ipv6 metric {TAG} {ge2_metric}",
            "exit",
            "end",
        ],
    )


def _verify_r1_to_r4_best(
    rt: TopologyRuntime,
    *,
    expect_if: str,
    expect_metric: int,
    expect_nh_v4: str,
    expect_nh_v6: str,
    stale_nh_v4: str,
    stale_nh_v6: str,
    expect_os_if: str,
) -> None:
    _wait_isis_route_best(
        rt,
        device="r1",
        afi="ipv4",
        destination=R4_LOOP_V4,
        mask=R4_LOOP_V4_LEN,
        prefix=R4_LOOP_V4_PREFIX,
        expect_nh=expect_nh_v4,
        expect_if=expect_if,
        expect_metric=expect_metric,
    )
    _wait_isis_route_best(
        rt,
        device="r1",
        afi="ipv6",
        destination=R4_LOOP_V6,
        mask=R4_LOOP_V6_LEN,
        prefix=R4_LOOP_V6_PREFIX,
        expect_nh=expect_nh_v6,
        expect_if=expect_if,
        expect_metric=expect_metric,
    )
    _wait_rib_best(
        rt,
        device="r1",
        afi="ipv4",
        destination=R4_LOOP_V4,
        mask=R4_LOOP_V4_LEN,
        prefix=R4_LOOP_V4_PREFIX,
        expect_nh=expect_nh_v4,
        expect_if=expect_if,
        stale_nhs=[stale_nh_v4],
    )
    _wait_rib_best(
        rt,
        device="r1",
        afi="ipv6",
        destination=R4_LOOP_V6,
        mask=R4_LOOP_V6_LEN,
        prefix=R4_LOOP_V6_PREFIX,
        expect_nh=expect_nh_v6,
        expect_if=expect_if,
        stale_nhs=[stale_nh_v6],
    )
    _wait_os_best(
        rt,
        device="r1",
        afi="ipv4",
        prefix=R4_LOOP_V4_PREFIX,
        expect_gateway=expect_nh_v4,
        expect_interface=expect_os_if,
        stale_gateways=[stale_nh_v4],
        timeout=90,
    )
    _wait_os_best(
        rt,
        device="r1",
        afi="ipv6",
        prefix=R4_LOOP_V6_PREFIX,
        expect_gateway=expect_nh_v6,
        expect_interface=expect_os_if,
        stale_gateways=[stale_nh_v6],
        timeout=90,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3", "r4"))

    r4_sysid = _net_to_sysid(R4_NET)

    r1_r2_nh_v4 = str(g_top.r1.GE_1.peer_ip)
    r1_r3_nh_v4 = str(g_top.r1.GE_2.peer_ip)
    r1_r2_nh_v6 = str(g_top.r1.GE_1.peer_ip6)
    r1_r3_nh_v6 = str(g_top.r1.GE_2.peer_ip6)

    r1_ge1_prefix = _link_prefix(str(g_top.r1.GE_1.ip), int(g_top.r1.GE_1.prefix))
    r1_ge2_prefix = _link_prefix(str(g_top.r1.GE_2.ip), int(g_top.r1.GE_2.prefix))

    r1_ge1_show_v4 = f"{g_top.r1.GE_1.ip}/{int(g_top.r1.GE_1.prefix)}"
    r1_ge2_show_v4 = f"{g_top.r1.GE_2.ip}/{int(g_top.r1.GE_2.prefix)}"
    r2_ge2_show_v4 = f"{g_top.r2.GE_2.ip}/{int(g_top.r2.GE_2.prefix)}"
    r3_ge2_show_v4 = f"{g_top.r3.GE_2.ip}/{int(g_top.r3.GE_2.prefix)}"
    r4_ge1_show_v4 = f"{g_top.r4.GE_1.ip}/{int(g_top.r4.GE_1.prefix)}"
    r4_ge2_show_v4 = f"{g_top.r4.GE_2.ip}/{int(g_top.r4.GE_2.prefix)}"

    r1_ge1_show_v6 = f"{g_top.r1.GE_1.ip6}/{int(g_top.r1.GE_1.prefix6)}"
    r1_ge2_show_v6 = f"{g_top.r1.GE_2.ip6}/{int(g_top.r1.GE_2.prefix6)}"
    r2_ge2_show_v6 = f"{g_top.r2.GE_2.ip6}/{int(g_top.r2.GE_2.prefix6)}"
    r3_ge2_show_v6 = f"{g_top.r3.GE_2.ip6}/{int(g_top.r3.GE_2.prefix6)}"
    r4_ge1_show_v6 = f"{g_top.r4.GE_1.ip6}/{int(g_top.r4.GE_1.prefix6)}"
    r4_ge2_show_v6 = f"{g_top.r4.GE_2.ip6}/{int(g_top.r4.GE_2.prefix6)}"

    try:
        _cleanup_case_config(rt)

        step("Ensure diamond underlay interfaces are up")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {R1_IF_R2}",
                    "contains": [
                        f"Interface {R1_IF_R2} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r1_ge1_show_v4}",
                        f"IPv6 Addr  : {r1_ge1_show_v6}",
                    ],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "r1",
                    "command": f"show if {R1_IF_R3}",
                    "contains": [
                        f"Interface {R1_IF_R3} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r1_ge2_show_v4}",
                        f"IPv6 Addr  : {r1_ge2_show_v6}",
                    ],
                    "label": "r1 GE-2 up",
                },
                {
                    "device": "r2",
                    "command": f"show if {R2_IF_R4}",
                    "contains": [
                        f"Interface {R2_IF_R4} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r2_ge2_show_v4}",
                        f"IPv6 Addr  : {r2_ge2_show_v6}",
                    ],
                    "label": "r2 GE-2 up",
                },
                {
                    "device": "r3",
                    "command": f"show if {R3_IF_R4}",
                    "contains": [
                        f"Interface {R3_IF_R4} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r3_ge2_show_v4}",
                        f"IPv6 Addr  : {r3_ge2_show_v6}",
                    ],
                    "label": "r3 GE-2 up",
                },
                {
                    "device": "r4",
                    "command": f"show if {R4_IF_R2}",
                    "contains": [
                        f"Interface {R4_IF_R2} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r4_ge1_show_v4}",
                        f"IPv6 Addr  : {r4_ge1_show_v6}",
                    ],
                    "label": "r4 GE-1 up",
                },
                {
                    "device": "r4",
                    "command": f"show if {R4_IF_R3}",
                    "contains": [
                        f"Interface {R4_IF_R3} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r4_ge2_show_v4}",
                        f"IPv6 Addr  : {r4_ge2_show_v6}",
                    ],
                    "label": "r4 GE-2 up",
                },
            ],
            timeout=40,
            interval=2,
        )

        step("Configure loopback prefixes for ISIS advertisement")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if loop {R1_LOOP_ID}",
                f"ip address {R1_LOOP_V4} {R1_LOOP_V4_LEN}",
                f"ipv6 address {R1_LOOP_V6} {R1_LOOP_V6_LEN}",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r4",
            commands=[
                "config",
                f"if loop {R4_LOOP_ID}",
                f"ip address {R4_LOOP_V4} {R4_LOOP_V4_LEN}",
                f"ipv6 address {R4_LOOP_V6} {R4_LOOP_V6_LEN}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if loop {R1_LOOP_ID}",
                    "contains": [
                        f"Interface loop{R1_LOOP_ID} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {R1_LOOP_V4}/{R1_LOOP_V4_LEN}",
                        f"IPv6 Addr  : {R1_LOOP_V6}/{R1_LOOP_V6_LEN}",
                    ],
                    "label": "r1 loop up",
                },
                {
                    "device": "r4",
                    "command": f"show if loop {R4_LOOP_ID}",
                    "contains": [
                        f"Interface loop{R4_LOOP_ID} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {R4_LOOP_V4}/{R4_LOOP_V4_LEN}",
                        f"IPv6 Addr  : {R4_LOOP_V6}/{R4_LOOP_V6_LEN}",
                    ],
                    "label": "r4 loop up",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure ISIS instance and AF on all routers")
        for device, net in (("r1", R1_NET), ("r2", R2_NET), ("r3", R3_NET), ("r4", R4_NET)):
            run_cmds(
                rt=rt,
                device=device,
                commands=[
                    "config",
                    f"isis {TAG}",
                    f"net {net}",
                    "is-type level-1-2",
                    "af ipv4",
                    "af ipv6",
                    "end",
                ],
            )

        step("Enable ISIS on transit links and loopback passive interfaces")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"if {R1_IF_R2}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis metric {TAG} {INIT_R1_GE1_METRIC}",
                f"isis ipv6 metric {TAG} {INIT_R1_GE1_METRIC}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                f"if {R1_IF_R3}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis metric {TAG} {INIT_R1_GE2_METRIC}",
                f"isis ipv6 metric {TAG} {INIT_R1_GE2_METRIC}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                f"if loop {R1_LOOP_ID}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis passive {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"if {R2_IF_R1}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                f"if {R2_IF_R4}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r3",
            commands=[
                "config",
                f"if {R3_IF_R1}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                f"if {R3_IF_R4}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r4",
            commands=[
                "config",
                f"if {R4_IF_R2}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                f"if {R4_IF_R3}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis ipv6 hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                f"isis ipv6 hold-multiplier {TAG} 3",
                "exit",
                f"if loop {R4_LOOP_ID}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"isis passive {TAG}",
                f"isis ipv6 passive {TAG}",
                "exit",
                "end",
            ],
        )

        step("Verify initial r1 ISIS metrics")
        _wait_interface_metric(rt, device="r1", afi="ipv4", if_name=R1_IF_R2, metric=INIT_R1_GE1_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv6", if_name=R1_IF_R2, metric=INIT_R1_GE1_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv4", if_name=R1_IF_R3, metric=INIT_R1_GE2_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv6", if_name=R1_IF_R3, metric=INIT_R1_GE2_METRIC)

        step("Wait ISIS neighbors up on all routers")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis neighbor {TAG}",
                    "contains": ["ISIS Neighbors", R1_IF_R2, R1_IF_R3],
                    "regex": [
                        rf"(?im)^\s*{TAG}\s+{re.escape(R1_IF_R2)}\s+L[12]\s+\S+\s+Up\s+",
                        rf"(?im)^\s*{TAG}\s+{re.escape(R1_IF_R3)}\s+L[12]\s+\S+\s+Up\s+",
                    ],
                    "label": "r1 ISIS neighbors up",
                },
                {
                    "device": "r2",
                    "command": f"show isis neighbor {TAG}",
                    "contains": ["ISIS Neighbors", R2_IF_R1, R2_IF_R4],
                    "regex": [
                        rf"(?im)^\s*{TAG}\s+{re.escape(R2_IF_R1)}\s+L[12]\s+\S+\s+Up\s+",
                        rf"(?im)^\s*{TAG}\s+{re.escape(R2_IF_R4)}\s+L[12]\s+\S+\s+Up\s+",
                    ],
                    "label": "r2 ISIS neighbors up",
                },
                {
                    "device": "r3",
                    "command": f"show isis neighbor {TAG}",
                    "contains": ["ISIS Neighbors", R3_IF_R1, R3_IF_R4],
                    "regex": [
                        rf"(?im)^\s*{TAG}\s+{re.escape(R3_IF_R1)}\s+L[12]\s+\S+\s+Up\s+",
                        rf"(?im)^\s*{TAG}\s+{re.escape(R3_IF_R4)}\s+L[12]\s+\S+\s+Up\s+",
                    ],
                    "label": "r3 ISIS neighbors up",
                },
                {
                    "device": "r4",
                    "command": f"show isis neighbor {TAG}",
                    "contains": ["ISIS Neighbors", R4_IF_R2, R4_IF_R3],
                    "regex": [
                        rf"(?im)^\s*{TAG}\s+{re.escape(R4_IF_R2)}\s+L[12]\s+\S+\s+Up\s+",
                        rf"(?im)^\s*{TAG}\s+{re.escape(R4_IF_R3)}\s+L[12]\s+\S+\s+Up\s+",
                    ],
                    "label": "r4 ISIS neighbors up",
                },
            ],
            timeout=90,
            interval=2,
        )

        step("Verify r1 LSDB includes r4 L1/L2 entries for IPv4 and IPv6")
        _wait_lsdb_entry(rt, device="r1", afi="ipv4", remote_sysid=r4_sysid, level=1)
        _wait_lsdb_entry(rt, device="r1", afi="ipv4", remote_sysid=r4_sysid, level=2)
        _wait_lsdb_entry(rt, device="r1", afi="ipv6", remote_sysid=r4_sysid, level=1)
        _wait_lsdb_entry(rt, device="r1", afi="ipv6", remote_sysid=r4_sysid, level=2)

        step("Wait ISIS routes learned in route RIB")
        _wait_route_in_rib(rt, device="r1", afi="ipv4", destination=R4_LOOP_V4, prefix=R4_LOOP_V4_PREFIX)
        _wait_route_in_rib(rt, device="r1", afi="ipv6", destination=R4_LOOP_V6, prefix=R4_LOOP_V6_PREFIX)
        _wait_route_in_rib(rt, device="r4", afi="ipv4", destination=R1_LOOP_V4, prefix=R1_LOOP_V4_PREFIX)
        _wait_route_in_rib(rt, device="r4", afi="ipv6", destination=R1_LOOP_V6, prefix=R1_LOOP_V6_PREFIX)

        step("Verify ISIS route list query on r1")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis ipv4 route {TAG}",
                    "contains": [f"ISIS ipv4 Routes (tag {TAG})", "Level", R4_LOOP_V4_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r1 show isis ipv4 route list",
                },
                {
                    "device": "r1",
                    "command": f"show isis ipv6 route {TAG}",
                    "contains": [f"ISIS ipv6 Routes (tag {TAG})", "Level", R4_LOOP_V6_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r1 show isis ipv6 route list",
                },
            ],
            timeout=60,
            interval=2,
        )

        step("Detect r1 OS interface names for GE-1 and GE-2")
        r1_ge1_os_if = _wait_connected_os_if(rt, device="r1", afi="ipv4", prefix=r1_ge1_prefix, timeout=30)
        r1_ge2_os_if = _wait_connected_os_if(rt, device="r1", afi="ipv4", prefix=r1_ge2_prefix, timeout=30)
        print(f"Detected OS interfaces: {R1_IF_R2}->{r1_ge1_os_if}, {R1_IF_R3}->{r1_ge2_os_if}")

        step("Verify initial SPF best path prefers GE-1")
        _verify_r1_to_r4_best(
            rt,
            expect_if=R1_IF_R2,
            expect_metric=EXPECT_INIT_R1_TO_R4_METRIC,
            expect_nh_v4=r1_r2_nh_v4,
            expect_nh_v6=r1_r2_nh_v6,
            stale_nh_v4=r1_r3_nh_v4,
            stale_nh_v6=r1_r3_nh_v6,
            expect_os_if=r1_ge1_os_if,
        )

        step("Raise GE-1 metric and lower GE-2 metric on r1")
        _set_r1_metrics(rt, ge1_metric=SWITCH_R1_GE1_METRIC, ge2_metric=SWITCH_R1_GE2_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv4", if_name=R1_IF_R2, metric=SWITCH_R1_GE1_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv6", if_name=R1_IF_R2, metric=SWITCH_R1_GE1_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv4", if_name=R1_IF_R3, metric=SWITCH_R1_GE2_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv6", if_name=R1_IF_R3, metric=SWITCH_R1_GE2_METRIC)

        step("Verify SPF best path switches to GE-2")
        _verify_r1_to_r4_best(
            rt,
            expect_if=R1_IF_R3,
            expect_metric=EXPECT_SWITCH_R1_TO_R4_METRIC,
            expect_nh_v4=r1_r3_nh_v4,
            expect_nh_v6=r1_r3_nh_v6,
            stale_nh_v4=r1_r2_nh_v4,
            stale_nh_v6=r1_r2_nh_v6,
            expect_os_if=r1_ge2_os_if,
        )

        step("Restore initial metric preference")
        _set_r1_metrics(rt, ge1_metric=INIT_R1_GE1_METRIC, ge2_metric=INIT_R1_GE2_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv4", if_name=R1_IF_R2, metric=INIT_R1_GE1_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv6", if_name=R1_IF_R2, metric=INIT_R1_GE1_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv4", if_name=R1_IF_R3, metric=INIT_R1_GE2_METRIC)
        _wait_interface_metric(rt, device="r1", afi="ipv6", if_name=R1_IF_R3, metric=INIT_R1_GE2_METRIC)

        step("Verify SPF best path switches back to GE-1")
        _verify_r1_to_r4_best(
            rt,
            expect_if=R1_IF_R2,
            expect_metric=EXPECT_INIT_R1_TO_R4_METRIC,
            expect_nh_v4=r1_r2_nh_v4,
            expect_nh_v6=r1_r2_nh_v6,
            stale_nh_v4=r1_r3_nh_v4,
            stale_nh_v6=r1_r3_nh_v6,
            expect_os_if=r1_ge1_os_if,
        )

        print("ISIS diamond SPF metric dual-stack check passed.")
    finally:
        _cleanup_case_config(rt)
