#!/usr/bin/env python3
"""
Scoped `show this` coverage for CLI-owned config views.

Covers:
- interface view: IF anchor body plus ISIS cross-module contribution
- ISIS view: only the ISIS top-level block
- BGP root view: root config plus AF/BMP child blocks
- BGP AF/BMP child views: only the current scoped block
"""

from __future__ import annotations

from module_api import check_output, g_top, require_devices, run_cmds, step, wait_check  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


TAG = 101
GE_IF = "GE-1"
BGP_AS_R1 = 65001
BGP_AS_R2 = 65002
BGP_ROUTER_ID_R1 = "1.1.1.1"
BGP_ROUTER_ID_R2 = "2.2.2.2"
BMP_NAME = "ci-bmp"
BMP_COLLECTOR_IP = "192.0.2.9"
BMP_COLLECTOR_PORT = 5000
BMP_STATS_INTERVAL = 60
BMP_RECONNECT_INTERVAL = 45
ISIS_NET = "49.0001.0000.0000.0f01.00"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup CLI show-this case config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"if {GE_IF}",
            f"no isis enable {TAG}",
            f"no isis ipv6 enable {TAG}",
            "exit",
            f"no isis {TAG}",
            "no bgp",
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
            "no bgp",
            "end",
        ],
    )


def _show_this(rt: TopologyRuntime, device: str, enter_commands: list[str]) -> str:
    outputs = run_cmds(
        rt=rt,
        device=device,
        strict=False,
        timeout=20,
        commands=["end", "config", *enter_commands, "show this", "end"],
    )
    return outputs[-2] if len(outputs) >= 2 else ""


def _assert_show_this(
    label: str,
    output: str,
    *,
    contains: list[str],
    not_contains: list[str] | None = None,
    regex: list[str] | None = None,
    not_regex: list[str] | None = None,
    count: dict[str, int] | None = None,
) -> None:
    violations = check_output(
        output,
        contains=contains,
        not_contains=not_contains or [],
        regex=regex or [],
        not_regex=not_regex or [],
        count=count or {},
    )
    if violations:
        raise RuntimeError(f"{label} violations: {'; '.join(violations)}\noutput:\n{output}")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    r1_ip4 = str(g_top.r1.GE_1.ip)
    r1_prefix4 = int(g_top.r1.GE_1.prefix)
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    try:
        _cleanup(rt)

        step("Configure ISIS on r1 and attach it to GE-1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"isis {TAG}",
                f"net {ISIS_NET}",
                "is-type level-2",
                "end",
                "config",
                f"if {GE_IF}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                "end",
            ],
        )

        step("Configure BGP root, AF, and BMP child view on r1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {BGP_AS_R1}",
                f"router-id {BGP_ROUTER_ID_R1}",
                f"neighbor {r1_peer_ip} as {BGP_AS_R2}",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "import-route static",
                "exit",
                f"bmp instance {BMP_NAME}",
                f"collector {BMP_COLLECTOR_IP} port {BMP_COLLECTOR_PORT}",
                f"stats-report interval {BMP_STATS_INTERVAL}",
                f"reconnect interval {BMP_RECONNECT_INTERVAL}",
                "monitor neighbor all",
                "end",
            ],
        )

        step("Configure matching BGP peer on r2")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                f"bgp {BGP_AS_R2}",
                f"router-id {BGP_ROUTER_ID_R2}",
                f"neighbor {r2_peer_ip} as {BGP_AS_R1}",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "end",
            ],
        )

        step("Wait aggregated current-config to reflect IF, ISIS, and BGP config")
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            timeout=20,
            interval=2,
            contains=[
                f"if {GE_IF}",
                f"ip address {r1_ip4} {r1_prefix4}",
                f"isis {TAG}",
                f"net {ISIS_NET}",
                "is-type level-2",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
                f"bgp {BGP_AS_R1}",
                f"router-id {BGP_ROUTER_ID_R1}",
                f"neighbor {r1_peer_ip} as {BGP_AS_R2}",
                "af ipv4-unicast",
                "import-route static",
                f"bmp instance {BMP_NAME}",
                f"collector {BMP_COLLECTOR_IP} port {BMP_COLLECTOR_PORT}",
                f"stats-report interval {BMP_STATS_INTERVAL}",
                f"reconnect interval {BMP_RECONNECT_INTERVAL}",
                "monitor neighbor all",
            ],
            label="r1 current-config reflects CLI show-this scenario",
        )

        step("Verify interface-view show this includes IF and ISIS contribution only")
        if_out = _show_this(rt, "r1", [f"if {GE_IF}"])
        _assert_show_this(
            "interface show this",
            if_out,
            contains=[
                f"if {GE_IF}",
                f"ip address {r1_ip4} {r1_prefix4}",
                f"isis enable {TAG}",
                f"isis ipv6 enable {TAG}",
            ],
            not_regex=[
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*af\s+ipv4-unicast\s*$",
                rf"(?mi)^\s*bmp\s+instance\s+{BMP_NAME}\s*$",
            ],
            count={f"if {GE_IF}": 1},
        )

        step("Verify ISIS-view show this only includes the ISIS top-level block")
        isis_out = _show_this(rt, "r1", [f"isis {TAG}"])
        _assert_show_this(
            "isis show this",
            isis_out,
            contains=[
                f"isis {TAG}",
                f"net {ISIS_NET}",
                "is-type level-2",
            ],
            not_regex=[
                rf"(?mi)^\s*if\s+{GE_IF}\s*$",
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*af\s+ipv4-unicast\s*$",
                rf"(?mi)^\s*bmp\s+instance\s+{BMP_NAME}\s*$",
            ],
            count={f"isis {TAG}": 1},
        )

        step("Verify BGP root-view show this includes AF and BMP child blocks")
        bgp_out = _show_this(rt, "r1", [f"bgp {BGP_AS_R1}"])
        _assert_show_this(
            "bgp root show this",
            bgp_out,
            contains=[
                f"bgp {BGP_AS_R1}",
                f"router-id {BGP_ROUTER_ID_R1}",
                f"neighbor {r1_peer_ip} as {BGP_AS_R2}",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "import-route static",
                f"bmp instance {BMP_NAME}",
                f"collector {BMP_COLLECTOR_IP} port {BMP_COLLECTOR_PORT}",
                f"stats-report interval {BMP_STATS_INTERVAL}",
                f"reconnect interval {BMP_RECONNECT_INTERVAL}",
                "monitor neighbor all",
            ],
            not_regex=[
                rf"(?mi)^\s*if\s+{GE_IF}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
            ],
            count={
                f"bgp {BGP_AS_R1}": 1,
                "af ipv4-unicast": 1,
                f"bmp instance {BMP_NAME}": 1,
            },
        )

        step("Verify BGP AF-view show this only includes the current AF block")
        bgp_af_out = _show_this(rt, "r1", [f"bgp {BGP_AS_R1}", "af ipv4-unicast"])
        _assert_show_this(
            "bgp af show this",
            bgp_af_out,
            contains=[
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "import-route static",
            ],
            not_regex=[
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*bmp\s+instance\s+{BMP_NAME}\s*$",
                rf"(?mi)^\s*if\s+{GE_IF}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
            ],
            count={"af ipv4-unicast": 1},
        )

        step("Verify BGP BMP-view show this only includes the current BMP block")
        bgp_bmp_out = _show_this(rt, "r1", [f"bgp {BGP_AS_R1}", f"bmp instance {BMP_NAME}"])
        _assert_show_this(
            "bgp bmp show this",
            bgp_bmp_out,
            contains=[
                f"bmp instance {BMP_NAME}",
                f"collector {BMP_COLLECTOR_IP} port {BMP_COLLECTOR_PORT}",
                f"stats-report interval {BMP_STATS_INTERVAL}",
                f"reconnect interval {BMP_RECONNECT_INTERVAL}",
                "monitor neighbor all",
            ],
            not_regex=[
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*af\s+ipv4-unicast\s*$",
                rf"(?mi)^\s*if\s+{GE_IF}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
            ],
            count={f"bmp instance {BMP_NAME}": 1},
        )

        print("CLI show-this scope check passed.")
    finally:
        _cleanup(rt)
