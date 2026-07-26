#!/usr/bin/env python3
"""
RPM generic route-policy and BGP export-policy integration.

The important ordering in this case is intentional:
  1. BGP rejects a reference to a policy that does not exist.
  2. A generic route-policy is created and bound to an established BGP peer.
  3. ``apply med`` is added, overwritten, restored after reboot, then removed
     while the BGP binding remains in place.

That proves BGP consumes RPM UPSERT events and recomputes its exported routes;
the policy does not have to be fully populated before a business module binds
to it.
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    check_output,
    cmd,
    g_top,
    mark_step_failed,
    reboot_device,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


POLICY = "CI_RPM_EXPORT"
PERMIT_PREFIX = "10.60.60.0"
DENY_PREFIX = "10.60.61.0"
PREFIX_LEN = 24


def _assert_output(
    label: str,
    output: str,
    *,
    contains: tuple[str, ...] = (),
    not_contains: tuple[str, ...] = (),
    regex: tuple[str, ...] = (),
    not_regex: tuple[str, ...] = (),
) -> None:
    violations = check_output(
        output,
        contains=contains,
        not_contains=not_contains,
        regex=regex,
        not_regex=not_regex,
    )
    if violations:
        mark_step_failed()
        raise AssertionError(f"{label}: {'; '.join(violations)}\n{output}")


def _cleanup(rt: TopologyRuntime, r1_peer_ip: str) -> None:
    run_cmds(
        rt,
        "r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no route static ipv4 {PERMIT_PREFIX} {PREFIX_LEN} {r1_peer_ip}",
            f"no route static ipv4 {DENY_PREFIX} {PREFIX_LEN} {r1_peer_ip}",
            "no bgp",
            f"no route-policy {POLICY}",
            "end",
        ],
    )
    run_cmds(
        rt,
        "r2",
        strict=False,
        commands=["end", "config", "no bgp", "end"],
    )


def _route_detail_cmd() -> str:
    return f"show bgp route af ipv4-unicast {PERMIT_PREFIX} {PREFIX_LEN}"


def _wait_med(rt: TopologyRuntime, med: str, *, label: str, timeout: int = 40) -> None:
    wait_check(
        rt,
        device="r2",
        command=_route_detail_cmd(),
        regex=[rf"(?im)^\s*MED\s*:\s*{re.escape(med)}\s*$"],
        timeout=timeout,
        label=label,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    # Keep this case independently repeatable even when a previous run failed
    # after saving a startup snapshot.
    _cleanup(rt, r1_peer_ip)

    try:
        step("Configure IPv4 BGP peers and static-route import")
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                f"neighbor {r1_peer_ip} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "import-route static",
                "end",
            ],
        )
        run_cmds(
            rt,
            "r2",
            commands=[
                "config",
                "bgp 65002",
                "router-id 2.2.2.2",
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "end",
            ],
        )

        step("Reject a BGP reference to a missing RPM policy")
        run_cmds(rt, "r1", commands=["config", "bgp 65001", "af ipv4-unicast"])
        missing_out = cmd(
            rt,
            "r1",
            f"neighbor {r1_peer_ip} route-policy {POLICY} export",
            strict=False,
        )
        _assert_output(
            "missing policy validation",
            missing_out,
            contains=("Export policy does not exist",),
        )
        cmd(rt, "r1", "end")

        step("Create a generic route-policy and bind it to BGP")
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"route-policy {POLICY} permit node 10",
                f"if-match network ipv4 {PERMIT_PREFIX} {PREFIX_LEN}",
                "end",
                "config",
                "bgp 65001",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} route-policy {POLICY} export",
                "end",
            ],
        )

        config_out = cmd(rt, "r1", "show current-configuration")
        _assert_output(
            "generic RPM configuration",
            config_out,
            contains=(
                f"route-policy {POLICY} permit node 10",
                f"if-match network ipv4 {PERMIT_PREFIX} {PREFIX_LEN}",
                f"neighbor {r1_peer_ip} route-policy {POLICY} export",
            ),
            not_contains=("bgp-export",),
        )

        session_checks = [
            {
                "device": "r1",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [
                    rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"
                ],
                "label": "r1 BGP session established",
            },
            {
                "device": "r2",
                "command": "show bgp neighbor af ipv4-unicast",
                "regex": [
                    rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"
                ],
                "label": "r2 BGP session established",
            },
        ]
        step("Wait for BGP session establishment")
        wait_checks(rt, session_checks, timeout=40)

        step("Import one matching and one non-matching static route")
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"route static ipv4 {PERMIT_PREFIX} {PREFIX_LEN} {r1_peer_ip}",
                f"route static ipv4 {DENY_PREFIX} {PREFIX_LEN} {r1_peer_ip}",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [f"{PERMIT_PREFIX}/{PREFIX_LEN}"],
                    "not_contains": [f"{DENY_PREFIX}/{PREFIX_LEN}"],
                    "label": "if-match permits only the selected prefix",
                }
            ],
            timeout=40,
        )
        _wait_med(rt, "-", label="bound policy initially has no MED")

        step("Add apply MED after the policy is already bound")
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"route-policy {POLICY} permit node 10",
                "apply med 100",
                "end",
            ],
        )
        _wait_med(rt, "100", label="RPM UPSERT changes exported MED to 100")

        step("Overwrite apply MED while the BGP binding remains unchanged")
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"route-policy {POLICY} permit node 10",
                "apply med 200",
                "end",
            ],
        )
        _wait_med(rt, "200", label="RPM UPSERT overwrites exported MED with 200")
        config_out = cmd(rt, "r1", "show current-configuration")
        _assert_output(
            "overwritten apply serialization",
            config_out,
            contains=("apply med 200",),
            not_contains=("apply med 100",),
        )

        step("Save and reboot r1; RPM must replay before the BGP reference")
        reboot_device(rt, "r1", timeout=120, save_config=True)
        wait_checks(rt, session_checks, timeout=40)
        _wait_med(rt, "200", label="overwritten MED survives startup replay", timeout=40)

        step("Delete apply MED after reboot while the policy remains bound")
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"route-policy {POLICY} permit node 10",
                "no apply med",
                "end",
            ],
        )
        _wait_med(rt, "-", label="RPM UPSERT removes MED from exported route")
        config_out = cmd(rt, "r1", "show current-configuration")
        _assert_output(
            "deleted apply serialization",
            config_out,
            not_contains=("apply med 100", "apply med 200"),
        )

        print("RPM/BGP dynamic export-policy check passed.")
    finally:
        _cleanup(rt, r1_peer_ip)
