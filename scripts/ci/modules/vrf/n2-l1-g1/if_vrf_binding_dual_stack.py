#!/usr/bin/env python3
"""
Interface VRF binding route/FIB/OS-FIB validation (dual-stack).

Coverage on r1 GE-1:
- bind interface to a VRF, and verify binding clears configured IP addresses
- configure IPv4/IPv6 under the bound VRF
- renumber IPv4/IPv6 and verify old routes withdraw/new routes install
- shutdown/no shutdown and verify route/FIB/OS-FIB withdraw/restore, including IPv4 /32 host route
- reboot and verify VRF binding plus route/FIB/OS-FIB restore
- delete IPv4/IPv6 and verify route/FIB/OS-FIB withdraw
- unbind from VRF, reconfigure IP addresses in public VRF, and verify routes move back
"""

from __future__ import annotations

import ipaddress
import re
import time

from module_api import g_top, reboot_device, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "blue"
GE_IF = "GE-1"

BASE_V6 = "2001:db8:12::1"
BASE_V6_LEN = 64

VRF_V4 = "10.12.88.1"
VRF_V4_LEN = 30
VRF_V6 = "2001:db8:1288::1"
VRF_V6_LEN = 64

NEW_V4 = "10.12.99.1"
NEW_V4_LEN = 30
NEW_V6 = "2001:db8:1299::1"
NEW_V6_LEN = 64


def _network_prefix(addr: str, prefix_len: int) -> tuple[str, str]:
    net = ipaddress.ip_network(f"{addr}/{prefix_len}", strict=False)
    return str(net.network_address), f"{net.network_address}/{prefix_len}"


def _route_detail_cmd(afi: str, prefix_addr: str, prefix_len: int, vrf: str) -> str:
    suffix = "" if vrf == "public" else f" vrf {vrf}"
    return f"show route {afi} {prefix_addr} {prefix_len}{suffix}"


def _fib_detail_cmd(afi: str, prefix_addr: str, prefix_len: int, vrf: str) -> str:
    suffix = "" if vrf == "public" else f" vrf {vrf}"
    return f"show fib {afi} {prefix_addr} {prefix_len}{suffix}"


def _fib_os_cmd(afi: str, vrf: str) -> str:
    suffix = "" if vrf == "public" else f" vrf {vrf}"
    return f"show fib {afi} os{suffix}"


def _wait_if_state(
    rt: TopologyRuntime,
    *,
    vrf: str,
    ip4: str | None,
    ip4_len: int | None,
    ip6: str | None,
    ip6_len: int | None,
    proto_state: str | None = None,
    timeout: int = 20,
) -> None:
    contains = [f"Interface {GE_IF} Detail:", f"VRF        : {vrf}"]
    if ip4 is not None:
        contains.append(f"IPv4 Addr  : {ip4}" if ip4_len is None else f"IPv4 Addr  : {ip4}/{ip4_len}")
    if ip6 is not None:
        contains.append(f"IPv6 Addr  : {ip6}" if ip6_len is None else f"IPv6 Addr  : {ip6}/{ip6_len}")

    regex: list[str] = []
    if proto_state is not None:
        regex.append(rf"(?im)^\s*(?:Proto\s+)?State\s*:\s*{re.escape(proto_state)}\s*$")

    wait_check(
        rt,
        device="r1",
        command=f"show if {GE_IF}",
        timeout=timeout,
        interval=2,
        contains=contains,
        regex=regex,
        label=f"r1 {GE_IF} vrf={vrf} state",
    )


def _wait_route_fib_os(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix_addr: str,
    prefix_len: int,
    vrf: str,
    expect_present: bool,
    timeout: int = 30,
) -> None:
    _, prefix = _network_prefix(prefix_addr, prefix_len)
    route_cmd = _route_detail_cmd(afi, prefix_addr, prefix_len, vrf)
    fib_cmd = _fib_detail_cmd(afi, prefix_addr, prefix_len, vrf)
    fib_os_cmd = _fib_os_cmd(afi, vrf)

    route_header = rf"(?im)^\s*Routing entry for {re.escape(prefix)} \(VRF: {re.escape(vrf)}\)\s*$"
    fib_header = rf"(?im)^\s*FIB Route Detail:\s*{re.escape(prefix)}\s*$"
    fib_afi = rf"(?im)^\s*AFI\s*:\s*{re.escape(afi)}\s*$"
    fib_skip_os = r"(?im)^\s*Skip OS\s*:\s*yes\s*$"
    os_row = (
        rf"(?im)^\s*\S+\s+unicast\s+{re.escape(prefix)}\s+-\s+\S+\s+kernel\s+\d+\s+\S+\s*$"
    )

    if expect_present:
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": route_cmd,
                    "regex": [
                        route_header,
                        r"(?im)^\s*Path\s*\[1\]\s*:\s*connected\b",
                        rf"(?im)^\s*Interface\s*:\s*{re.escape(GE_IF)}\s*$",
                    ],
                    "not_contains": ["(no routes)", "(no matching routes)"],
                    "label": f"r1 route {afi} {prefix} vrf={vrf} present",
                },
                {
                    "device": "r1",
                    "command": fib_cmd,
                    "regex": [fib_header, fib_afi, fib_skip_os],
                    "not_contains": ["(no routes)"],
                    "label": f"r1 fib {afi} {prefix} vrf={vrf} present",
                },
                {
                    "device": "r1",
                    "command": fib_os_cmd,
                    "regex": [os_row],
                    "label": f"r1 fib-os {afi} {prefix} vrf={vrf} present",
                },
            ],
            timeout=timeout,
            interval=2,
        )
        return

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": route_cmd,
                "regex": [r"(?im)\((?:no routes|no matching routes)\)"],
                "not_regex": [r"(?im)^\s*Path\s*\[1\]\s*:\s*connected\b"],
                "label": f"r1 route {afi} {prefix} vrf={vrf} absent",
            },
            {
                "device": "r1",
                "command": fib_cmd,
                "not_regex": [fib_header],
                "label": f"r1 fib {afi} {prefix} vrf={vrf} absent",
            },
            {
                "device": "r1",
                "command": fib_os_cmd,
                "not_regex": [os_row],
                "label": f"r1 fib-os {afi} {prefix} vrf={vrf} absent",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_ipv4_host_route_fib_os(
    rt: TopologyRuntime,
    *,
    host_addr: str,
    vrf: str,
    expect_present: bool,
    timeout: int = 30,
) -> None:
    prefix = f"{host_addr}/32"
    route_cmd = _route_detail_cmd("ipv4", host_addr, 32, vrf)
    fib_cmd = _fib_detail_cmd("ipv4", host_addr, 32, vrf)
    fib_os_cmd = _fib_os_cmd("ipv4", vrf)

    route_header = rf"(?im)^\s*Routing entry for {re.escape(prefix)} \(VRF: {re.escape(vrf)}\)\s*$"
    route_path = r"(?im)^\s*Path\s*\[1\]\s*:\s*(?:connected|local)\b"
    fib_header = rf"(?im)^\s*FIB Route Detail:\s*{re.escape(prefix)}\s*$"
    fib_afi = r"(?im)^\s*AFI\s*:\s*ipv4\s*$"
    fib_skip_os = r"(?im)^\s*Skip OS\s*:\s*yes\s*$"
    os_local_row = (
        rf"(?im)^\s*\S+\s+local\s+{re.escape(prefix)}\s+-\s+\S+\s+kernel\s+\d+\s+\S+\s*$"
    )

    if expect_present:
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": route_cmd,
                    "regex": [
                        route_header,
                        route_path,
                        rf"(?im)^\s*Interface\s*:\s*{re.escape(GE_IF)}\s*$",
                    ],
                    "not_contains": ["(no routes)", "(no matching routes)"],
                    "label": f"r1 route ipv4 host {prefix} vrf={vrf} present",
                },
                {
                    "device": "r1",
                    "command": fib_cmd,
                    "regex": [fib_header, fib_afi, fib_skip_os],
                    "not_contains": ["(no routes)"],
                    "label": f"r1 fib ipv4 host {prefix} vrf={vrf} present",
                },
                {
                    "device": "r1",
                    "command": fib_os_cmd,
                    "regex": [os_local_row],
                    "label": f"r1 fib-os ipv4 host {prefix} vrf={vrf} present",
                },
            ],
            timeout=timeout,
            interval=2,
        )
        return

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": route_cmd,
                "regex": [r"(?im)\((?:no routes|no matching routes)\)"],
                "not_regex": [route_path],
                "label": f"r1 route ipv4 host {prefix} vrf={vrf} absent",
            },
            {
                "device": "r1",
                "command": fib_cmd,
                "not_regex": [fib_header],
                "label": f"r1 fib ipv4 host {prefix} vrf={vrf} absent",
            },
            {
                "device": "r1",
                "command": fib_os_cmd,
                "not_regex": [os_local_row],
                "label": f"r1 fib-os ipv4 host {prefix} vrf={vrf} absent",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_dual_stack_routes(
    rt: TopologyRuntime,
    *,
    v4_addr: str,
    v4_len: int,
    v6_addr: str,
    v6_len: int,
    vrf: str,
    expect_present: bool,
    timeout: int = 30,
) -> None:
    v4_net, _ = _network_prefix(v4_addr, v4_len)
    v6_net, _ = _network_prefix(v6_addr, v6_len)
    _wait_route_fib_os(
        rt,
        afi="ipv4",
        prefix_addr=v4_net,
        prefix_len=v4_len,
        vrf=vrf,
        expect_present=expect_present,
        timeout=timeout,
    )
    _wait_route_fib_os(
        rt,
        afi="ipv6",
        prefix_addr=v6_net,
        prefix_len=v6_len,
        vrf=vrf,
        expect_present=expect_present,
        timeout=timeout,
    )


def _cleanup(rt: TopologyRuntime, base_v4: str, base_v4_len: int) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"no ip address {VRF_V4} {VRF_V4_LEN}",
            f"no ip address {NEW_V4} {NEW_V4_LEN}",
            f"no ipv6 address {BASE_V6} {BASE_V6_LEN}",
            f"no ipv6 address {VRF_V6} {VRF_V6_LEN}",
            f"no ipv6 address {NEW_V6} {NEW_V6_LEN}",
            "no vrf forwarding",
            f"ip address {base_v4} {base_v4_len}",
            "exit",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    base_v4 = str(g_top.r1.GE_1.ip)
    base_v4_len = int(g_top.r1.GE_1.prefix)

    try:
        _run_inner(rt, base_v4, base_v4_len)
    finally:
        step("Restore baseline interface and VRF config")
        _cleanup(rt, base_v4, base_v4_len)

    print("Interface VRF binding dual-stack route/FIB/OS-FIB check passed.")


def _run_inner(rt: TopologyRuntime, base_v4: str, base_v4_len: int) -> None:
    base_v4_net, _ = _network_prefix(base_v4, base_v4_len)
    base_v6_net, _ = _network_prefix(BASE_V6, BASE_V6_LEN)
    vrf_v4_net, _ = _network_prefix(VRF_V4, VRF_V4_LEN)
    vrf_v6_net, _ = _network_prefix(VRF_V6, VRF_V6_LEN)
    new_v4_net, _ = _network_prefix(NEW_V4, NEW_V4_LEN)
    new_v6_net, _ = _network_prefix(NEW_V6, NEW_V6_LEN)

    step("Prepare public interface and test VRF")
    _cleanup(rt, base_v4, base_v4_len)
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "exit",
            "end",
        ],
    )
    wait_check(
        rt,
        device="r1",
        command=f"show vrf name {VRF_NAME}",
        timeout=10,
        interval=1,
        contains=[
            "VRF Detail:",
            f"Name           : {VRF_NAME}",
            "OS State       : UP",
        ],
        label=f"r1 vrf {VRF_NAME} ready before IF bind",
    )
    time.sleep(3)
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            f"ip address {base_v4} {base_v4_len}",
            f"ipv6 address {BASE_V6} {BASE_V6_LEN}",
            "exit",
            "end",
        ],
    )
    _wait_if_state(
        rt,
        vrf="public",
        ip4=base_v4,
        ip4_len=base_v4_len,
        ip6=BASE_V6,
        ip6_len=BASE_V6_LEN,
        proto_state="UP",
    )
    _wait_dual_stack_routes(
        rt,
        v4_addr=base_v4_net,
        v4_len=base_v4_len,
        v6_addr=base_v6_net,
        v6_len=BASE_V6_LEN,
        vrf="public",
        expect_present=True,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=base_v4, vrf="public", expect_present=True)

    step("Bind GE-1 to VRF blue and verify IP/routes are cleared")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            f"vrf forwarding {VRF_NAME}",
            "exit",
            "end",
        ],
    )
    _wait_if_state(rt, vrf=VRF_NAME, ip4="-", ip4_len=None, ip6="-", ip6_len=None, proto_state="DOWN")
    _wait_dual_stack_routes(
        rt,
        v4_addr=base_v4_net,
        v4_len=base_v4_len,
        v6_addr=base_v6_net,
        v6_len=BASE_V6_LEN,
        vrf="public",
        expect_present=False,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=base_v4, vrf="public", expect_present=False)
    _wait_dual_stack_routes(
        rt,
        v4_addr=base_v4_net,
        v4_len=base_v4_len,
        v6_addr=base_v6_net,
        v6_len=BASE_V6_LEN,
        vrf=VRF_NAME,
        expect_present=False,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=base_v4, vrf=VRF_NAME, expect_present=False)

    step("Configure IPv4/IPv6 on GE-1 inside VRF blue")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            f"ip address {VRF_V4} {VRF_V4_LEN}",
            f"ipv6 address {VRF_V6} {VRF_V6_LEN}",
            "exit",
            "end",
        ],
    )
    _wait_if_state(
        rt,
        vrf=VRF_NAME,
        ip4=VRF_V4,
        ip4_len=VRF_V4_LEN,
        ip6=VRF_V6,
        ip6_len=VRF_V6_LEN,
        proto_state="UP",
    )
    _wait_dual_stack_routes(
        rt,
        v4_addr=vrf_v4_net,
        v4_len=VRF_V4_LEN,
        v6_addr=vrf_v6_net,
        v6_len=VRF_V6_LEN,
        vrf=VRF_NAME,
        expect_present=True,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=VRF_V4, vrf=VRF_NAME, expect_present=True)
    _wait_dual_stack_routes(
        rt,
        v4_addr=vrf_v4_net,
        v4_len=VRF_V4_LEN,
        v6_addr=vrf_v6_net,
        v6_len=VRF_V6_LEN,
        vrf="public",
        expect_present=False,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=VRF_V4, vrf="public", expect_present=False)

    step("Modify IPv4/IPv6 addresses inside VRF blue")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            f"no ip address {VRF_V4} {VRF_V4_LEN}",
            f"ip address {NEW_V4} {NEW_V4_LEN}",
            f"no ipv6 address {VRF_V6} {VRF_V6_LEN}",
            f"ipv6 address {NEW_V6} {NEW_V6_LEN}",
            "exit",
            "end",
        ],
    )
    _wait_if_state(
        rt,
        vrf=VRF_NAME,
        ip4=NEW_V4,
        ip4_len=NEW_V4_LEN,
        ip6=NEW_V6,
        ip6_len=NEW_V6_LEN,
        proto_state="UP",
    )
    _wait_dual_stack_routes(
        rt,
        v4_addr=vrf_v4_net,
        v4_len=VRF_V4_LEN,
        v6_addr=vrf_v6_net,
        v6_len=VRF_V6_LEN,
        vrf=VRF_NAME,
        expect_present=False,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=VRF_V4, vrf=VRF_NAME, expect_present=False)
    _wait_dual_stack_routes(
        rt,
        v4_addr=new_v4_net,
        v4_len=NEW_V4_LEN,
        v6_addr=new_v6_net,
        v6_len=NEW_V6_LEN,
        vrf=VRF_NAME,
        expect_present=True,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=NEW_V4, vrf=VRF_NAME, expect_present=True)

    step("Shutdown GE-1 in VRF blue and verify routes withdraw")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            "shutdown",
            "exit",
            "end",
        ],
    )
    _wait_if_state(
        rt,
        vrf=VRF_NAME,
        ip4=NEW_V4,
        ip4_len=NEW_V4_LEN,
        ip6=NEW_V6,
        ip6_len=NEW_V6_LEN,
        proto_state="DOWN",
    )
    _wait_dual_stack_routes(
        rt,
        v4_addr=new_v4_net,
        v4_len=NEW_V4_LEN,
        v6_addr=new_v6_net,
        v6_len=NEW_V6_LEN,
        vrf=VRF_NAME,
        expect_present=False,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=NEW_V4, vrf=VRF_NAME, expect_present=False)

    step("No shutdown GE-1 in VRF blue and verify routes restore")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            "no shutdown",
            "exit",
            "end",
        ],
    )
    _wait_if_state(
        rt,
        vrf=VRF_NAME,
        ip4=NEW_V4,
        ip4_len=NEW_V4_LEN,
        ip6=NEW_V6,
        ip6_len=NEW_V6_LEN,
        proto_state="UP",
    )
    _wait_dual_stack_routes(
        rt,
        v4_addr=new_v4_net,
        v4_len=NEW_V4_LEN,
        v6_addr=new_v6_net,
        v6_len=NEW_V6_LEN,
        vrf=VRF_NAME,
        expect_present=True,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=NEW_V4, vrf=VRF_NAME, expect_present=True)

    step("Reboot r1 and verify VRF binding and routes restore")
    reboot_device(rt, "r1", timeout=120)
    wait_check(
        rt,
        device="r1",
        command=f"show vrf name {VRF_NAME}",
        timeout=20,
        interval=2,
        contains=[
            "VRF Detail:",
            f"Name           : {VRF_NAME}",
            "OS State       : UP",
        ],
        label=f"r1 vrf {VRF_NAME} restored after reboot",
    )
    _wait_if_state(
        rt,
        vrf=VRF_NAME,
        ip4=NEW_V4,
        ip4_len=NEW_V4_LEN,
        ip6=NEW_V6,
        ip6_len=NEW_V6_LEN,
        proto_state="UP",
        timeout=30,
    )
    _wait_dual_stack_routes(
        rt,
        v4_addr=new_v4_net,
        v4_len=NEW_V4_LEN,
        v6_addr=new_v6_net,
        v6_len=NEW_V6_LEN,
        vrf=VRF_NAME,
        expect_present=True,
        timeout=40,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=NEW_V4, vrf=VRF_NAME, expect_present=True, timeout=40)

    step("Delete IPv4/IPv6 from VRF blue and verify routes withdraw")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            f"no ip address {NEW_V4} {NEW_V4_LEN}",
            f"no ipv6 address {NEW_V6} {NEW_V6_LEN}",
            "exit",
            "end",
        ],
    )
    _wait_if_state(rt, vrf=VRF_NAME, ip4="-", ip4_len=None, ip6="-", ip6_len=None, proto_state="DOWN")
    _wait_dual_stack_routes(
        rt,
        v4_addr=new_v4_net,
        v4_len=NEW_V4_LEN,
        v6_addr=new_v6_net,
        v6_len=NEW_V6_LEN,
        vrf=VRF_NAME,
        expect_present=False,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=NEW_V4, vrf=VRF_NAME, expect_present=False)

    step("Unbind GE-1 from VRF blue and verify public routes after reconfigure")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if {GE_IF}",
            "no vrf forwarding",
            f"ip address {base_v4} {base_v4_len}",
            f"ipv6 address {BASE_V6} {BASE_V6_LEN}",
            "exit",
            "end",
        ],
    )
    _wait_if_state(
        rt,
        vrf="public",
        ip4=base_v4,
        ip4_len=base_v4_len,
        ip6=BASE_V6,
        ip6_len=BASE_V6_LEN,
        proto_state="UP",
    )
    _wait_dual_stack_routes(
        rt,
        v4_addr=base_v4_net,
        v4_len=base_v4_len,
        v6_addr=base_v6_net,
        v6_len=BASE_V6_LEN,
        vrf="public",
        expect_present=True,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=base_v4, vrf="public", expect_present=True)
    _wait_dual_stack_routes(
        rt,
        v4_addr=base_v4_net,
        v4_len=base_v4_len,
        v6_addr=base_v6_net,
        v6_len=BASE_V6_LEN,
        vrf=VRF_NAME,
        expect_present=False,
    )
    _wait_ipv4_host_route_fib_os(rt, host_addr=base_v4, vrf=VRF_NAME, expect_present=False)
