#!/usr/bin/env python3
"""
Route static best-path switch check on dual links.

Goal:
- install two static routes for one prefix via GE-1/GE-2 peer gateways
- switch best path by metric update
- verify OS route gateway/interface always matches the selected link
"""

from __future__ import annotations

import ipaddress
import re
import time

from module_api import cmd, g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TARGET_PREFIX_ADDR = "203.0.113.0"
TARGET_MASK = "24"
TARGET_PREFIX = f"{TARGET_PREFIX_ADDR}/24"


def _wait_path_total(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    expect_total: int,
    timeout: int,
    interval: int = 2,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {TARGET_MASK}",
        timeout=timeout,
        interval=interval,
        contains=[f"Total {expect_total} path(s)"],
        label=f"{device} path-total {destination}={expect_total}",
    )


def _wait_route_paths(
    rt: TopologyRuntime,
    *,
    device: str,
    destination: str,
    expect_paths: dict[str, tuple[str, int]],
    timeout: int,
    interval: int = 2,
) -> None:
    path_regex = [
        rf"(?is)Nexthop\s*:\s*{re.escape(nh)}\b.*?Interface\s*:\s*{re.escape(if_name)}\b.*?Metric\s*:\s*{metric}\b"
        for nh, (if_name, metric) in expect_paths.items()
    ]
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {destination} {TARGET_MASK}",
        timeout=timeout,
        interval=interval,
        regex=path_regex,
        label=f"{device} route-paths {destination}",
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
    prefix: str,
    timeout: int,
    interval: int = 2,
) -> str:
    deadline = time.time() + timeout
    last_out = ""
    while time.time() < deadline:
        out = cmd(rt, device, "show route ipv4 os", strict=False)
        last_out = out
        os_if = _extract_connected_os_if(out, prefix=prefix)
        if os_if is not None:
            return os_if
        time.sleep(interval)

    raise RuntimeError(
        f"{device} connected OS interface detect timeout after {timeout}s\n"
        f"expect: main unicast {prefix} - <if> kernel\n"
        f"command: show route ipv4 os\n"
        f"last output:\n{last_out}"
    )


def _wait_os_best(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    expect_gateway: str,
    expect_interface: str,
    stale_gateways: list[str],
    timeout: int,
    interval: int = 2,
) -> None:
    row_regex = (
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+"
        rf"{re.escape(expect_interface)}\s+static\s+\d+\s*$"
    )
    stale_regex = [
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(gw)}\s+\S+\s+static\s+\d+\s*$"
        for gw in stale_gateways
    ]
    stale_regex.append(
        rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(expect_gateway)}\s+(?!{re.escape(expect_interface)}\b)\S+\s+static\s+\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show route ipv4 os",
        timeout=timeout,
        interval=interval,
        regex=[row_regex],
        not_regex=stale_regex,
        label=f"{device} os-best {prefix} via {expect_gateway} dev {expect_interface}",
    )


def _cleanup(rt: TopologyRuntime, *, device: str, nh_ge1: str, nh_ge2: str) -> None:
    run_cmds(
        rt=rt,
        device=device,
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {nh_ge1}",
            f"no route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {nh_ge2}",
            "end",
        ],
    )


def _link_prefix(ip: str, prefix: int) -> str:
    net_addr = ipaddress.ip_interface(f"{ip}/{prefix}").network.network_address
    return f"{net_addr}/{prefix}"


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    ge1_name = "GE-1"
    ge2_name = "GE-2"
    ge1_nh = str(g_top.r1.GE_1.peer_ip)
    ge2_nh = str(g_top.r1.GE_2.peer_ip)
    ge1_link_prefix = _link_prefix(str(g_top.r1.GE_1.ip), int(g_top.r1.GE_1.prefix))
    ge2_link_prefix = _link_prefix(str(g_top.r1.GE_2.ip), int(g_top.r1.GE_2.prefix))

    try:
        step("Cleanup stale static config")
        _cleanup(rt, device="r1", nh_ge1=ge1_nh, nh_ge2=ge2_nh)

        step("Ensure dual links are up")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"if {ge1_name}",
                "no shutdown",
                "exit",
                f"if {ge2_name}",
                "no shutdown",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {ge1_name}",
                    "contains": [f"Interface {ge1_name} Detail:", "State      : UP"],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "r1",
                    "command": f"show if {ge2_name}",
                    "contains": [f"Interface {ge2_name} Detail:", "State      : UP"],
                    "label": "r1 GE-2 up",
                },
            ],
            timeout=20,
            interval=2,
        )

        step("Detect OS interface names for GE-1/GE-2 connected links")
        ge1_os_if = _wait_connected_os_if(rt, device="r1", prefix=ge1_link_prefix, timeout=20)
        ge2_os_if = _wait_connected_os_if(rt, device="r1", prefix=ge2_link_prefix, timeout=20)
        print(f"Detected OS interfaces: {ge1_name}->{ge1_os_if}, {ge2_name}->{ge2_os_if}")

        step("Add two static paths for same prefix")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {ge1_nh} metric 10",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {ge2_nh} metric 20",
                "end",
            ],
        )

        step("Verify GE-1 path selected initially")
        _wait_path_total(rt, device="r1", destination=TARGET_PREFIX_ADDR, expect_total=2, timeout=30)
        _wait_route_paths(
            rt,
            device="r1",
            destination=TARGET_PREFIX_ADDR,
            expect_paths={
                ge1_nh: (ge1_name, 10),
                ge2_nh: (ge2_name, 20),
            },
            timeout=30,
        )
        _wait_os_best(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=ge1_nh,
            expect_interface=ge1_os_if,
            stale_gateways=[ge2_nh],
            timeout=30,
        )

        step("Raise GE-1 metric and verify GE-2 takeover")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {ge1_nh} metric 30",
                "end",
            ],
        )
        _wait_path_total(rt, device="r1", destination=TARGET_PREFIX_ADDR, expect_total=2, timeout=30)
        _wait_route_paths(
            rt,
            device="r1",
            destination=TARGET_PREFIX_ADDR,
            expect_paths={
                ge1_nh: (ge1_name, 30),
                ge2_nh: (ge2_name, 20),
            },
            timeout=30,
        )
        _wait_os_best(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=ge2_nh,
            expect_interface=ge2_os_if,
            stale_gateways=[ge1_nh],
            timeout=30,
        )

        step("Lower GE-1 metric and verify switch back")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {TARGET_PREFIX_ADDR} {TARGET_MASK} {ge1_nh} metric 5",
                "end",
            ],
        )
        _wait_path_total(rt, device="r1", destination=TARGET_PREFIX_ADDR, expect_total=2, timeout=30)
        _wait_route_paths(
            rt,
            device="r1",
            destination=TARGET_PREFIX_ADDR,
            expect_paths={
                ge1_nh: (ge1_name, 5),
                ge2_nh: (ge2_name, 20),
            },
            timeout=30,
        )
        _wait_os_best(
            rt,
            device="r1",
            prefix=TARGET_PREFIX,
            expect_gateway=ge1_nh,
            expect_interface=ge1_os_if,
            stale_gateways=[ge2_nh],
            timeout=30,
        )

        print("Route dual-link static metric switch check passed.")
    finally:
        step("Cleanup dual-link static config")
        _cleanup(rt, device="r1", nh_ge1=ge1_nh, nh_ge2=ge2_nh)
