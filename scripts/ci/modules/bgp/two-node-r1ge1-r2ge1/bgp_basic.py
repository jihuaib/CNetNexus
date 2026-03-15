#!/usr/bin/env python3
"""
BGP basic check script.

This file is a case script loaded by `scripts/ci/module_runner.py`.
Runner lifecycle:
- load case top.yaml once
- start topology runtime once per case directory
- run all scripts in this case directory
- cleanup runtime once after all scripts
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any


SELF_PATH = Path(__file__).resolve()
CI_DIR: Path | None = None
for parent in SELF_PATH.parents:
    if (parent / "module_api.py").exists() and (parent / "top_runner.py").exists():
        CI_DIR = parent
        break
if CI_DIR is None:
    raise SystemExit(f"failed to locate scripts/ci directory from {SELF_PATH}")

if str(CI_DIR) not in sys.path:
    sys.path.insert(0, str(CI_DIR))

from module_api import reboot_device, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime, find_peer_ip  # noqa: E402


BGP_DEVICE_CFG: dict[str, dict[str, Any]] = {
    "r1": {"asn": 65001, "router_id": "1.1.1.1"},
    "r2": {"asn": 65002, "router_id": "2.2.2.2"},
}

BGP_SESSIONS: list[dict[str, Any]] = [
    {"local": "r1", "peer": "r2", "local_if": "GE-1", "afs": ["ipv4-unicast"], "import_static": True},
    {"local": "r2", "peer": "r1", "local_if": "GE-1", "afs": ["ipv4-unicast"], "import_static": False},
]

STATIC_ROUTES: list[dict[str, str]] = [
    {"device": "r1", "prefix": "10.10.10.0", "mask": "255.255.255.0", "nexthop": "10.12.0.2"},
]

SESSION_TIMEOUT_SEC = 30
ROUTE_TIMEOUT_SEC = 30
REBOOT_RECOVER_TIMEOUT_SEC = 90
ROUTE_CHECKS: list[dict[str, str]] = [
    {"device": "r1", "af": "ipv4-unicast", "prefix": "10.10.10.0/24"},
]


def validate_session_references(top: dict[str, Any]) -> None:
    devs = set(top["devices"].keys())
    for sess in BGP_SESSIONS:
        if sess["local"] not in devs or sess["peer"] not in devs:
            raise ValueError(f"invalid session local/peer in script config: {sess}")


def build_session_checks(rt: TopologyRuntime, top: dict[str, Any]) -> list[dict[str, object]]:
    step("Configure BGP base")
    for device, cfg in BGP_DEVICE_CFG.items():
        run_cmds(
            rt=rt,
            device=device,
            commands=[
                "config",
                f"bgp {int(cfg['asn'])}",
                f"router-id {cfg['router_id']}",
                "end",
            ],
        )

    step("Configure BGP neighbors")
    session_checks: list[dict[str, object]] = []
    for sess in BGP_SESSIONS:
        local_device = str(sess["local"])
        peer_device = str(sess["peer"])
        afs = list(sess.get("afs", ["ipv4-unicast"]))
        local_if = sess.get("local_if")
        peer_ip = find_peer_ip(top, local_device, peer_device, local_if=local_if)

        run_cmds(
            rt=rt,
            device=local_device,
            commands=[
                "config",
                f"bgp {int(BGP_DEVICE_CFG[local_device]['asn'])}",
                f"neighbor {peer_ip} as {int(BGP_DEVICE_CFG[peer_device]['asn'])}",
            ],
        )
        for af in afs:
            run_cmds(rt=rt, device=local_device, commands=[f"af {af}", f"neighbor {peer_ip} enable"])
            if bool(sess.get("import_static", False)):
                run_cmds(rt=rt, device=local_device, commands=["import-route static"])
            run_cmds(rt=rt, device=local_device, commands=["exit"])
            session_checks.append(
                {
                    "device": local_device,
                    "command": f"show bgp neighbor af {af}",
                    "contains": [peer_ip, "Established"],
                    "label": f"{local_device}->{peer_device} {af}",
                }
            )
        run_cmds(rt=rt, device=local_device, commands=["end"])

    return session_checks


def apply_static_routes(rt: TopologyRuntime, *, strict: bool = True) -> None:
    step("Apply static routes")
    for route in STATIC_ROUTES:
        run_cmds(
            rt=rt,
            device=str(route["device"]),
            strict=strict,
            commands=[
                "config",
                f"route ipv4 {route['prefix']} {route['mask']} {route['nexthop']}",
                "end",
            ],
        )


def run(rt: TopologyRuntime, top: dict[str, Any]) -> None:
    """
    Entry called by module_runner.
    """
    require_devices(top, BGP_DEVICE_CFG.keys())
    validate_session_references(top)

    session_checks = build_session_checks(rt, top)
    apply_static_routes(rt, strict=True)

    route_checks = [
        {
            "device": str(chk["device"]),
            "command": f"show bgp route af {chk.get('af', 'ipv4-unicast')}",
            "contains": [str(chk["prefix"])],
            "label": f"{chk['device']} route {chk['prefix']}",
        }
        for chk in ROUTE_CHECKS
    ]

    step("Wait BGP sessions")
    wait_checks(rt, session_checks, timeout=SESSION_TIMEOUT_SEC)

    step("Wait BGP routes")
    wait_checks(rt, route_checks, timeout=ROUTE_TIMEOUT_SEC)

    step("Reboot r1 and wait CLI reconnect")
    reboot_device(rt, "r1", timeout=REBOOT_RECOVER_TIMEOUT_SEC)

    step("Wait BGP sessions after reboot")
    wait_checks(rt, session_checks, timeout=SESSION_TIMEOUT_SEC)

    step("Replay static routes after reboot")
    apply_static_routes(rt, strict=False)

    step("Wait BGP routes after reboot")
    wait_checks(rt, route_checks, timeout=ROUTE_TIMEOUT_SEC)

    print("BGP basic check passed.")
