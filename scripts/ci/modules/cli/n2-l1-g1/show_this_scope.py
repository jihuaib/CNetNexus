#!/usr/bin/env python3
"""
Scoped `show this` coverage for CLI-owned config views.

Covers:
- interface view: IF anchor body plus ISIS cross-module contribution
- ISIS view: only the ISIS top-level block
- BGP root view: root config plus AF/VRF/BMP child blocks
- BGP VRF/AF/BMP child views: only the current scoped block
- ACCESS line and SBMP views: only the current scoped block
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
BGP_VRF_NAME = "ci-show-this"
BGP_VRF_ROUTER_ID = "11.11.11.11"
BGP_VRF_RD = "65001:101"
BMP_NAME = "ci-bmp"
BMP_COLLECTOR_IP = "192.0.2.9"
BMP_COLLECTOR_PORT = 5000
BMP_STATS_INTERVAL = 60
BMP_RECONNECT_INTERVAL = 45
SBMP_SERVER_PORT = 5010
VTY_FIRST = 1
VTY_LAST = 2
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
            f"no vrf {BGP_VRF_NAME}",
            f"line vty {VTY_FIRST} {VTY_LAST}",
            "transport input none",
            "exit",
            "no bmp-server",
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
                "cost-style wide",
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

        step("Configure BGP VRF child view on r1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"vrf {BGP_VRF_NAME}",
                "af ipv4",
                f"route-distinguisher {BGP_VRF_RD}",
                "exit",
                "exit",
                f"bgp {BGP_AS_R1}",
                f"vrf {BGP_VRF_NAME}",
                f"router-id {BGP_VRF_ROUTER_ID}",
                f"neighbor {r1_peer_ip} as {BGP_AS_R2}",
                "af ipv4-unicast",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Configure ACCESS line view and SBMP view on r1")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"line vty {VTY_FIRST} {VTY_LAST}",
                "transport input telnet",
                "end",
                "config",
                "bmp-server",
                f"server port {SBMP_SERVER_PORT}",
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
                "cost-style wide",
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
                f"vrf {BGP_VRF_NAME}",
                f"route-distinguisher {BGP_VRF_RD}",
                f"router-id {BGP_VRF_ROUTER_ID}",
                f"neighbor {r1_peer_ip} as {BGP_AS_R2}",
                f"line vty {VTY_FIRST} {VTY_LAST}",
                "transport input telnet",
                "bmp-server",
                f"server port {SBMP_SERVER_PORT}",
            ],
            regex=[
                rf"(?m)^  af ipv4-unicast\s*$",
                rf"(?m)^   import-route static\s*$",
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
                "cost-style wide",
            ],
            not_regex=[
                rf"(?mi)^\s*if\s+{GE_IF}\s*$",
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*af\s+ipv4-unicast\s*$",
                rf"(?mi)^\s*bmp\s+instance\s+{BMP_NAME}\s*$",
            ],
            count={f"isis {TAG}": 1},
        )

        step("Verify ACCESS line-view show this only includes the selected line block")
        line_out = _show_this(rt, "r1", [f"line vty {VTY_FIRST} {VTY_LAST}"])
        _assert_show_this(
            "line show this",
            line_out,
            contains=[
                f"line vty {VTY_FIRST} {VTY_LAST}",
                "transport input telnet",
            ],
            not_regex=[
                rf"(?mi)^\s*line\s+console\s+0\s*$",
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
                rf"(?mi)^\s*bmp-server\s*$",
            ],
            count={f"line vty {VTY_FIRST} {VTY_LAST}": 1},
        )

        step("Verify ACCESS console-view show this only includes the console block")
        console_out = _show_this(rt, "r1", ["line console 0"])
        _assert_show_this(
            "line console show this",
            console_out,
            contains=["line console 0"],
            not_regex=[
                rf"(?mi)^\s*line\s+vty\s+{VTY_FIRST}\s+{VTY_LAST}\s*$",
                rf"(?mi)^\s*transport\s+input\s+telnet\s*$",
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
            ],
            count={"line console 0": 1},
        )

        step("Verify SBMP-view show this only includes the SBMP block")
        sbmp_out = _show_this(rt, "r1", ["bmp-server"])
        _assert_show_this(
            "sbmp show this",
            sbmp_out,
            contains=[
                "bmp-server",
                f"server port {SBMP_SERVER_PORT}",
            ],
            not_regex=[
                rf"(?mi)^\s*line\s+vty\s+{VTY_FIRST}\s+{VTY_LAST}\s*$",
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
            ],
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
                f"vrf {BGP_VRF_NAME}",
                f"router-id {BGP_VRF_ROUTER_ID}",
            ],
            regex=[
                rf"(?m)^  af ipv4-unicast\s*$",
                rf"(?m)^   import-route static\s*$",
            ],
            not_regex=[
                rf"(?mi)^\s*if\s+{GE_IF}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
            ],
            count={
                f"bgp {BGP_AS_R1}": 1,
                "af ipv4-unicast": 2,
                f"bmp instance {BMP_NAME}": 1,
            },
        )

        step("Verify BGP VRF-view show this only includes the current VRF block")
        bgp_vrf_out = _show_this(rt, "r1", [f"bgp {BGP_AS_R1}", f"vrf {BGP_VRF_NAME}"])
        _assert_show_this(
            "bgp vrf show this",
            bgp_vrf_out,
            contains=[
                f"vrf {BGP_VRF_NAME}",
                f"router-id {BGP_VRF_ROUTER_ID}",
                f"neighbor {r1_peer_ip} as {BGP_AS_R2}",
                "af ipv4-unicast",
                "import-route static",
            ],
            regex=[
                rf"(?m)^  af ipv4-unicast\s*$",
                rf"(?m)^   import-route static\s*$",
            ],
            not_regex=[
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?m)^ af ipv4-unicast\s*$",
                rf"(?mi)^\s*bmp\s+instance\s+{BMP_NAME}\s*$",
                rf"(?mi)^\s*if\s+{GE_IF}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
            ],
            count={f"vrf {BGP_VRF_NAME}": 1, "af ipv4-unicast": 1},
        )

        step("Verify BGP VRF AF-view show this only includes the current VRF AF block")
        bgp_vrf_af_out = _show_this(rt, "r1", [f"bgp {BGP_AS_R1}", f"vrf {BGP_VRF_NAME}", "af ipv4-unicast"])
        _assert_show_this(
            "bgp vrf af show this",
            bgp_vrf_af_out,
            contains=[
                "af ipv4-unicast",
                "import-route static",
            ],
            regex=[
                rf"(?m)^  af ipv4-unicast\s*$",
                rf"(?m)^   import-route static\s*$",
            ],
            not_regex=[
                rf"(?mi)^\s*bgp\s+{BGP_AS_R1}\s*$",
                rf"(?mi)^\s*vrf\s+{BGP_VRF_NAME}\s*$",
                rf"(?m)^ af ipv4-unicast\s*$",
                rf"(?mi)^\s*bmp\s+instance\s+{BMP_NAME}\s*$",
                rf"(?mi)^\s*if\s+{GE_IF}\s*$",
                rf"(?mi)^\s*isis\s+{TAG}\s*$",
            ],
            count={"af ipv4-unicast": 1},
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
