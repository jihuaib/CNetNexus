#!/usr/bin/env python3
"""
BGP import-route check script.

Goal:
- enable `import-route static` on r2
- inject a static route on r2
- verify r2 imports the static route into local BGP RIB
- verify r1 receives the route from r2
"""

from __future__ import annotations

from module_api import g_top, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


def _cleanup_case_config(rt: TopologyRuntime, r2_local_ip: str) -> None:
    step("Cleanup BGP/static config")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 10.20.20.0 24 {r2_local_ip}",
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    """
    Entry called by module_runner.
    """
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    r2_local_ip = str(g_top.r2.GE_1.ip)

    try:
        step("Ensure BGP base config")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=["config", "bgp 65001", "router-id 1.1.1.1", "end"],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", "bgp 65002", "router-id 2.2.2.2", "end"],
        )

        step("Ensure BGP neighbors + import-route static")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
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
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Wait BGP sessions")
        wait_checks(
            rt,
            [
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
            ],
            timeout=30,
        )

        step("Inject static route on r2 for import-route")
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=["config", f"route ipv4 10.20.20.0 24 {r2_local_ip}", "end"],
        )

        step("Check imported route on r2 local and r1 peer BGP RIB")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": ["10.20.20.0/24"],
                    "label": "r2 local imported static route",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": ["10.20.20.0/24"],
                    "label": "r1 learned route from r2",
                },
            ],
            timeout=30,
        )

        print("BGP import-route check passed.")
    finally:
        _cleanup_case_config(rt, r2_local_ip)
