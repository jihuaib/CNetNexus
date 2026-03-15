#!/usr/bin/env python3
"""
BGP import-route check script.

Goal:
- enable `import-route static` on r2
- inject a static route on r2
- verify r1 learns the route via BGP
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

from module_api import require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime, find_peer_ip  # noqa: E402


DEVICE_CFG: dict[str, dict[str, Any]] = {
    "r1": {"asn": 65001, "router_id": "1.1.1.1"},
    "r2": {"asn": 65002, "router_id": "2.2.2.2"},
}

SESSION_TIMEOUT_SEC = 30
ROUTE_TIMEOUT_SEC = 30
IMPORT_PREFIX = "10.20.20.0"
IMPORT_MASK = "255.255.255.0"
IMPORT_CIDR = "10.20.20.0/24"


def ensure_bgp_base(rt: TopologyRuntime) -> None:
    step("Ensure BGP base config")
    for device, cfg in DEVICE_CFG.items():
        # strict=False keeps this script idempotent when base config already exists.
        run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=[
                "config",
                f"bgp {int(cfg['asn'])}",
                f"router-id {cfg['router_id']}",
                "end",
            ],
        )


def ensure_neighbors_and_import(rt: TopologyRuntime, top: dict[str, Any]) -> list[dict[str, object]]:
    step("Ensure BGP neighbors + import-route static")

    r1_peer_ip = find_peer_ip(top, "r1", "r2", local_if="GE-1")
    r2_peer_ip = find_peer_ip(top, "r2", "r1", local_if="GE-1")

    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"bgp {DEVICE_CFG['r1']['asn']}",
            f"neighbor {r1_peer_ip} as {DEVICE_CFG['r2']['asn']}",
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
            f"bgp {DEVICE_CFG['r2']['asn']}",
            f"neighbor {r2_peer_ip} as {DEVICE_CFG['r1']['asn']}",
            "af ipv4-unicast",
            f"neighbor {r2_peer_ip} enable",
            "import-route static",
            "exit",
            "end",
        ],
    )

    return [
        {
            "device": "r1",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r1_peer_ip, "Established"],
            "label": "r1->r2 ipv4-unicast",
        },
        {
            "device": "r2",
            "command": "show bgp neighbor af ipv4-unicast",
            "contains": [r2_peer_ip, "Established"],
            "label": "r2->r1 ipv4-unicast",
        },
    ]


def inject_static_on_r2(rt: TopologyRuntime, top: dict[str, Any]) -> None:
    step("Inject static route on r2 for import-route")
    nexthop = find_peer_ip(top, "r2", "r1", local_if="GE-1")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"route ipv4 {IMPORT_PREFIX} {IMPORT_MASK} {nexthop}",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, Any]) -> None:
    """
    Entry called by module_runner.
    """
    require_devices(top, DEVICE_CFG.keys())

    ensure_bgp_base(rt)
    session_checks = ensure_neighbors_and_import(rt, top)

    step("Wait BGP sessions")
    wait_checks(rt, session_checks, timeout=SESSION_TIMEOUT_SEC)

    inject_static_on_r2(rt, top)

    step("Check imported route appears on r1")
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show bgp route af ipv4-unicast",
                "contains": [IMPORT_CIDR],
                "label": "r1 learned imported static from r2",
            }
        ],
        timeout=ROUTE_TIMEOUT_SEC,
    )

    print("BGP import-route check passed.")
