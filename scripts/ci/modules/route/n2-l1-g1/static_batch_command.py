#!/usr/bin/env python3
"""
Route static-batch command syntax and overlap checks.

Goals:
- `route static-batch` follows single static route syntax:
  `route static-batch <name> ipv4|ipv6 [vrf <name>] <start> <len> ...`
- nexthop, nexthop+interface, interface-only, metric, and VRF forms are accepted
- different static-batch names cannot generate overlapping prefixes in the same VRF/AFI
- route detail, FIB detail, and FIB OS views match resolved/unresolved state
- `show current-configuration` emits the new command prefix and argument order
"""

from __future__ import annotations

import ipaddress
import re

from module_api import check_output, cmd, require_devices, run_cmds, step, wait_check, wait_checks
from top_runner import TopologyRuntime


VRF_NAME = "sbblue"
BATCH_A = "sb_a"
BATCH_B = "sb_b"
BATCH_VRF = "sb_vrf"
BATCH_V6 = "sb_v6"
V4_START = "198.18.210.0"
V4_OVERLAP = "198.18.211.0"
V4_REPLACED = "198.18.212.0"
V4_VRF_START = "198.18.220.0"
V4_LEN = 24
V4_NH = "203.0.113.1"
V4_CONFLICT_NH = "203.0.113.2"
V6_START = "2001:db8:220::"
V6_LEN = 64


def _prefixes(start: str, prefix_len: int, count: int) -> list[str]:
    first = ipaddress.ip_network(f"{start}/{prefix_len}", strict=False)
    addr_cls = first.network_address.__class__
    step = first.num_addresses
    prefixes: list[str] = []
    for idx in range(count):
        addr = addr_cls(int(first.network_address) + idx * step)
        network = ipaddress.ip_network(f"{addr}/{prefix_len}", strict=False)
        prefixes.append(f"{network.network_address}/{prefix_len}")
    return prefixes


def _split_prefix(prefix: str) -> tuple[str, int]:
    addr, plen = prefix.rsplit("/", 1)
    return addr, int(plen)


def _route_detail_cmd(afi: str, prefix: str, vrf: str) -> str:
    addr, plen = _split_prefix(prefix)
    if vrf == "public":
        return f"show route {afi} {addr} {plen}"
    return f"show route {afi} vrf {vrf} {addr} {plen}"


def _fib_detail_cmd(afi: str, prefix: str, vrf: str) -> str:
    addr, plen = _split_prefix(prefix)
    if vrf == "public":
        return f"show fib {afi} {addr} {plen}"
    return f"show fib {afi} vrf {vrf} {addr} {plen}"


def _fib_os_cmd(afi: str, vrf: str) -> str:
    if vrf == "public":
        return f"show fib os {afi}"
    return f"show fib os {afi} vrf {vrf} "


def _wait_route_fib_os(
    rt: TopologyRuntime,
    *,
    afi: str,
    prefix: str,
    vrf: str = "public",
    expect_present: bool,
    os_route_type: str = "unicast",
    timeout: int = 30,
) -> None:
    route_header = rf"(?im)^\s*Routing entry for\s+{re.escape(prefix)}\s+\(VRF:\s*{re.escape(vrf)}\)\s*$"
    route_static_path = r"(?im)^\s*Path\s*\[\d+\]\s*:\s*static\b"
    fib_header = rf"(?im)^\s*Routing entry for\s+{re.escape(prefix)}\b"
    fib_installed = r"(?im)^\s*Installed\s*:\s*yes\s*$"
    fib_skip_os = r"(?im)^\s*Skip OS\s*:\s*no\s*$"
    fib_blackhole = r"(?im)^\s*NH-Type\s*:\s*blackhole\s*$"
    os_row = (
        rf"(?im)^\s*\S+\s+{re.escape(os_route_type)}\s+{re.escape(prefix)}\s+"
        rf"\S+\s+\S+\s+static\s+\d+(?:\s+\S+)?\s*$"
    )
    os_any_row = rf"(?im)^\s*\S+\s+\S+\s+{re.escape(prefix)}\s+"

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": _route_detail_cmd(afi, prefix, vrf),
                "regex": [route_header, route_static_path] if expect_present else (),
                "contains": [] if expect_present else ["(no matching routes)"],
                "label": f"r1 route {afi} {prefix} vrf={vrf} {'present' if expect_present else 'absent'}",
            },
            {
                "device": "r1",
                "command": _fib_detail_cmd(afi, prefix, vrf),
                "regex": [fib_header, fib_installed, fib_skip_os, fib_blackhole] if expect_present else (),
                "not_regex": [] if expect_present else [fib_header],
                "label": f"r1 fib {afi} {prefix} vrf={vrf} {'present' if expect_present else 'absent'}",
            },
            {
                "device": "r1",
                "command": _fib_os_cmd(afi, vrf),
                "regex": [os_row] if expect_present else (),
                "not_regex": [] if expect_present else [os_any_row],
                "label": f"r1 fib-os {afi} {prefix} vrf={vrf} {'present' if expect_present else 'absent'}",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _assert(label: str, output: str, *, contains=(), not_contains=(), regex=(), not_regex=()) -> None:
    violations = check_output(
        output,
        contains=contains,
        not_contains=not_contains,
        regex=regex,
        not_regex=not_regex,
    )
    if violations:
        raise AssertionError(f"{label}: " + "; ".join(violations))


def _cleanup(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route static-batch {BATCH_A}",
            f"no route static-batch {BATCH_B}",
            f"no route static-batch {BATCH_VRF}",
            f"no route static-batch {BATCH_V6}",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))

    try:
        step("Cleanup stale static-batch configuration")
        _cleanup(rt)

        step("Add IPv4 static-batch with nexthop and metric")
        out = run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static-batch {BATCH_A} ipv4 {V4_START} {V4_LEN} {V4_NH} metric 5 count 2",
                "end",
            ],
        )[1]
        _assert("add public static-batch", out, contains=[f"Added 2 static-batch IPv4 route(s) for '{BATCH_A}'"])
        wait_check(
            rt,
            device="r1",
            command="show route static ipv4",
            timeout=20,
            interval=2,
            contains=["Total 2 static route(s)"],
            regex=[
                rf"(?im)^\s*ipv4\s+{re.escape(V4_START)}/24\s+{re.escape(V4_NH)}\s+-\s+5\s+1\s+no\s+no\s*$",
                rf"(?im)^\s*ipv4\s+{re.escape(V4_OVERLAP)}/24\s+{re.escape(V4_NH)}\s+-\s+5\s+1\s+no\s+no\s*$",
            ],
            label="public static-batch candidate rows",
        )

        step("Verify unresolved public static-batch stays out of route/FIB/FIB-OS")
        for prefix in _prefixes(V4_START, V4_LEN, 2):
            _wait_route_fib_os(rt, afi="ipv4", prefix=prefix, expect_present=False)

        step("Reject overlapping static-batch with a different name")
        outs = run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                f"route static-batch {BATCH_B} ipv4 {V4_OVERLAP} {V4_LEN} {V4_CONFLICT_NH} count 1",
                "end",
            ],
        )
        _assert(
            "overlap rejected",
            outs[1],
            contains=[f"Error: Static-batch route overlaps with batch '{BATCH_A}'"],
        )

        step("Allow same-name replacement")
        out = run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static-batch {BATCH_A} ipv4 {V4_REPLACED} {V4_LEN} {V4_NH} count 1",
                "end",
            ],
        )[1]
        _assert("same-name replacement", out, contains=[f"Added 1 static-batch IPv4 route(s) for '{BATCH_A}'"])
        wait_check(
            rt,
            device="r1",
            command="show route static ipv4",
            timeout=20,
            interval=2,
            regex=[rf"(?im)^\s*ipv4\s+{re.escape(V4_REPLACED)}/24\s+{re.escape(V4_NH)}\s+-\s+0\s+1\s+no\s+no\s*$"],
            not_regex=[rf"(?im)^\s*ipv4\s+{re.escape(V4_START)}/24\b"],
            label="same-name replacement candidate rows",
        )

        step("Verify same-name replacement is still unresolved outside route/FIB/FIB-OS")
        for prefix in _prefixes(V4_START, V4_LEN, 2) + _prefixes(V4_REPLACED, V4_LEN, 1):
            _wait_route_fib_os(rt, afi="ipv4", prefix=prefix, expect_present=False)

        step("Add VRF static-batch with interface-only and metric")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"vrf {VRF_NAME}",
                "af ipv4-unicast",
                "route-distinguisher 65001:220",
                "exit",
                "exit",
                f"route static-batch {BATCH_VRF} ipv4 vrf {VRF_NAME} {V4_VRF_START} {V4_LEN} interface null0 metric 7 count 2",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show route static ipv4 vrf {VRF_NAME}",
            timeout=20,
            interval=2,
            contains=["Total 2 static route(s)"],
            regex=[
                rf"(?im)^\s*ipv4\s+{re.escape(V4_VRF_START)}/24\s+-\s+null0\s+7\s+1\s+yes\s+yes\s*$",
            ],
            label="vrf interface-only static-batch rows",
        )
        step("Verify VRF static-batch installs into route/FIB/FIB-OS")
        for prefix in _prefixes(V4_VRF_START, V4_LEN, 2):
            _wait_route_fib_os(
                rt,
                afi="ipv4",
                prefix=prefix,
                vrf=VRF_NAME,
                expect_present=True,
                os_route_type="blackhole",
            )

        step("Add IPv6 static-batch with interface-only and metric")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route static-batch {BATCH_V6} ipv6 {V6_START} {V6_LEN} interface null0 metric 9 count 2",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command="show route static ipv6",
            timeout=20,
            interval=2,
            contains=["Total 2 static route(s)"],
            regex=[
                rf"(?im)^\s*ipv6\s+{re.escape(V6_START)}/64\s+-\s+null0\s+9\s+1\s+yes\s+yes\s*$",
            ],
            label="ipv6 interface-only static-batch rows",
        )
        step("Verify IPv6 static-batch installs into route/FIB/FIB-OS")
        for prefix in _prefixes(V6_START, V6_LEN, 2):
            _wait_route_fib_os(
                rt,
                afi="ipv6",
                prefix=prefix,
                expect_present=True,
                os_route_type="blackhole",
            )

        step("Verify current configuration uses route static-batch order")
        out = cmd(rt, "r1", "show current-configuration", strict=False)
        _assert(
            "current configuration",
            out,
            contains=[
                f"route static-batch {BATCH_A} ipv4 {V4_REPLACED} {V4_LEN} {V4_NH} count 1",
                f"route static-batch {BATCH_VRF} ipv4 vrf {VRF_NAME} {V4_VRF_START} {V4_LEN} interface null0 metric 7 count 2",
                f"route static-batch {BATCH_V6} ipv6 {V6_START} {V6_LEN} interface null0 metric 9 count 2",
            ],
            not_contains=[f"route batch {BATCH_A}", f"route static-batch {BATCH_B}"],
        )

        print("Route static-batch command syntax and overlap check passed.")
    finally:
        _cleanup(rt)
