#!/usr/bin/env python3
"""
SBMP module end-to-end check (linear topology: s1 --- r1 --- r2).

Roles:
- s1: BMP server (SBMP)
- r1: BGP speaker + BMP client reporter
- r2: BGP peer that generates routes for r1

Coverage:
- SBMP server config/show and no-commands
- BGP BMP client config/show and no-commands
- monitor all/specific/none transitions
- Peer Up/Down and Route Monitoring message flow to SBMP
- SBMP show client/peer/route (with and without filters)
"""

from __future__ import annotations

import re

from module_api import g_top, require_devices, run_cmds, step, wait_check, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


BMP_PORT = 11019
BMP_INST = "ci-r1"
R2_STATIC_PFX = "10.120.20.0"
R2_STATIC_MASK = "24"


def _session_checks(r1_peer_ip: str, r2_peer_ip: str) -> list[dict[str, object]]:
    return [
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r1_peer_ip],
                "regex": [rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r1->r2 established",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r2_peer_ip],
                "regex": [rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
            "label": "r2->r1 established",
        },
    ]


def _cleanup(rt: TopologyRuntime, *, r2_route_nh: str) -> None:
    step("Cleanup SBMP/BGP/static config")
    run_cmds(rt=rt, device="s1", strict=False, commands=["config", "no bmp-server", "end"])
    run_cmds(rt=rt, device="r1", strict=False, commands=["config", "no bgp", "end"])
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {R2_STATIC_PFX} {R2_STATIC_MASK} {r2_route_nh}",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("s1", "r1", "r2"))

    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_route_nh = str(g_top.r2.GE_1.peer_ip)

    r1_collector_ip = str(g_top.r1.GE_2.peer_ip)
    r1_client_id = str(g_top.r1.GE_2.ip)

    try:
        step("Configure SBMP server")
        run_cmds(
            rt=rt,
            device="s1",
            strict=False,
            commands=["config", "bmp-server", f"server port {BMP_PORT}", "end"],
        )
        wait_check(
            rt,
            device="s1",
            command="show bmp-server",
            timeout=20,
            contains=[
                f"Port             : {BMP_PORT}",
                "State            : running",
                "Clients(total/up): 0/0",
            ],
            label="s1 bmp-server up",
        )

        step("Configure r1 BMP client and r1/r2 BGP")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                "timer connect-retry 5",
                f"bmp instance {BMP_INST}",
                f"collector {r1_collector_ip} port {BMP_PORT}",
                "stats-report interval 15",
                "reconnect interval 5",
                "monitor neighbor all",
                "exit",
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
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "router-id 2.2.2.2",
                "timer connect-retry 5",
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Wait BGP and BMP connections")
        wait_checks(rt, _session_checks(r1_peer_ip, r2_peer_ip), timeout=40)
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show bgp bmp instance {BMP_INST}",
                    "contains": [
                        f"BMP Instance: {BMP_INST}",
                        f"Collector:          {r1_collector_ip} port {BMP_PORT}",
                        "Stats interval:     15 sec",
                        "Reconnect interval: 5 sec",
                        "Monitor:            all neighbors",
                        "State:              Up",
                    ],
                    "regex": [
                        r"Initiation sent:\s*[1-9]\d*",
                        r"Peer Up sent:\s*[1-9]\d*",
                    ],
                    "label": "r1 bmp instance up",
                },
                {
                    "device": "s1",
                    "command": "show bmp-server",
                    "contains": [
                        f"Port             : {BMP_PORT}",
                        "State            : running",
                        "Clients(total/up): 1/1",
                        "Peers(total)     : 1",
                    ],
                    "label": "s1 sees one bmp client",
                },
                {
                    "device": "s1",
                    "command": "show bmp-server client",
                    "contains": [r1_client_id, "connected"],
                    "label": "s1 client summary",
                },
            ],
            timeout=40,
        )

        step("Inject route on r2 and wait r1 learns it")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"route ipv4 {R2_STATIC_PFX} {R2_STATIC_MASK} {r2_route_nh}", "end"],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{R2_STATIC_PFX}/{R2_STATIC_MASK}"],
                    "label": "r2 local imported static route",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{R2_STATIC_PFX}/{R2_STATIC_MASK}"],
                    "label": "r1 learned route from r2",
                },
            ],
            timeout=40,
        )

        step("Validate SBMP show client/peer/route commands")
        wait_checks(
            rt,
            [
                {
                    "device": "s1",
                    "command": f"show bmp-server client {r1_client_id}",
                    "contains": [
                        f"BMP Client: {r1_client_id}",
                        "Connected       : yes",
                        "Peers           : 1",
                    ],
                    "regex": [r"Msg RM\(total/pre/post/loc\):\s*[1-9]\d*/[1-9]\d*/\d+/\d+"],
                    "label": "s1 client detail with route-monitor counters",
                },
                {
                    "device": "s1",
                    "command": "show bmp-server peer",
                    "contains": [r1_client_id, r1_peer_ip, "up"],
                    "label": "s1 peer summary",
                },
                {
                    "device": "s1",
                    "command": f"show bmp-server peer client {r1_client_id} peer {r1_peer_ip}",
                    "contains": [r1_client_id, r1_peer_ip, "up"],
                    "label": "s1 peer filtered",
                },
                {
                    "device": "s1",
                    "command": "show bmp-server route af ipv4-unicast policy pre",
                    "contains": ["BMP Routes (AF: ipv4-unicast, Policy: pre)"],
                    "not_contains": ["(no routes)"],
                    "regex": [r"Total Listed Routes:\s*[1-9]\d*"],
                    "label": "s1 route pre-policy has entries",
                },
                {
                    "device": "s1",
                    "command": f"show bmp-server route af ipv4-unicast client {r1_client_id} peer {r1_peer_ip} policy pre",
                    "contains": [
                        f"Client: {r1_client_id}  Policy: pre",
                        r1_peer_ip,
                        f"{R2_STATIC_PFX}/{R2_STATIC_MASK}",
                    ],
                    "label": "s1 route filtered by client+peer",
                },
                {
                    "device": "s1",
                    "command": "show bmp-server route af ipv4-unicast policy post",
                    "contains": ["BMP Routes (AF: ipv4-unicast, Policy: post)", "(no routes)"],
                    "label": "s1 route post-policy command",
                },
                {
                    "device": "s1",
                    "command": "show bmp-server route af ipv4-unicast policy loc-rib",
                    "contains": ["BMP Routes (AF: ipv4-unicast, Policy: loc-rib)", "(no routes)"],
                    "label": "s1 route loc-rib command",
                },
            ],
            timeout=40,
        )

        step("Exercise monitor neighbor specific/none/all")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"bmp instance {BMP_INST}",
                f"monitor neighbor {r1_peer_ip}",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show bgp bmp instance {BMP_INST}",
            timeout=20,
            contains=["Monitor:            specific neighbors", f"- {r1_peer_ip}"],
            label="r1 monitor specific neighbor",
        )

        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"bmp instance {BMP_INST}",
                f"no monitor neighbor {r1_peer_ip}",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show bgp bmp instance {BMP_INST}",
            timeout=20,
            contains=["Monitor:            none"],
            label="r1 monitor none",
        )

        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"bmp instance {BMP_INST}",
                "monitor neighbor all",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show bgp bmp instance {BMP_INST}",
            timeout=20,
            contains=["Monitor:            all neighbors"],
            label="r1 monitor all restored",
        )

        step("Trigger peer-down and verify counters")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "af ipv4-unicast",
                f"no neighbor {r2_peer_ip}",
                "exit",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "not_contains": ["Established"],
                    "label": "r1 session down",
                },
                {
                    "device": "r1",
                    "command": f"show bgp bmp instance {BMP_INST}",
                    "regex": [r"Peer Down sent:\s*[1-9]\d*"],
                    "label": "r1 peer-down counter",
                },
                {
                    "device": "s1",
                    "command": f"show bmp-server peer client {r1_client_id} peer {r1_peer_ip}",
                    "contains": [r1_client_id, r1_peer_ip, "down"],
                    "label": "s1 peer state down",
                },
                {
                    "device": "s1",
                    "command": f"show bmp-server client {r1_client_id}",
                    "regex": [r"Msg UP/DOWN/STAT:\s*\d+/[1-9]\d*/\d+"],
                    "label": "s1 peer-down message counter",
                },
            ],
            timeout=40,
        )

        step("Exercise no stats/no reconnect/no collector")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"bmp instance {BMP_INST}",
                "no stats-report interval",
                "no reconnect interval",
                "no collector",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show bgp bmp instance {BMP_INST}",
            timeout=20,
            contains=[
                "Collector:          (not configured)",
                "Stats interval:     0 sec (disabled)",
                "Reconnect interval: 30 sec",
                "State:              Idle",
            ],
            label="r1 no-commands applied",
        )

        step("Exercise no bmp instance")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", "bgp 65001", f"no bmp instance {BMP_INST}", "end"],
        )
        wait_check(
            rt,
            device="r1",
            command="show bgp bmp",
            timeout=20,
            contains=["(no BMP instances configured)"],
            not_contains=[BMP_INST],
            label="r1 bmp instance removed",
        )

        step("Recreate BMP instance for no-bgp cleanup validation")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                f"bmp instance {BMP_INST}",
                f"collector {r1_collector_ip} port {BMP_PORT}",
                "monitor neighbor all",
                "end",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show bgp bmp instance {BMP_INST}",
            timeout=20,
            contains=[f"BMP Instance: {BMP_INST}", "State:              Up"],
            label="r1 bmp instance recreated",
        )

        step("Exercise no server port and no bmp-server")
        run_cmds(
            rt=rt,
            device="s1",
            strict=False,
            commands=["config", "bmp-server", "no server port", "end"],
        )
        wait_check(
            rt,
            device="s1",
            command="show bmp-server",
            timeout=20,
            contains=["State            : not configured"],
            label="s1 no server port",
        )

        run_cmds(
            rt=rt,
            device="s1",
            strict=False,
            commands=["config", "bmp-server", f"server port {BMP_PORT}", "end"],
        )
        wait_check(
            rt,
            device="s1",
            command="show bmp-server",
            timeout=20,
            contains=[f"Port             : {BMP_PORT}", "State            : running"],
            label="s1 server port restored",
        )

        run_cmds(rt=rt, device="s1", strict=False, commands=["config", "no bmp-server", "end"])
        wait_check(
            rt,
            device="s1",
            command="show bmp-server",
            timeout=20,
            contains=["State            : not configured"],
            label="s1 no bmp-server",
        )

        print("SBMP BMP client-report check passed.")
    finally:
        _cleanup(rt, r2_route_nh=r2_route_nh)
