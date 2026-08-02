#!/usr/bin/env python3
"""SRv6 L3VPN BE End.DT6 end-to-end forwarding check.

Topology:

* r1 -- r2 runs ISIS over GE-1.  Each PE explicitly selects one public IPv6
  locator /64 for advertisement under the ISIS IPv6 address-family.
* Each PE owns a ``red`` VRF IPv6 /128 loopback.  Public eBGP VPNv6 exchanges
  the private routes.  The private IPv6-unicast AF selects the local locator,
  while per-neighbor ``srv6-sid`` in the public VPNv6 AF controls SID
  advertisement and ``srv6 be`` in the private AF controls SID recursion.
* The test first proves the default MPLS tunnel/VPN-label path, then enables
  SID advertisement in one direction before enabling it in both directions.
  It also exercises live BE disable/restore and neighbor SID fallback/restore.

The test intentionally does not write SRv6 sysctls.  FIB owns that runtime
initialization; this case only verifies that all/default/GE-1's Linux interface
are enabled after the LocalSID is installed.
"""

from __future__ import annotations

import ipaddress
import re
import subprocess
import time

from module_api import (  # noqa: E402
    g_top,
    hold_check,
    process_reboot,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
GE_IF = "GE-1"
GE_LINUX_IF = "eth1"

ISIS_TAG = 161
R1_NET = "49.0161.0000.0000.0001.00"
R2_NET = "49.0161.0000.0000.0002.00"

R1_AS = 65001
R2_AS = 65002
R1_RD = "65001:161"
R2_RD = "65002:161"
RT = "65000:161"

LOCATOR_LEN = 64
LOCATOR_FUNCTION_BITS = 16
R1_LOCATOR_NAME = "loc-r1-dt6"
R2_LOCATOR_NAME = "loc-r2-dt6"
R1_LOCATOR_PREFIX = "2001:db8:161:1::"
R2_LOCATOR_PREFIX = "2001:db8:161:2::"
R1_DUMMY_LOCATOR_NAME = "loc-r1-dt6-unused"
R2_DUMMY_LOCATOR_NAME = "loc-r2-dt6-unused"
R1_DUMMY_LOCATOR_PREFIX = "2001:db8:161:11::"
R2_DUMMY_LOCATOR_PREFIX = "2001:db8:161:12::"

PRIVATE_LEN = 128
R1_PRIVATE_LOOP = "2001:db8:6161:1::1"
R2_PRIVATE_LOOP = "2001:db8:6161:2::2"
R1_PRIVATE_LOOP_ID = 73
R2_PRIVATE_LOOP_ID = 74

PING_FAIL_RE = (
    r"(?im)(?:\b100(?:\.0)?%\s+packet loss\b|network is unreachable|"
    r"destination host unreachable|no route to host|connect:|"
    r"module timed out or failed to respond|failed to start ping)"
)
MPLS_LABEL_RE = r"(?:1[6-9]|[2-9]\d|[1-9]\d{2,})"


def _remove_bgp_instance(
    rt: TopologyRuntime,
    *,
    device: str,
    local_as: int,
    timeout: float = 30.0,
    interval: float = 0.5,
) -> None:
    """Retry a deferred ``no bgp`` until the BDR instance is actually gone."""
    instance_re = re.compile(rf"(?m)^bgp {local_as}\r?$")
    valid_config_re = re.compile(r"(?m)^sysname\s+\S+\r?$")
    pending_text = "Pending work did not converge"
    deadline = time.monotonic() + timeout
    attempts = 0
    last_remove_output = ""

    def _read_current_configuration() -> str:
        output = rt.exec_cmd(device, "show current-configuration", timeout=15)
        if not valid_config_re.search(output):
            raise RuntimeError(
                f"{device}: invalid current-configuration output while removing bgp {local_as}: "
                f"{output!r}"
            )
        return output

    while True:
        current = _read_current_configuration()
        if not instance_re.search(current):
            return
        if time.monotonic() >= deadline:
            break

        attempts += 1
        outputs = run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=["end", "config", "no bgp", "end"],
        )
        last_remove_output = outputs[2] if len(outputs) > 2 else ""
        if not instance_re.search(_read_current_configuration()):
            return
        if "Error:" in last_remove_output and pending_text not in last_remove_output:
            raise RuntimeError(
                f"{device}: no bgp returned a non-retryable error: {last_remove_output.strip()}"
            )

        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(interval, remaining))

    raise RuntimeError(
        f"{device}: bgp {local_as} still exists in current-configuration after "
        f"{attempts} no bgp attempt(s) over {timeout:.1f}s; "
        f"last output={last_remove_output.strip()!r}"
    )


def _cleanup(rt: TopologyRuntime) -> None:
    """Release BGP-owned SIDs before deleting the VRF and locator."""
    step("Cleanup SRv6 L3VPN BE End.DT6 test configuration")
    for device, local_as, private_loop_id, locator_name, dummy_locator_name in (
        ("r1", R1_AS, R1_PRIVATE_LOOP_ID, R1_LOCATOR_NAME, R1_DUMMY_LOCATOR_NAME),
        ("r2", R2_AS, R2_PRIVATE_LOOP_ID, R2_LOCATOR_NAME, R2_DUMMY_LOCATOR_NAME),
    ):
        _remove_bgp_instance(rt, device=device, local_as=local_as)
        run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=[
                "end",
                "config",
                f"no isis {ISIS_TAG}",
                f"no if loop {private_loop_id}",
                f"no vrf {VRF_NAME}",
                "srv6",
                f"no locator {locator_name}",
                f"no locator {dummy_locator_name}",
                "exit",
                "no srv6",
                "end",
            ],
        )


def _configure_underlay(
    rt: TopologyRuntime,
    *,
    device: str,
    net: str,
    locator_name: str,
    locator_prefix: str,
    dummy_locator_name: str,
    dummy_locator_prefix: str,
) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            "srv6",
            f"locator {locator_name} prefix {locator_prefix} {LOCATOR_LEN} "
            f"function-bits {LOCATOR_FUNCTION_BITS}",
            f"locator {dummy_locator_name} prefix {dummy_locator_prefix} {LOCATOR_LEN} "
            f"function-bits {LOCATOR_FUNCTION_BITS}",
            "exit",
            f"isis {ISIS_TAG}",
            f"net {net}",
            "is-type level-1-2",
            "cost-style wide",
            "af ipv4",
            "af ipv6",
            "end",
            "config",
            f"if {GE_IF}",
            f"isis enable {ISIS_TAG}",
            f"isis ipv6 enable {ISIS_TAG}",
            f"isis hello-interval {ISIS_TAG} 3",
            f"isis ipv6 hello-interval {ISIS_TAG} 3",
            f"isis hold-multiplier {ISIS_TAG} 3",
            f"isis ipv6 hold-multiplier {ISIS_TAG} 3",
            "exit",
            "end",
        ],
    )


def _configure_pe(
    rt: TopologyRuntime,
    *,
    device: str,
    local_as: int,
    remote_as: int,
    rd: str,
    router_id: str,
    peer_ip: str,
    private_loop: str,
    private_loop_id: int,
    locator_name: str,
) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv6",
            f"route-distinguisher {rd}",
            "apply-label per-vrf",
            f"vpn-target {RT} export",
            f"vpn-target {RT} import",
            "exit",
            "exit",
            f"if loop {private_loop_id}",
            f"vrf forwarding {VRF_NAME}",
            f"ipv6 address {private_loop} {PRIVATE_LEN}",
            "exit",
            f"bgp {local_as}",
            f"router-id {router_id}",
            f"neighbor {peer_ip} as {remote_as}",
            f"vrf {VRF_NAME}",
            "af ipv6-unicast",
            "import-route connected",
            f"segment-routing srv6 locator {locator_name}",
            "exit",
            "exit",
            "af vpnv6",
            f"neighbor {peer_ip} enable",
            "exit",
            "end",
        ],
    )


def _set_bgp_private_af_leaf(
    rt: TopologyRuntime,
    *,
    device: str,
    local_as: int,
    leaf: str,
) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"bgp {local_as}",
            f"vrf {VRF_NAME}",
            "af ipv6-unicast",
            leaf,
            "end",
        ],
    )


def _set_bgp_vpn_neighbor_srv6_sid(
    rt: TopologyRuntime,
    *,
    device: str,
    local_as: int,
    peer_ip: str,
    enable: bool,
) -> None:
    leaf = f"neighbor {peer_ip} srv6-sid"
    if not enable:
        leaf = f"no {leaf}"
    run_cmds(
        rt=rt,
        device=device,
        commands=["config", f"bgp {local_as}", "af vpnv6", leaf, "end"],
    )


def _private_af_bdr_regex(local_as: int, leaf_command: str) -> str:
    """Match a leaf without crossing out of bgp/VRF/private-AF indentation."""
    return (
        rf"(?m)^bgp {local_as}\r?$\n"
        rf"(?:^ .*\r?$\n)*?^ vrf {re.escape(VRF_NAME)}\r?$\n"
        r"(?:^  .*\r?$\n)*?^  af ipv6-unicast\r?$\n"
        rf"(?:^   .*\r?$\n)*?^   {re.escape(leaf_command)}\r?$"
    )


def _vpn_neighbor_bdr_regex(local_as: int, peer_ip: str, leaf: str) -> str:
    """Match one peer leaf below the public VPNv6 AF."""
    return (
        rf"(?m)^bgp {local_as}\r?$\n"
        r"(?:^ +.*\r?$\n)*?^ af vpnv6\r?$\n"
        rf"(?:^  .*\r?$\n)*?^  neighbor {re.escape(peer_ip)} {re.escape(leaf)}\r?$"
    )


def _public_vpn_srv6_regex(locator_name: str) -> str:
    """Forbidden placement: SRv6 must not be owned by public VPNv6."""
    return (
        r"(?m)^ af vpnv6\r?$\n"
        rf"(?:^  .*\r?$\n)*?^  segment-routing srv6 locator {re.escape(locator_name)}\r?$"
    )


def _isis_srv6_bdr_regex(locator_name: str) -> str:
    """The locator advertisement must be nested below the ISIS IPv6 AF."""
    return (
        rf"(?m)^isis {ISIS_TAG}\r?$\n"
        rf"(?:^ .*\r?$\n)*?^ af ipv6\r?$\n"
        rf"(?:^  .*\r?$\n)*?^  segment-routing srv6 locator {re.escape(locator_name)}\r?$"
    )


def _isis_neighbor_check(device: str) -> dict[str, object]:
    return {
        "device": device,
        "command": f"show isis neighbor {ISIS_TAG}",
        "contains": ["ISIS Neighbors", GE_IF],
        "regex": [
            rf"(?im)^\s*{ISIS_TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\s+yes\s+yes\b"
        ],
        "label": f"{device} ISIS adjacency is Up before locator publication checks",
    }


def _set_isis_locator_advertisement(
    rt: TopologyRuntime,
    *,
    device: str,
    locator_name: str | None,
) -> None:
    leaf = f"segment-routing srv6 locator {locator_name}" if locator_name else "no segment-routing srv6"
    run_cmds(
        rt=rt,
        device=device,
        commands=["config", f"isis {ISIS_TAG}", "af ipv6", leaf, "end"],
    )


def _hold_remote_locators_absent(
    rt: TopologyRuntime,
    *,
    device: str,
    remote_prefixes: tuple[str, ...],
    duration: int,
    label: str,
) -> None:
    hold_check(
        rt,
        device=device,
        command="show route ipv6",
        duration=duration,
        interval=2,
        not_contains=[f"{prefix}/{LOCATOR_LEN}" for prefix in remote_prefixes],
        label=label,
    )
    hold_check(
        rt,
        device=device,
        command=f"show isis lsdb ipv6 {ISIS_TAG}",
        duration=duration,
        interval=2,
        not_contains=[f"{prefix}/{LOCATOR_LEN}" for prefix in remote_prefixes],
        label=f"{label} in the remote LSP",
    )


def _remote_ipv6_lsp_check(device: str) -> dict[str, object]:
    return {
        "device": device,
        "command": f"show isis lsdb ipv6 {ISIS_TAG}",
        "contains": ["ISIS LSDB", GE_IF],
        "not_contains": ["(no entries)"],
        "regex": [
            rf"(?im)^\s*Rx-If\s*:\s*{re.escape(GE_IF)}\s*$",
            r"(?im)^\s*TLV\[\d+\]\s*:\s*type=236\b",
        ],
        "label": f"{device} has received the peer IPv6 LSP",
    }


def _locator_lsdb_check(device: str, remote_prefix: str, unselected_prefix: str) -> dict[str, object]:
    return {
        "device": device,
        "command": f"show isis lsdb ipv6 {ISIS_TAG}",
        "contains": [f"{remote_prefix}/{LOCATOR_LEN}"],
        "not_contains": [f"{unselected_prefix}/{LOCATOR_LEN}"],
        "regex": [r"(?im)^\s*TLV\[\d+\]\s*:\s*type=236\b"],
        "label": f"{device} LSDB contains only the peer-selected locator",
    }


def _locator_route_checks(device: str, remote_prefix: str) -> list[dict[str, object]]:
    prefix = f"{remote_prefix}/{LOCATOR_LEN}"
    return [
        {
            "device": device,
            "command": f"show route ipv6 {remote_prefix} {LOCATOR_LEN}",
            "contains": [f"Routing entry for {prefix}"],
            "regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*isis\b"],
            "label": f"{device} learns remote locator {prefix} through ISIS",
        },
        {
            "device": device,
            "command": f"show fib ipv6 {remote_prefix} {LOCATOR_LEN}",
            "contains": [f"Routing entry for {prefix}"],
            "regex": [
                r"(?im)^\s*Path\s*\[\d+\]\s*:\s*isis\b",
                r"(?im)^\s*NH-Type\s*:\s*ip\s*$",
                r"(?im)^\s*Iter OIF\s*:\s*[1-9]\d*\s*$",
                r"(?im)^\s*Installed\s*:\s*yes\s*$",
                r"(?im)^\s*Skip OS\s*:\s*no\s*$",
            ],
            "label": f"{device} installs remote ISIS locator {prefix} in FIB",
        },
    ]


def _local_locator_route_check(device: str, local_prefix: str) -> dict[str, object]:
    """The locator source must be SRV6's ROUTE injection, never connected."""
    prefix = f"{local_prefix}/{LOCATOR_LEN}"
    return {
        "device": device,
        "command": f"show route ipv6 {local_prefix} {LOCATOR_LEN}",
        "contains": [f"Routing entry for {prefix}"],
        "regex": [
            r"(?im)^\s*Path\s*\[\d+\]\s*:\s*srv6\b",
            r"(?im)^\s*NH-Type\s*:\s*blackhole\s*$",
        ],
        "not_regex": [r"(?im)^\s*Path\s*\[\d+\]\s*:\s*connected\b"],
        "label": f"{device} receives local locator {prefix} from SRV6 into ROUTE",
    }


def _wait_service_sid(
    rt: TopologyRuntime,
    *,
    device: str,
    locator_name: str,
    locator_prefix: str,
) -> str:
    row_re = (
        rf"(?im)^(?P<sid>[0-9a-f:]+)\s+{re.escape(locator_name)}\s+\d+\s+"
        r"End\.DT6\s+\S+\s+[1-9]\d*\s+installed\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show srv6 sid",
        timeout=90,
        interval=2,
        regex=[row_re],
        label=f"{device} End.DT6 LocalSID is installed",
    )
    output = rt.exec_cmd(device, "show srv6 sid")
    match = re.search(row_re, output)
    if not match:
        raise RuntimeError(f"{device}: installed End.DT6 LocalSID disappeared after convergence")

    sid = match.group("sid")
    locator = ipaddress.ip_network(f"{locator_prefix}/{LOCATOR_LEN}")
    if ipaddress.ip_address(sid) not in locator:
        raise RuntimeError(f"{device}: service SID {sid} is outside locator {locator}")
    return sid


def _assert_kernel_seg6_enabled(rt: TopologyRuntime) -> None:
    """Read-only verification of the FIB-managed SRv6 receive sysctls."""
    relative_paths = (
        "all/seg6_enabled",
        "default/seg6_enabled",
        f"{GE_LINUX_IF}/seg6_enabled",
    )
    for device in ("r1", "r2"):
        container = rt.container_name(device)
        for relative in relative_paths:
            path = f"/proc/sys/net/ipv6/conf/{relative}"
            proc = subprocess.run(
                ["docker", "exec", container, "cat", path],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            value = (proc.stdout or "").strip()
            if proc.returncode != 0 or value != "1":
                raise RuntimeError(
                    f"{device}: FIB did not enable {path} (rc={proc.returncode}, "
                    f"value={value!r}, stderr={(proc.stderr or '').strip()!r})"
                )


def _vpn_sid_attribute_check(
    *,
    device: str,
    remote_rd: str,
    remote_prefix: str,
    remote_sid: str,
) -> dict[str, object]:
    sid_re = re.escape(remote_sid)
    prefix = f"{remote_prefix}/{PRIVATE_LEN}"
    return {
        "device": device,
        "command": f"show bgp route af vpnv6 rd {remote_rd} {remote_prefix} {PRIVATE_LEN}",
        "contains": ["BGP Route Detail", f"RD: {remote_rd}"],
        "regex": [
            r"(?im)^\s*RecvLabel\s*:\s*3\s*$",
            rf"(?im)^\s*NextHop\s*:\s*{sid_re}\s*$",
            r"(?im)^\s*Valid\s*:\s*Yes\s*$",
        ],
        "label": f"{device} receives VPNv6 {prefix} with the advertised service SID",
    }


def _vpn_and_tunnel_checks(
    *,
    device: str,
    remote_rd: str,
    remote_prefix: str,
    remote_transport_nh: str,
) -> list[dict[str, object]]:
    prefix = f"{remote_prefix}/{PRIVATE_LEN}"
    nh_re = re.escape(remote_transport_nh)
    return [
        {
            "device": device,
            "command": f"show bgp route af vpnv6 rd {remote_rd} {remote_prefix} {PRIVATE_LEN}",
            "contains": ["BGP Route Detail", f"RD: {remote_rd}"],
            "regex": [
                rf"(?im)^\s*RecvLabel\s*:\s*{MPLS_LABEL_RE}\s*$",
                rf"(?im)^\s*NextHop\s*:\s*{nh_re}\s*$",
                r"(?im)^\s*Valid\s*:\s*Yes\s*$",
            ],
            "not_regex": [r"(?im)^\s*RecvLabel\s*:\s*3\s*$"],
            "label": f"{device} receives VPNv6 {prefix} with a real MPLS VPN label and no SID nexthop",
        },
        {
            "device": device,
            "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME} {remote_prefix} {PRIVATE_LEN}",
            "contains": ["BGP Route Detail", "Imported", "REMOTE_CROSS"],
            "regex": [
                rf"(?im)^\s*NextHop\s*:\s*{nh_re}\s*$",
                r"(?im)^\s*Valid\s*:\s*Yes\s*$",
                r"(?im)^\s*IterState\s*:\s*Resolved\s*$",
                r"(?im)^\s*Tunnel-ID\s*:\s*[1-9]\d*\s*$",
            ],
            "label": f"{device} imports {prefix} through the MPLS VPN tunnel",
        },
        {
            "device": device,
            "command": f"show route ipv6 vrf {VRF_NAME} {remote_prefix} {PRIVATE_LEN}",
            "contains": [f"Routing entry for {prefix}"],
            "regex": [
                r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
                r"(?im)^\s*Tunnel-ID\s*:\s*[1-9]\d*\s*$",
                rf"(?im)^\s*Out-Label\s*:\s*{MPLS_LABEL_RE}\s*$",
            ],
            "not_regex": [r"(?im)^\s*NH-Type\s*:\s*srv6\s*$"],
            "label": f"{device} Route RIB uses tunnel plus a real VPN label for {prefix}",
        },
        {
            "device": device,
            "command": f"show fib ipv6 vrf {VRF_NAME} {remote_prefix} {PRIVATE_LEN}",
            "contains": [f"Routing entry for {prefix}"],
            "regex": [
                r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
                r"(?im)^\s*Tunnel-ID\s*:\s*[1-9]\d*\s*$",
                rf"(?im)^\s*Out-Label\s*:\s*{MPLS_LABEL_RE}\s*$",
                r"(?im)^\s*Installed\s*:\s*yes\s*$",
            ],
            "not_regex": [r"(?im)^\s*NH-Type\s*:\s*srv6\s*$"],
            "label": f"{device} FIB installs the MPLS VPN tunnel route for {prefix}",
        },
    ]


def _srv6_forwarding_absent_checks(
    *,
    device: str,
    remote_prefix: str,
    remote_sid: str,
) -> list[dict[str, object]]:
    prefix = f"{remote_prefix}/{PRIVATE_LEN}"
    sid_re = re.escape(remote_sid)
    return [
        {
            "device": device,
            "command": f"show route ipv6 vrf {VRF_NAME} {remote_prefix} {PRIVATE_LEN}",
            "not_regex": [
                r"(?im)^\s*NH-Type\s*:\s*srv6\s*$",
                rf"(?im)^\s*Nexthop\s*:\s*{sid_re}\s*$",
            ],
            "label": f"{device} Route RIB no longer retains SRv6 forwarding for {prefix}",
        },
        {
            "device": device,
            "command": f"show fib ipv6 vrf {VRF_NAME} {remote_prefix} {PRIVATE_LEN}",
            "not_contains": [f"Routing entry for {prefix}"],
            "not_regex": [r"(?im)^\s*NH-Type\s*:\s*srv6\s*$"],
            "label": f"{device} FIB fails closed after srv6 be is disabled for {prefix}",
        },
    ]


def _vpn_and_recursive_checks(
    *,
    device: str,
    remote_rd: str,
    remote_prefix: str,
    remote_sid: str,
) -> list[dict[str, object]]:
    sid_re = re.escape(remote_sid)
    prefix = f"{remote_prefix}/{PRIVATE_LEN}"
    return [
        _vpn_sid_attribute_check(
            device=device,
            remote_rd=remote_rd,
            remote_prefix=remote_prefix,
            remote_sid=remote_sid,
        ),
        {
            "device": device,
            "command": f"show bgp route af ipv6-unicast vrf {VRF_NAME} {remote_prefix} {PRIVATE_LEN}",
            "contains": ["BGP Route Detail", "Imported", "REMOTE_CROSS"],
            "regex": [
                rf"(?im)^\s*NextHop\s*:\s*{sid_re}\s*$",
                r"(?im)^\s*Valid\s*:\s*Yes\s*$",
                r"(?im)^\s*IterState\s*:\s*Resolved\s*$",
                r"(?im)^\s*Out-If\s*:\s*(?!-)\S+\([1-9]\d*\)\s*$",
                r"(?im)^\s*BorrowRef\s*:\s*1\s*$",
            ],
            "label": f"{device} imports {prefix} and resolves its service SID",
        },
        {
            "device": device,
            "command": f"show route ipv6 vrf {VRF_NAME} {remote_prefix} {PRIVATE_LEN}",
            "contains": [f"Routing entry for {prefix}"],
            "regex": [
                rf"(?im)^\s*Nexthop\s*:\s*{sid_re}\s*$",
                r"(?im)^\s*NH-Type\s*:\s*srv6\s*$",
                r"(?im)^\s*Iter NH\s*:\s*(?!-)\S+\s*$",
                rf"(?im)^\s*Iter OIF\s*:\s*{re.escape(GE_IF)}\s*$",
            ],
            "label": f"{device} Route RIB recursively resolves complete service SID for {prefix}",
        },
        {
            "device": device,
            "command": f"show fib ipv6 vrf {VRF_NAME} {remote_prefix} {PRIVATE_LEN}",
            "contains": [f"Routing entry for {prefix}"],
            "regex": [
                rf"(?im)^\s*Nexthop\s*:\s*srv6:{sid_re}\s*$",
                r"(?im)^\s*NH-Type\s*:\s*srv6\s*$",
                r"(?im)^\s*Iter NH\s*:\s*(?!-)\S+\s*$",
                r"(?im)^\s*Iter OIF\s*:\s*[1-9]\d*\s*$",
                r"(?im)^\s*Installed\s*:\s*yes\s*$",
                r"(?im)^\s*Skip OS\s*:\s*no\s*$",
            ],
            "label": f"{device} installs SRv6 recursive FIB entry for {prefix}",
        },
    ]


def _ping_check(device: str, source: str, destination: str) -> dict[str, object]:
    return {
        "device": device,
        "command": f"ping ipv6 {destination} -a {source} vrf {VRF_NAME}",
        "contains": ["bytes from", "0% packet loss"],
        "not_regex": [PING_FAIL_RE],
        "normalize_whitespace": False,
        "label": f"{device} {source} -> {destination} over the active L3VPN data plane",
    }


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_router_id = str(g_top.r1.GE_1.ip)
    r2_router_id = str(g_top.r2.GE_1.ip)
    # VPNv6 MP_REACH carries a VPN-IPv6 nexthop, so use the directly
    # connected IPv6 transport addresses for the default MPLS path.  In
    # explicit SRv6 mode the exported service SID replaces this transport NH.
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip6)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip6)

    try:
        _cleanup(rt)

        step("Configure selected and unselected SRv6 locators; leave ISIS publication disabled")
        _configure_underlay(
            rt,
            device="r1",
            net=R1_NET,
            locator_name=R1_LOCATOR_NAME,
            locator_prefix=R1_LOCATOR_PREFIX,
            dummy_locator_name=R1_DUMMY_LOCATOR_NAME,
            dummy_locator_prefix=R1_DUMMY_LOCATOR_PREFIX,
        )
        _configure_underlay(
            rt,
            device="r2",
            net=R2_NET,
            locator_name=R2_LOCATOR_NAME,
            locator_prefix=R2_LOCATOR_PREFIX,
            dummy_locator_name=R2_DUMMY_LOCATOR_NAME,
            dummy_locator_prefix=R2_DUMMY_LOCATOR_PREFIX,
        )

        step("Wait for ISIS adjacency and verify all local locators stay owned by SRV6")
        wait_checks(
            rt,
            [
                _isis_neighbor_check("r1"),
                _isis_neighbor_check("r2"),
                _remote_ipv6_lsp_check("r1"),
                _remote_ipv6_lsp_check("r2"),
                _local_locator_route_check("r1", R1_LOCATOR_PREFIX),
                _local_locator_route_check("r1", R1_DUMMY_LOCATOR_PREFIX),
                _local_locator_route_check("r2", R2_LOCATOR_PREFIX),
                _local_locator_route_check("r2", R2_DUMMY_LOCATOR_PREFIX),
            ],
            timeout=90,
            interval=2,
        )

        step("Verify ISIS does not publish any locator by default")
        _hold_remote_locators_absent(
            rt,
            device="r1",
            remote_prefixes=(R2_LOCATOR_PREFIX, R2_DUMMY_LOCATOR_PREFIX),
            duration=8,
            label="r1 learns no r2 locator before explicit ISIS IPv6-AF configuration",
        )
        _hold_remote_locators_absent(
            rt,
            device="r2",
            remote_prefixes=(R1_LOCATOR_PREFIX, R1_DUMMY_LOCATOR_PREFIX),
            duration=8,
            label="r2 learns no r1 locator before explicit ISIS IPv6-AF configuration",
        )

        step("Explicitly select one locator per PE under the ISIS IPv6 address-family")
        _set_isis_locator_advertisement(rt, device="r1", locator_name=R1_LOCATOR_NAME)
        _set_isis_locator_advertisement(rt, device="r2", locator_name=R2_LOCATOR_NAME)
        wait_checks(
            rt,
            [
                *_locator_route_checks("r1", R2_LOCATOR_PREFIX),
                *_locator_route_checks("r2", R1_LOCATOR_PREFIX),
                _locator_lsdb_check("r1", R2_LOCATOR_PREFIX, R2_DUMMY_LOCATOR_PREFIX),
                _locator_lsdb_check("r2", R1_LOCATOR_PREFIX, R1_DUMMY_LOCATOR_PREFIX),
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "regex": [_isis_srv6_bdr_regex(R1_LOCATOR_NAME)],
                    "not_regex": [_isis_srv6_bdr_regex(R1_DUMMY_LOCATOR_NAME)],
                    "label": "r1 BDR nests only the selected locator under ISIS IPv6 AF",
                },
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "regex": [_isis_srv6_bdr_regex(R2_LOCATOR_NAME)],
                    "not_regex": [_isis_srv6_bdr_regex(R2_DUMMY_LOCATOR_NAME)],
                    "label": "r2 BDR nests only the selected locator under ISIS IPv6 AF",
                },
            ],
            timeout=90,
            interval=2,
        )
        _hold_remote_locators_absent(
            rt,
            device="r1",
            remote_prefixes=(R2_DUMMY_LOCATOR_PREFIX,),
            duration=6,
            label="r1 never learns r2's unselected locator",
        )
        _hold_remote_locators_absent(
            rt,
            device="r2",
            remote_prefixes=(R1_DUMMY_LOCATOR_PREFIX,),
            duration=6,
            label="r2 never learns r1's unselected locator",
        )

        step("Withdraw and restore r1 locator advertisement without deleting its local locator")
        _set_isis_locator_advertisement(rt, device="r1", locator_name=None)
        wait_check(
            rt,
            device="r2",
            command=f"show route ipv6 {R1_LOCATOR_PREFIX} {LOCATOR_LEN}",
            timeout=60,
            interval=2,
            contains=["(no matching routes)"],
            label="r2 withdraws r1 locator after no segment-routing srv6",
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": f"show isis lsdb ipv6 {ISIS_TAG}",
                    "not_contains": [f"{R1_LOCATOR_PREFIX}/{LOCATOR_LEN}"],
                    "label": "r2 LSDB withdraws r1 locator from TLV236",
                },
                {
                    "device": "r2",
                    "command": f"show fib ipv6 {R1_LOCATOR_PREFIX} {LOCATOR_LEN}",
                    "contains": ["(no routes)"],
                    "label": "r2 FIB withdraws r1 locator",
                },
                {
                    "device": "r2",
                    "command": "show fib os ipv6",
                    "not_contains": [f"{R1_LOCATOR_PREFIX}/{LOCATOR_LEN}"],
                    "label": "r2 Linux FIB withdraws r1 locator",
                },
                _local_locator_route_check("r1", R1_LOCATOR_PREFIX),
                *_locator_route_checks("r1", R2_LOCATOR_PREFIX),
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "not_regex": [_isis_srv6_bdr_regex(R1_LOCATOR_NAME)],
                    "label": "r1 BDR removes the ISIS locator leaf after undo",
                },
            ],
            timeout=30,
            interval=2,
        )
        _set_isis_locator_advertisement(rt, device="r1", locator_name=R1_LOCATOR_NAME)
        wait_checks(
            rt,
            [
                *_locator_route_checks("r2", R1_LOCATOR_PREFIX),
                _locator_lsdb_check("r2", R1_LOCATOR_PREFIX, R1_DUMMY_LOCATOR_PREFIX),
            ],
            timeout=90,
            interval=2,
        )

        step("Configure locator-only private IPv6 AFs; leave neighbor srv6-sid and srv6 be at defaults")
        _configure_pe(
            rt,
            device="r1",
            local_as=R1_AS,
            remote_as=R2_AS,
            rd=R1_RD,
            router_id=r1_router_id,
            peer_ip=r1_peer_ip,
            private_loop=R1_PRIVATE_LOOP,
            private_loop_id=R1_PRIVATE_LOOP_ID,
            locator_name=R1_LOCATOR_NAME,
        )
        _configure_pe(
            rt,
            device="r2",
            local_as=R2_AS,
            remote_as=R1_AS,
            rd=R2_RD,
            router_id=r2_router_id,
            peer_ip=r2_peer_ip,
            private_loop=R2_PRIVATE_LOOP,
            private_loop_id=R2_PRIVATE_LOOP_ID,
            locator_name=R2_LOCATOR_NAME,
        )

        step("Assert SID advertisement is absent from VPNv6 peers and BE is absent from private AFs")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "regex": [
                        _private_af_bdr_regex(R1_AS, "import-route connected"),
                        _private_af_bdr_regex(
                            R1_AS, f"segment-routing srv6 locator {R1_LOCATOR_NAME}"
                        ),
                        _vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "enable"),
                    ],
                    "not_regex": [
                        _private_af_bdr_regex(R1_AS, "srv6 be"),
                        _vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "srv6-sid"),
                        _public_vpn_srv6_regex(R1_LOCATOR_NAME),
                    ],
                    "not_contains": ["srv6 be"],
                    "label": "r1 BDR keeps peer SID advertisement and private BE default-off",
                },
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "regex": [
                        _private_af_bdr_regex(R2_AS, "import-route connected"),
                        _private_af_bdr_regex(
                            R2_AS, f"segment-routing srv6 locator {R2_LOCATOR_NAME}"
                        ),
                        _vpn_neighbor_bdr_regex(R2_AS, r2_peer_ip, "enable"),
                    ],
                    "not_regex": [
                        _private_af_bdr_regex(R2_AS, "srv6 be"),
                        _vpn_neighbor_bdr_regex(R2_AS, r2_peer_ip, "srv6-sid"),
                        _public_vpn_srv6_regex(R2_LOCATOR_NAME),
                    ],
                    "not_contains": ["srv6 be"],
                    "label": "r2 BDR keeps peer SID advertisement and private BE default-off",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Wait for VPNv6 eBGP establishment")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af vpnv6",
                    "regex": [
                        rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"
                    ],
                    "label": "r1 to r2 VPNv6 established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af vpnv6",
                    "regex": [
                        rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"
                    ],
                    "label": "r2 to r1 VPNv6 established",
                },
            ],
            timeout=90,
            interval=2,
        )

        step("Default gates: verify real VPN labels, tunnel FIBs, no SID attribute, and MPLS forwarding")
        wait_checks(
            rt,
            [
                *_vpn_and_tunnel_checks(
                    device="r1",
                    remote_rd=R2_RD,
                    remote_prefix=R2_PRIVATE_LOOP,
                    remote_transport_nh=r1_peer_ip,
                ),
                *_vpn_and_tunnel_checks(
                    device="r2",
                    remote_rd=R1_RD,
                    remote_prefix=R1_PRIVATE_LOOP,
                    remote_transport_nh=r2_peer_ip,
                ),
            ],
            timeout=90,
            interval=2,
        )
        wait_checks(
            rt,
            [
                _ping_check("r1", R1_PRIVATE_LOOP, R2_PRIVATE_LOOP),
                _ping_check("r2", R2_PRIVATE_LOOP, R1_PRIVATE_LOOP),
            ],
            timeout=60,
            interval=3,
        )

        step("Verify locator selection pre-allocates both End.DT6 LocalSIDs")
        r1_sid = _wait_service_sid(
            rt,
            device="r1",
            locator_name=R1_LOCATOR_NAME,
            locator_prefix=R1_LOCATOR_PREFIX,
        )
        r2_sid = _wait_service_sid(
            rt,
            device="r2",
            locator_name=R2_LOCATOR_NAME,
            locator_prefix=R2_LOCATOR_PREFIX,
        )

        step("Enable SID advertisement only from r1 to its VPNv6 peer")
        _set_bgp_vpn_neighbor_srv6_sid(
            rt,
            device="r1",
            local_as=R1_AS,
            peer_ip=r1_peer_ip,
            enable=True,
        )
        wait_checks(
            rt,
            [
                _vpn_sid_attribute_check(
                    device="r2",
                    remote_rd=R1_RD,
                    remote_prefix=R1_PRIVATE_LOOP,
                    remote_sid=r1_sid,
                ),
                *_srv6_forwarding_absent_checks(
                    device="r2", remote_prefix=R1_PRIVATE_LOOP, remote_sid=r1_sid
                ),
                *_vpn_and_tunnel_checks(
                    device="r1",
                    remote_rd=R2_RD,
                    remote_prefix=R2_PRIVATE_LOOP,
                    remote_transport_nh=r1_peer_ip,
                ),
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "regex": [_vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "srv6-sid")],
                    "not_regex": [_private_af_bdr_regex(R1_AS, "srv6 be")],
                    "label": "r1 BDR enables SID advertisement only on its VPNv6 peer",
                },
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "regex": [_vpn_neighbor_bdr_regex(R2_AS, r2_peer_ip, "enable")],
                    "not_regex": [
                        _vpn_neighbor_bdr_regex(R2_AS, r2_peer_ip, "srv6-sid"),
                        _private_af_bdr_regex(R2_AS, "srv6 be"),
                    ],
                    "label": "r2 keeps SID advertisement and BE disabled",
                },
            ],
            timeout=90,
            interval=2,
        )

        step("Enable r2 peer SID advertisement and private IPv6 BE recursion on both PEs")
        _set_bgp_vpn_neighbor_srv6_sid(
            rt,
            device="r2",
            local_as=R2_AS,
            peer_ip=r2_peer_ip,
            enable=True,
        )
        for device, local_as in (("r1", R1_AS), ("r2", R2_AS)):
            _set_bgp_private_af_leaf(rt, device=device, local_as=local_as, leaf="srv6 be")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "regex": [
                        _vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "srv6-sid"),
                        _private_af_bdr_regex(R1_AS, "srv6 be"),
                    ],
                    "label": "r1 BDR keeps peer SID advertisement separate from private BE",
                },
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "regex": [
                        _vpn_neighbor_bdr_regex(R2_AS, r2_peer_ip, "srv6-sid"),
                        _private_af_bdr_regex(R2_AS, "srv6 be"),
                    ],
                    "label": "r2 BDR keeps peer SID advertisement separate from private BE",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Verify FIB enabled SRv6 receive processing on existing Linux interfaces")
        _assert_kernel_seg6_enabled(rt)

        step("Verify VPNv6 propagation and recursive private FIB service-SID nexthops")
        wait_checks(
            rt,
            [
                *_vpn_and_recursive_checks(
                    device="r1",
                    remote_rd=R2_RD,
                    remote_prefix=R2_PRIVATE_LOOP,
                    remote_sid=r2_sid,
                ),
                *_vpn_and_recursive_checks(
                    device="r2",
                    remote_rd=R1_RD,
                    remote_prefix=R1_PRIVATE_LOOP,
                    remote_sid=r1_sid,
                ),
            ],
            timeout=90,
            interval=2,
        )

        step("Verify bidirectional VRF IPv6 traffic through remote End.DT6")
        wait_checks(
            rt,
            [
                _ping_check("r1", R1_PRIVATE_LOOP, R2_PRIVATE_LOOP),
                _ping_check("r2", R2_PRIVATE_LOOP, R1_PRIVATE_LOOP),
            ],
            timeout=60,
            interval=3,
        )

        step("Reboot r1 BGP with peer SID advertisement and private BE; verify End.DT6 recovers")
        process_reboot(rt, "r1", "bgp", ready_timeout=90)
        r1_sid = _wait_service_sid(
            rt,
            device="r1",
            locator_name=R1_LOCATOR_NAME,
            locator_prefix=R1_LOCATOR_PREFIX,
        )
        r2_sid = _wait_service_sid(
            rt,
            device="r2",
            locator_name=R2_LOCATOR_NAME,
            locator_prefix=R2_LOCATOR_PREFIX,
        )
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "regex": [
                        _vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "srv6-sid"),
                        _private_af_bdr_regex(R1_AS, "srv6 be"),
                    ],
                    "not_regex": [_public_vpn_srv6_regex(R1_LOCATOR_NAME)],
                    "label": "r1 BDR restores peer SID advertisement and private BE after reboot",
                },
                {
                    "device": "r2",
                    "command": "show current-configuration",
                    "regex": [
                        _vpn_neighbor_bdr_regex(R2_AS, r2_peer_ip, "srv6-sid"),
                        _private_af_bdr_regex(R2_AS, "srv6 be"),
                    ],
                    "not_regex": [_public_vpn_srv6_regex(R2_LOCATOR_NAME)],
                    "label": "r2 BDR retains peer SID advertisement and private BE during reboot",
                },
                {
                    "device": "r1",
                    "command": "show bgp neighbor af vpnv6",
                    "regex": [
                        rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"
                    ],
                    "label": "r1 VPNv6 session recovers after BGP reboot",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af vpnv6",
                    "regex": [
                        rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"
                    ],
                    "label": "r2 VPNv6 session recovers after r1 BGP reboot",
                },
                *_vpn_and_recursive_checks(
                    device="r1",
                    remote_rd=R2_RD,
                    remote_prefix=R2_PRIVATE_LOOP,
                    remote_sid=r2_sid,
                ),
                *_vpn_and_recursive_checks(
                    device="r2",
                    remote_rd=R1_RD,
                    remote_prefix=R1_PRIVATE_LOOP,
                    remote_sid=r1_sid,
                ),
            ],
            timeout=120,
            interval=2,
        )
        wait_checks(
            rt,
            [
                _ping_check("r1", R1_PRIVATE_LOOP, R2_PRIVATE_LOOP),
                _ping_check("r2", R2_PRIVATE_LOOP, R1_PRIVATE_LOOP),
            ],
            timeout=60,
            interval=3,
        )

        step("Disable srv6 be on r1; keep the received SID attribute but remove SRv6 forwarding")
        _set_bgp_private_af_leaf(rt, device="r1", local_as=R1_AS, leaf="no srv6 be")
        wait_checks(
            rt,
            [
                _vpn_sid_attribute_check(
                    device="r1",
                    remote_rd=R2_RD,
                    remote_prefix=R2_PRIVATE_LOOP,
                    remote_sid=r2_sid,
                ),
                *_srv6_forwarding_absent_checks(
                    device="r1", remote_prefix=R2_PRIVATE_LOOP, remote_sid=r2_sid
                ),
                *_vpn_and_recursive_checks(
                    device="r2",
                    remote_rd=R1_RD,
                    remote_prefix=R1_PRIVATE_LOOP,
                    remote_sid=r1_sid,
                ),
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "regex": [_vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "srv6-sid")],
                    "not_regex": [_private_af_bdr_regex(R1_AS, "srv6 be")],
                    "not_contains": ["srv6 be"],
                    "label": "r1 BDR removes only srv6 be",
                },
            ],
            timeout=60,
            interval=2,
        )

        step("Restore srv6 be on r1 and verify SID recursion and bidirectional traffic recover")
        _set_bgp_private_af_leaf(rt, device="r1", local_as=R1_AS, leaf="srv6 be")
        wait_checks(
            rt,
            _vpn_and_recursive_checks(
                device="r1",
                remote_rd=R2_RD,
                remote_prefix=R2_PRIVATE_LOOP,
                remote_sid=r2_sid,
            ),
            timeout=90,
            interval=2,
        )
        wait_checks(
            rt,
            [
                _ping_check("r1", R1_PRIVATE_LOOP, R2_PRIVATE_LOOP),
                _ping_check("r2", R2_PRIVATE_LOOP, R1_PRIVATE_LOOP),
            ],
            timeout=60,
            interval=3,
        )

        step("Disable r1 VPNv6 peer SID advertisement; r2 must relearn a real label and tunnel")
        _set_bgp_vpn_neighbor_srv6_sid(
            rt,
            device="r1",
            local_as=R1_AS,
            peer_ip=r1_peer_ip,
            enable=False,
        )
        wait_checks(
            rt,
            [
                *_vpn_and_tunnel_checks(
                    device="r2",
                    remote_rd=R1_RD,
                    remote_prefix=R1_PRIVATE_LOOP,
                    remote_transport_nh=r2_peer_ip,
                ),
                *_vpn_and_recursive_checks(
                    device="r1",
                    remote_rd=R2_RD,
                    remote_prefix=R2_PRIVATE_LOOP,
                    remote_sid=r2_sid,
                ),
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "regex": [
                        _private_af_bdr_regex(R1_AS, "srv6 be"),
                        _vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "enable"),
                    ],
                    "not_regex": [
                        _vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "srv6-sid")
                    ],
                    "label": "r1 BDR removes only the VPNv6 peer SID leaf",
                },
            ],
            timeout=90,
            interval=2,
        )
        wait_checks(
            rt,
            [
                _ping_check("r1", R1_PRIVATE_LOOP, R2_PRIVATE_LOOP),
                _ping_check("r2", R2_PRIVATE_LOOP, R1_PRIVATE_LOOP),
            ],
            timeout=60,
            interval=3,
        )

        step("Restore r1 VPNv6 peer SID advertisement and verify SID recursion and traffic again")
        _set_bgp_vpn_neighbor_srv6_sid(
            rt,
            device="r1",
            local_as=R1_AS,
            peer_ip=r1_peer_ip,
            enable=True,
        )
        r1_sid = _wait_service_sid(
            rt,
            device="r1",
            locator_name=R1_LOCATOR_NAME,
            locator_prefix=R1_LOCATOR_PREFIX,
        )
        wait_checks(
            rt,
            [
                *_vpn_and_recursive_checks(
                    device="r1",
                    remote_rd=R2_RD,
                    remote_prefix=R2_PRIVATE_LOOP,
                    remote_sid=r2_sid,
                ),
                *_vpn_and_recursive_checks(
                    device="r2",
                    remote_rd=R1_RD,
                    remote_prefix=R1_PRIVATE_LOOP,
                    remote_sid=r1_sid,
                ),
                {
                    "device": "r1",
                    "command": "show current-configuration",
                    "regex": [
                        _vpn_neighbor_bdr_regex(R1_AS, r1_peer_ip, "srv6-sid"),
                        _private_af_bdr_regex(R1_AS, "srv6 be"),
                    ],
                    "label": "r1 BDR restores VPNv6 peer SID advertisement and private BE",
                },
            ],
            timeout=90,
            interval=2,
        )
        wait_checks(
            rt,
            [
                _ping_check("r1", R1_PRIVATE_LOOP, R2_PRIVATE_LOOP),
                _ping_check("r2", R2_PRIVATE_LOOP, R1_PRIVATE_LOOP),
            ],
            timeout=60,
            interval=3,
        )

        print("SRv6 L3VPN End.DT6 peer SID advertisement and BE lifecycle check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
