#!/usr/bin/env python3
"""
OSPFv2 two-router point-to-point integration check.

Coverage:
- OSPFv2 instance, Area, and interface configuration
- Non-backbone Area adjacency and referenced-Area deletion guard
- Full adjacency and Router-LSA exchange
- Passive loopback /32 learning through OSPF
- Route RIB, FIB, OS FIB, and sourced forwarding
- Interface flap withdrawal and recovery
- OSPF process reboot, persistent configuration restore, and forwarding recovery
- Running-configuration rendering
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    check_output,
    g_top,
    hold_check,
    process_reboot,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
    wait_fib_route,
)
from top_runner import TopologyRuntime  # noqa: E402


PROCESS_ID = 100
SENTINEL_PROCESS_ID = 101
AREA = 1
UNUSED_AREA = 2
SENTINEL_AREA = 4294967295
GE_IF = "GE-1"
HELLO_INTERVAL = 2
DEAD_INTERVAL = 8

R1_ROUTER_ID = "10.255.1.1"
R2_ROUTER_ID = "10.255.2.2"
R1_LOOP_ID = 11
R2_LOOP_ID = 22
R1_LOOP_PREFIX = f"{R1_ROUTER_ID}/32"
R2_LOOP_PREFIX = f"{R2_ROUTER_ID}/32"

PING_SUCCESS_RE = r"(?im)\b0(?:\.0)?%\s+packet loss\b"
PING_FAILURE_RE = (
    r"(?im)(?:\b100(?:\.0)?%\s+packet loss\b|network is unreachable|"
    r"destination host unreachable|no route to host|connect:|"
    r"module timed out or failed to respond|failed to start ping)"
)
COMMAND_FAILURE_RE = (
    r"(?im)(?:unknown command|invalid input|command not found|"
    r"module timed out or failed to respond|failed to start module|error:\s)"
)


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup OSPF and loopback config")
    for device, loop_id in (("r1", R1_LOOP_ID), ("r2", R2_LOOP_ID)):
        run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=[
                "end",
                "config",
                f"no ospf {PROCESS_ID}",
                f"no ospf {SENTINEL_PROCESS_ID}",
                f"no if loop {loop_id}",
                f"if {GE_IF}",
                "no shutdown",
                "exit",
                "end",
            ],
        )


def _verify_cleanup(rt: TopologyRuntime) -> None:
    step("Verify OSPF and loopback config cleanup")
    checks: list[dict[str, object]] = []
    for device, loop_id, peer_router_id, peer_prefix in (
        ("r1", R1_LOOP_ID, R2_ROUTER_ID, R2_LOOP_PREFIX),
        ("r2", R2_LOOP_ID, R1_ROUTER_ID, R1_LOOP_PREFIX),
    ):
        checks.append(
            {
                "device": device,
                "command": "show current-configuration",
                "not_regex": [
                    rf"(?im)^\s*ospf\s+{PROCESS_ID}\s*$",
                    rf"(?im)^\s*ospf\s+\S+\s+{PROCESS_ID}(?:\s|$)",
                    rf"(?im)^\s*ospf\s+{SENTINEL_PROCESS_ID}\s*$",
                    rf"(?im)^\s*ospf\s+\S+\s+{SENTINEL_PROCESS_ID}(?:\s|$)",
                    rf"(?im)^\s*if\s+loop\s+{loop_id}\s*$",
                    COMMAND_FAILURE_RE,
                ],
                "label": f"{device} OSPF process and loopback config removed",
            }
        )
        checks.extend(
            [
                {
                    "device": device,
                    "command": f"show if {GE_IF}",
                    "contains": ["Proto State: UP", "Link State : UP"],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": f"{device} {GE_IF} restored up",
                },
                {
                    "device": device,
                    "command": f"show route ipv4 {peer_router_id} 32",
                    "contains": ["(no matching routes)"],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": f"{device} Route RIB cleanup for {peer_prefix}",
                },
                {
                    "device": device,
                    "command": "show fib os ipv4",
                    "not_contains": [peer_prefix],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": f"{device} OS FIB cleanup for {peer_prefix}",
                },
            ]
        )
    wait_checks(rt, checks, timeout=30, interval=2)
    wait_fib_route(
        rt,
        device="r1",
        afi="ipv4",
        prefix_addr=R2_ROUTER_ID,
        prefix_len=32,
        expect_present=False,
        timeout=30,
        interval=2,
        label=f"r1 FIB cleanup for {R2_LOOP_PREFIX}",
    )
    wait_fib_route(
        rt,
        device="r2",
        afi="ipv4",
        prefix_addr=R1_ROUTER_ID,
        prefix_len=32,
        expect_present=False,
        timeout=30,
        interval=2,
        label=f"r2 FIB cleanup for {R1_LOOP_PREFIX}",
    )


def _final_cleanup(rt: TopologyRuntime, test_error: BaseException | None) -> None:
    try:
        _cleanup(rt)
        _verify_cleanup(rt)
    except BaseException as cleanup_error:
        if test_error is None:
            raise
        message = f"final OSPF cleanup verification also failed: {cleanup_error}"
        add_note = getattr(test_error, "add_note", None)
        if callable(add_note):
            add_note(message)
        print(f"ERROR: {message}")


def _configure_router(
    rt: TopologyRuntime,
    *,
    device: str,
    router_id: str,
    loop_id: int,
    explicit_primary_area: bool,
) -> None:
    process_commands = [
        f"ospf {PROCESS_ID}",
        f"router-id {router_id}",
        f"area {UNUSED_AREA}",
    ]
    if explicit_primary_area:
        process_commands.append(f"area {AREA}")
    process_commands.append("exit")

    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"if loop {loop_id}",
            f"ip address {router_id} 32",
            "exit",
            *process_commands,
            f"if {GE_IF}",
            f"ospf enable {PROCESS_ID} area {AREA}",
            f"ospf network-type {PROCESS_ID} point-to-point",
            f"ospf hello-interval {PROCESS_ID} {HELLO_INTERVAL}",
            f"ospf dead-interval {PROCESS_ID} {DEAD_INTERVAL}",
            "exit",
            f"if loop {loop_id}",
            f"ospf enable {PROCESS_ID} area {AREA}",
            f"ospf passive {PROCESS_ID}",
            "exit",
            "end",
        ],
    )


def _wait_full_adjacencies(rt: TopologyRuntime, *, timeout: int = 90) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show ospf neighbor {PROCESS_ID}",
                "contains": [GE_IF, R2_ROUTER_ID],
                "not_contains": ["(no OSPF neighbor)", "(instance not found)"],
                "regex": [r"(?im)\bFull\b"],
                "label": "r1 sees r2 OSPF neighbor Full",
            },
            {
                "device": "r2",
                "command": f"show ospf neighbor {PROCESS_ID}",
                "contains": [GE_IF, R1_ROUTER_ID],
                "not_contains": ["(no OSPF neighbor)", "(instance not found)"],
                "regex": [r"(?im)\bFull\b"],
                "label": "r2 sees r1 OSPF neighbor Full",
            },
        ],
        timeout=timeout,
        interval=2,
    )


def _wait_neighbor_absent(
    rt: TopologyRuntime,
    *,
    device: str,
    peer_router_id: str,
    timeout: int = 30,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"show ospf neighbor {PROCESS_ID}",
        timeout=timeout,
        interval=2,
        not_contains=[peer_router_id],
        not_regex=[COMMAND_FAILURE_RE],
        label=f"{device} OSPF neighbor {peer_router_id} removed",
    )


def _wait_route_present(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    nexthop: str,
    advertising_router: str,
    timeout: int = 90,
) -> None:
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_check(
        rt,
        device=device,
        command=f"show ospf route {PROCESS_ID}",
        timeout=timeout,
        interval=2,
        regex=[
            rf"(?im)^\s*{PROCESS_ID}\s+{re.escape(prefix)}\s+\d+\s+"
            rf"{re.escape(nexthop)}\s+\S+\s+{re.escape(advertising_router)}\s*$"
        ],
        not_contains=["(no OSPF route)", "(instance not found)"],
        not_regex=[COMMAND_FAILURE_RE],
        label=f"{device} OSPF protocol route {prefix}",
    )
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {prefix_addr} {prefix_len}",
        timeout=timeout,
        interval=2,
        contains=[f"Routing entry for {prefix}"],
        regex=[
            r"(?im)^\s*Path\s*\[\d+\]\s*:\s*ospf\s*$",
            rf"(?im)^\s*Nexthop\s*:\s*{re.escape(nexthop)}\s*$",
            r"(?im)^\s*Preference\s*:\s*110\s*$",
        ],
        label=f"{device} Route RIB has OSPF route {prefix}",
    )
    wait_fib_route(
        rt,
        device=device,
        afi="ipv4",
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=True,
        nexthop=nexthop,
        installed=True,
        skip_os=False,
        timeout=timeout,
        interval=2,
        label=f"{device} FIB has installed OSPF route {prefix}",
    )
    wait_check(
        rt,
        device=device,
        command="show fib os ipv4",
        timeout=timeout,
        interval=2,
        regex=[
            rf"(?im)^\s*main\s+unicast\s+{re.escape(prefix)}\s+{re.escape(nexthop)}\s+\S+\s+ospf\s+\d+\b"
        ],
        label=f"{device} OS FIB has OSPF route {prefix}",
    )


def _wait_route_absent(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    timeout: int = 45,
) -> None:
    prefix_addr, prefix_len = prefix.rsplit("/", 1)
    wait_check(
        rt,
        device=device,
        command=f"show ospf route {PROCESS_ID}",
        timeout=timeout,
        interval=2,
        not_contains=[prefix],
        not_regex=[COMMAND_FAILURE_RE],
        label=f"{device} OSPF protocol route {prefix} withdrawn",
    )
    wait_check(
        rt,
        device=device,
        command=f"show route ipv4 {prefix_addr} {prefix_len}",
        timeout=timeout,
        interval=2,
        contains=["(no matching routes)"],
        not_regex=[
            r"(?im)^\s*Path\s*\[\d+\]\s*:\s*ospf\s*$",
            COMMAND_FAILURE_RE,
        ],
        label=f"{device} Route RIB withdrew OSPF route {prefix}",
    )
    wait_fib_route(
        rt,
        device=device,
        afi="ipv4",
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=False,
        timeout=timeout,
        interval=2,
        label=f"{device} FIB withdrew OSPF route {prefix}",
    )
    wait_check(
        rt,
        device=device,
        command="show fib os ipv4",
        timeout=timeout,
        interval=2,
        not_contains=[prefix],
        not_regex=[COMMAND_FAILURE_RE],
        label=f"{device} OS FIB withdrew OSPF route {prefix}",
    )


def _wait_sourced_ping(
    rt: TopologyRuntime,
    *,
    device: str,
    source: str,
    destination: str,
    timeout: int = 45,
) -> None:
    wait_check(
        rt,
        device=device,
        command=f"ping {destination} -a {source}",
        timeout=timeout,
        interval=2,
        regex=[PING_SUCCESS_RE],
        not_regex=[PING_FAILURE_RE],
        normalize_whitespace=False,
        label=f"{device} sourced ping {source} -> {destination}",
    )


def _assert_output(
    label: str,
    output: str,
    *,
    contains: list[str] | None = None,
    not_regex: list[str] | None = None,
) -> None:
    violations = check_output(
        output,
        contains=contains or [],
        not_regex=not_regex or [],
    )
    if violations:
        raise AssertionError(f"{label}: {'; '.join(violations)}\noutput:\n{output}")


def _interface_show_this(rt: TopologyRuntime, interface_command: str) -> str:
    outputs = run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["end", "config", interface_command, "show this", "exit", "end"],
    )
    return outputs[3]


def _verify_process_delete_cascade(rt: TopologyRuntime) -> None:
    step("Keep a sentinel process and delete OSPF 100 with all interface configuration")
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"ospf {SENTINEL_PROCESS_ID}",
            f"area {SENTINEL_AREA}",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r1",
        commands=["config", f"no ospf {PROCESS_ID}", "end"],
    )

    ospf_cmd_re = rf"(?im)^\s*ospf\s+\S+\s+{PROCESS_ID}(?:\s|$)"
    ge_show = _interface_show_this(rt, f"if {GE_IF}")
    loop_show = _interface_show_this(rt, f"if loop {R1_LOOP_ID}")
    _assert_output(
        "GE-1 show this after process delete",
        ge_show,
        not_regex=[ospf_cmd_re, COMMAND_FAILURE_RE],
    )
    _assert_output(
        "loop11 show this after process delete",
        loop_show,
        not_regex=[ospf_cmd_re, COMMAND_FAILURE_RE],
    )

    step("Recreate and reboot an empty OSPF 100 process to detect persistent orphan rows")
    run_cmds(
        rt=rt,
        device="r1",
        commands=["config", f"ospf {PROCESS_ID}", "exit", "end"],
    )
    reboot_out = process_reboot(
        rt,
        "r1",
        "ospf",
        cmd_timeout=90,
        ready_timeout=90,
    )
    if "reboot ospf ok" not in reboot_out:
        raise AssertionError(f"unexpected empty-process `process reboot ospf` response:\n{reboot_out}")

    wait_check(
        rt,
        device="r1",
        command=f"show ospf interface {PROCESS_ID}",
        timeout=60,
        interval=2,
        contains=["(no OSPF interface)"],
        not_regex=[COMMAND_FAILURE_RE],
        label="recreated OSPF process has no restored interface configuration",
    )
    ge_show = _interface_show_this(rt, f"if {GE_IF}")
    loop_show = _interface_show_this(rt, f"if loop {R1_LOOP_ID}")
    _assert_output(
        "GE-1 show this after empty-process reboot",
        ge_show,
        not_regex=[ospf_cmd_re, COMMAND_FAILURE_RE],
    )
    _assert_output(
        "loop11 show this after empty-process reboot",
        loop_show,
        not_regex=[ospf_cmd_re, COMMAND_FAILURE_RE],
    )

    outputs = run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=["end", "config", f"ospf {PROCESS_ID}", "show this", "exit", "end"],
    )
    _assert_output(
        "empty OSPF process show this",
        outputs[3],
        contains=[f"ospf {PROCESS_ID}"],
        not_regex=[
            r"(?im)^\s+area\s+\d+\s*$",
            r"(?im)^\s+router-id\s+",
            COMMAND_FAILURE_RE,
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_link_ip = str(g_top.r1.GE_1.ip)
    r2_link_ip = str(g_top.r2.GE_1.ip)
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    test_error: BaseException | None = None
    try:
        _cleanup(rt)

        step("Verify GE-1 baseline connectivity")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r1_link_ip}/30",
                    ],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "r2",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {r2_link_ip}/30",
                    ],
                    "label": "r2 GE-1 up",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure OSPF point-to-point interfaces and passive loopbacks")
        _configure_router(
            rt,
            device="r1",
            router_id=R1_ROUTER_ID,
            loop_id=R1_LOOP_ID,
            explicit_primary_area=True,
        )
        _configure_router(
            rt,
            device="r2",
            router_id=R2_ROUTER_ID,
            loop_id=R2_LOOP_ID,
            explicit_primary_area=False,
        )

        step("Wait for bidirectional OSPF Full adjacency")
        _wait_full_adjacencies(rt)
        hold_check(
            rt,
            device="r1",
            command=f"show ospf neighbor {PROCESS_ID}",
            duration=6,
            interval=2,
            contains=[GE_IF, R2_ROUTER_ID],
            regex=[r"(?im)\bFull\b"],
            label="r1 OSPF adjacency remains Full",
        )

        step("Verify non-backbone Area runtime and implicit Area creation")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show ospf interface {PROCESS_ID}",
                    "regex": [
                        rf"(?im)^\s*{PROCESS_ID}\s+{re.escape(GE_IF)}\s+0\.0\.0\.1\s+",
                    ],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r1 GE-1 is active in Area 1",
                },
                {
                    "device": "r2",
                    "command": f"show ospf interface {PROCESS_ID}",
                    "regex": [
                        rf"(?im)^\s*{PROCESS_ID}\s+{re.escape(GE_IF)}\s+0\.0\.0\.1\s+",
                    ],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r2 GE-1 is active in implicitly created Area 1",
                },
                {
                    "device": "r1",
                    "command": f"show ospf summary {PROCESS_ID}",
                    "contains": ["Areas"],
                    "regex": [
                        rf"(?im)^\s*{PROCESS_ID}\s+public\s+{re.escape(R1_ROUTER_ID)}\s+Up\s+2\s+2\s+",
                    ],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r1 summary counts both configured Areas",
                },
                {
                    "device": "r2",
                    "command": f"show ospf summary {PROCESS_ID}",
                    "contains": ["Areas"],
                    "regex": [
                        rf"(?im)^\s*{PROCESS_ID}\s+public\s+{re.escape(R2_ROUTER_ID)}\s+Up\s+2\s+2\s+",
                    ],
                    "not_regex": [COMMAND_FAILURE_RE],
                    "label": "r2 summary counts explicit and implicit Areas",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Reject deletion of Area 1 while interfaces still reference it")
        outputs = run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", f"ospf {PROCESS_ID}", f"no area {AREA}", "end"],
        )
        if "is in use by interface" not in outputs[2]:
            raise AssertionError(f"referenced Area deletion was not rejected:\n{outputs[2]}")
        _wait_full_adjacencies(rt, timeout=30)

        step("Verify Router-LSAs are exchanged")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show ospf lsdb {PROCESS_ID}",
                    "contains": [R1_ROUTER_ID, R2_ROUTER_ID],
                    "not_contains": ["(no OSPF LSA)", "(instance not found)"],
                    "label": "r1 LSDB has both Router-LSAs",
                },
                {
                    "device": "r2",
                    "command": f"show ospf lsdb {PROCESS_ID}",
                    "contains": [R1_ROUTER_ID, R2_ROUTER_ID],
                    "not_contains": ["(no OSPF LSA)", "(instance not found)"],
                    "label": "r2 LSDB has both Router-LSAs",
                },
            ],
            timeout=60,
            interval=2,
        )

        step("Verify learned loopbacks in OSPF RIB, Route RIB, FIB, and OS")
        _wait_route_present(
            rt, device="r1", prefix=R2_LOOP_PREFIX, nexthop=r1_peer_ip, advertising_router=R2_ROUTER_ID
        )
        _wait_route_present(
            rt, device="r2", prefix=R1_LOOP_PREFIX, nexthop=r2_peer_ip, advertising_router=R1_ROUTER_ID
        )

        step("Verify bidirectional sourced forwarding")
        _wait_sourced_ping(rt, device="r1", source=R1_ROUTER_ID, destination=R2_ROUTER_ID)
        _wait_sourced_ping(rt, device="r2", source=R2_ROUTER_ID, destination=R1_ROUTER_ID)

        step("Verify OSPF running-configuration rendering")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "contains": [
                        f"ospf {PROCESS_ID}",
                        f"router-id {R1_ROUTER_ID}",
                        f"ospf enable {PROCESS_ID} area {AREA}",
                        f"ospf network-type {PROCESS_ID} point-to-point",
                        f"ospf hello-interval {PROCESS_ID} {HELLO_INTERVAL}",
                        f"ospf dead-interval {PROCESS_ID} {DEAD_INTERVAL}",
                        f"ospf passive {PROCESS_ID}",
                    ],
                    "regex": [
                        rf"(?m)^\s+area\s+{AREA}\r?$",
                        rf"(?m)^\s+area\s+{UNUSED_AREA}\r?$",
                    ],
                    "label": "r1 OSPF running config",
                },
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "contains": [
                        f"ospf {PROCESS_ID}",
                        f"router-id {R2_ROUTER_ID}",
                        f"ospf enable {PROCESS_ID} area {AREA}",
                        f"ospf network-type {PROCESS_ID} point-to-point",
                        f"ospf hello-interval {PROCESS_ID} {HELLO_INTERVAL}",
                        f"ospf dead-interval {PROCESS_ID} {DEAD_INTERVAL}",
                        f"ospf passive {PROCESS_ID}",
                    ],
                    "regex": [
                        rf"(?m)^\s+area\s+{AREA}\r?$",
                        rf"(?m)^\s+area\s+{UNUSED_AREA}\r?$",
                    ],
                    "label": "r2 OSPF running config",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Reboot OSPF on r1 and verify persisted configuration is restored")
        reboot_out = process_reboot(
            rt,
            "r1",
            "ospf",
            cmd_timeout=90,
            ready_timeout=90,
        )
        if "reboot ospf ok" not in reboot_out:
            raise AssertionError(f"unexpected `process reboot ospf` response:\n{reboot_out}")

        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            timeout=60,
            interval=2,
            contains=[
                f"ospf {PROCESS_ID}",
                f"router-id {R1_ROUTER_ID}",
                f"ospf enable {PROCESS_ID} area {AREA}",
                f"ospf network-type {PROCESS_ID} point-to-point",
                f"ospf hello-interval {PROCESS_ID} {HELLO_INTERVAL}",
                f"ospf dead-interval {PROCESS_ID} {DEAD_INTERVAL}",
                f"ospf passive {PROCESS_ID}",
            ],
            regex=[
                rf"(?m)^\s+area\s+{AREA}\r?$",
                rf"(?m)^\s+area\s+{UNUSED_AREA}\r?$",
            ],
            not_regex=[COMMAND_FAILURE_RE],
            label="r1 OSPF config survives process reboot",
        )

        step("Delete and recreate an unused Area without disturbing adjacency")
        run_cmds(
            rt=rt,
            device="r1",
            commands=["config", f"ospf {PROCESS_ID}", f"no area {UNUSED_AREA}", "end"],
        )
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            timeout=30,
            interval=2,
            not_regex=[rf"(?m)^\s+area\s+{UNUSED_AREA}\r?$", COMMAND_FAILURE_RE],
            label="unused Area 2 is removed",
        )
        _wait_full_adjacencies(rt, timeout=30)
        run_cmds(
            rt=rt,
            device="r1",
            commands=["config", f"ospf {PROCESS_ID}", f"area {UNUSED_AREA}", "end"],
        )

        step("Verify adjacency, routes, FIB, and forwarding recover after OSPF reboot")
        _wait_full_adjacencies(rt, timeout=120)
        _wait_route_present(
            rt,
            device="r1",
            prefix=R2_LOOP_PREFIX,
            nexthop=r1_peer_ip,
            advertising_router=R2_ROUTER_ID,
            timeout=120,
        )
        _wait_route_present(
            rt,
            device="r2",
            prefix=R1_LOOP_PREFIX,
            nexthop=r2_peer_ip,
            advertising_router=R1_ROUTER_ID,
            timeout=120,
        )
        _wait_sourced_ping(rt, device="r1", source=R1_ROUTER_ID, destination=R2_ROUTER_ID, timeout=60)
        _wait_sourced_ping(rt, device="r2", source=R2_ROUTER_ID, destination=R1_ROUTER_ID, timeout=60)

        step("Shut r1 GE-1 and verify adjacency and routes are withdrawn")
        run_cmds(
            rt=rt,
            device="r1",
            commands=["config", f"if {GE_IF}", "shutdown", "exit", "end"],
        )
        _wait_neighbor_absent(rt, device="r1", peer_router_id=R2_ROUTER_ID)
        _wait_neighbor_absent(rt, device="r2", peer_router_id=R1_ROUTER_ID)
        _wait_route_absent(rt, device="r1", prefix=R2_LOOP_PREFIX)
        _wait_route_absent(rt, device="r2", prefix=R1_LOOP_PREFIX)

        step("Restore r1 GE-1 and verify OSPF and forwarding recover")
        run_cmds(
            rt=rt,
            device="r1",
            commands=["config", f"if {GE_IF}", "no shutdown", "exit", "end"],
        )
        _wait_full_adjacencies(rt)
        _wait_route_present(
            rt, device="r1", prefix=R2_LOOP_PREFIX, nexthop=r1_peer_ip, advertising_router=R2_ROUTER_ID
        )
        _wait_route_present(
            rt, device="r2", prefix=R1_LOOP_PREFIX, nexthop=r2_peer_ip, advertising_router=R1_ROUTER_ID
        )
        _wait_sourced_ping(rt, device="r1", source=R1_ROUTER_ID, destination=R2_ROUTER_ID)
        _wait_sourced_ping(rt, device="r2", source=R2_ROUTER_ID, destination=R1_ROUTER_ID)

        _verify_process_delete_cascade(rt)

        print("OSPFv2 basic point-to-point check passed.")
    except BaseException as error:
        test_error = error
        raise
    finally:
        if not should_skip_cleanup():
            _final_cleanup(rt, test_error)
