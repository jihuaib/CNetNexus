#!/usr/bin/env python3
"""
NetNexus <-> FRR eBGP IPv4 interop smoke test.
"""

from __future__ import annotations

import re

from module_api import frr_config, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


NN_AS = 65001
FRR_AS = 65002
NN_PREFIX = "10.101.0.0"
NN_PREFIX_LEN = 24
FRR_PREFIX = "10.202.0.0/24"


def _cleanup(rt: TopologyRuntime, nn_peer_ip: str) -> None:
    step("Cleanup NetNexus/FRR BGP interop config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            f"no route ipv4 {NN_PREFIX} {NN_PREFIX_LEN} {nn_peer_ip}",
            "no bgp",
            "end",
        ],
    )
    frr_config(
        rt,
        "f1",
        [
            f"no ip route {FRR_PREFIX} Null0",
            f"no router bgp {FRR_AS}",
        ],
        strict=False,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "f1"))
    nn_peer_ip = "10.12.0.2"
    frr_peer_ip = "10.12.0.1"

    try:
        step("Configure NetNexus BGP")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"bgp {NN_AS}",
                "router-id 1.1.1.1",
                f"neighbor {nn_peer_ip} as {FRR_AS}",
                "af ipv4-unicast",
                f"neighbor {nn_peer_ip} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Configure FRR BGP")
        frr_config(
            rt,
            "f1",
            [
                f"ip route {FRR_PREFIX} Null0",
                f"router bgp {FRR_AS}",
                "bgp router-id 2.2.2.2",
                "no bgp ebgp-requires-policy",
                f"neighbor {frr_peer_ip} remote-as {NN_AS}",
                "address-family ipv4 unicast",
                f"network {FRR_PREFIX}",
                "exit-address-family",
            ],
        )

        step("Wait eBGP session Established on both sides")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "contains": [nn_peer_ip],
                    "regex": [rf"(?im)^\s*{re.escape(nn_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "NetNexus sees FRR Established",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show bgp ipv4 unicast summary json'",
                    "contains": [frr_peer_ip],
                    "regex": [r'"state"\s*:\s*"Established"'],
                    "label": "FRR sees NetNexus Established",
                },
            ],
            timeout=60,
        )

        step("Advertise NetNexus static route to FRR")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "config",
                f"route ipv4 {NN_PREFIX} {NN_PREFIX_LEN} {nn_peer_ip}",
                "end",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "f1",
                    "command": f"vtysh -c 'show bgp ipv4 unicast {NN_PREFIX}/{NN_PREFIX_LEN}'",
                    "contains": [f"{NN_PREFIX}/{NN_PREFIX_LEN}", frr_peer_ip],
                    "label": "FRR learned NetNexus route",
                }
            ],
            timeout=60,
        )

        step("Verify NetNexus learns FRR originated route")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [FRR_PREFIX],
                    "label": "NetNexus learned FRR route",
                }
            ],
            timeout=60,
        )

        step("Withdraw FRR route and verify NetNexus removes it")
        frr_config(
            rt,
            "f1",
            [
                f"router bgp {FRR_AS}",
                "address-family ipv4 unicast",
                f"no network {FRR_PREFIX}",
                "exit-address-family",
                f"no ip route {FRR_PREFIX} Null0",
            ],
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [FRR_PREFIX],
                    "label": "NetNexus withdrew FRR route",
                }
            ],
            timeout=60,
        )

        print("NetNexus <-> FRR IPv4 eBGP interop check passed.")
    finally:
        _cleanup(rt, nn_peer_ip)
