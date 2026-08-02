#!/usr/bin/env python3
"""Per-peer SRv6 SID advertisement and VPN update-group partition check.

The two physical links create two otherwise-identical peers for each VPN
address family.  Consequently the peers must share one update-group whenever
their ``srv6-sid`` setting is equal, and must split only while the setting
differs.

The receiver's per-peer Adj-RIB-In is also checked so the grouping assertion
is tied to the actual wire encoding: SID peers receive label 3 plus the
End.DT4/End.DT6 Service SID nexthop, while default peers receive a real VPN
label and their transport nexthop.
"""

from __future__ import annotations

import ipaddress
import re
import time

from module_api import (  # noqa: E402
    cmd,
    g_top,
    process_reboot,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


A_AS = 65001
B_AS = 65002
VRF_NAME = "red"
VRF_LOOP_ID = 75
PRIVATE_ADDR = "100.162.1.1"
PRIVATE_LEN = 32
PRIVATE_ADDR_V6 = "2001:db8:162:75::1"
PRIVATE_LEN_V6 = 128
RD = "65001:162"
RT = "65000:162"

LOCATOR_NAME = "loc-a-ug"
LOCATOR_PREFIX = "2001:db8:162:1::"
LOCATOR_LEN = 64
LOCATOR_FUNCTION_BITS = 16

MPLS_LABEL_RE = r"(?:1[6-9]|[2-9]\d|[1-9]\d{2,})"
SUMMARY_ROW_RE = re.compile(
    r"(?im)^\s*(\d+)\s+(?:iBGP|eBGP|Unknown)\b.*$"
)
UG_SRV6_RE = re.compile(r"(?im)^\s*SRv6-SID\s*:\s*(Yes|No)\s*$")


class VpnFamilyCase:
    __slots__ = (
        "vpn_af",
        "vrf_af",
        "bgp_vrf_af",
        "address_command",
        "private_addr",
        "private_len",
        "endpoint_behavior",
    )

    def __init__(
        self,
        *,
        vpn_af: str,
        vrf_af: str,
        bgp_vrf_af: str,
        address_command: str,
        private_addr: str,
        private_len: int,
        endpoint_behavior: str,
    ) -> None:
        self.vpn_af = vpn_af
        self.vrf_af = vrf_af
        self.bgp_vrf_af = bgp_vrf_af
        self.address_command = address_command
        self.private_addr = private_addr
        self.private_len = private_len
        self.endpoint_behavior = endpoint_behavior


VPNV4 = VpnFamilyCase(
    vpn_af="vpnv4",
    vrf_af="ipv4",
    bgp_vrf_af="ipv4-unicast",
    address_command="ip address",
    private_addr=PRIVATE_ADDR,
    private_len=PRIVATE_LEN,
    endpoint_behavior="End.DT4",
)
VPNV6 = VpnFamilyCase(
    vpn_af="vpnv6",
    vrf_af="ipv6",
    bgp_vrf_af="ipv6-unicast",
    address_command="ipv6 address",
    private_addr=PRIVATE_ADDR_V6,
    private_len=PRIVATE_LEN_V6,
    endpoint_behavior="End.DT6",
)


def _remove_bgp_instance(
    rt: TopologyRuntime,
    *,
    device: str,
    local_as: int,
    timeout: float = 30.0,
    interval: float = 0.5,
) -> None:
    instance_re = re.compile(rf"(?m)^bgp {local_as}\r?$")
    valid_config_re = re.compile(r"(?m)^sysname\s+\S+\r?$")
    deadline = time.monotonic() + timeout
    attempts = 0
    last_output = ""

    while time.monotonic() < deadline:
        current = cmd(rt, device, "show current-configuration", strict=False)
        if not valid_config_re.search(current):
            raise RuntimeError(
                f"{device}: invalid current-configuration while removing bgp {local_as}: "
                f"{current!r}"
            )
        if not instance_re.search(current):
            return

        attempts += 1
        outputs = run_cmds(
            rt=rt,
            device=device,
            strict=False,
            commands=["end", "config", "no bgp", "end"],
        )
        last_output = outputs[2] if len(outputs) > 2 else ""
        if "Error:" in last_output and "Pending work did not converge" not in last_output:
            raise RuntimeError(
                f"{device}: no bgp returned a non-retryable error: {last_output.strip()}"
            )
        time.sleep(interval)

    raise RuntimeError(
        f"{device}: bgp {local_as} still exists after {attempts} removal attempts; "
        f"last output={last_output.strip()!r}"
    )


def _cleanup(rt: TopologyRuntime, *, locator_nh: str) -> None:
    step("Cleanup per-peer SRv6 SID update-group configuration")
    _remove_bgp_instance(rt, device="a", local_as=A_AS)
    _remove_bgp_instance(rt, device="b", local_as=B_AS)

    run_cmds(
        rt=rt,
        device="b",
        strict=False,
        commands=[
            "config",
            f"no route static ipv6 {LOCATOR_PREFIX} {LOCATOR_LEN} {locator_nh}",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="a",
        strict=False,
        commands=[
            "config",
            f"no if loop {VRF_LOOP_ID}",
            f"no vrf {VRF_NAME}",
            "srv6",
            f"no locator {LOCATOR_NAME}",
            "exit",
            "no srv6",
            "end",
        ],
    )


def _configure(
    rt: TopologyRuntime,
    *,
    a_ge1_peer: str,
    a_ge2_peer: str,
    b_ge1_peer: str,
    b_ge2_peer: str,
    locator_nh: str,
    case: VpnFamilyCase = VPNV4,
) -> None:
    run_cmds(
        rt=rt,
        device="a",
        commands=[
            "config",
            "srv6",
            f"locator {LOCATOR_NAME} prefix {LOCATOR_PREFIX} {LOCATOR_LEN} "
            f"function-bits {LOCATOR_FUNCTION_BITS}",
            "exit",
            f"vrf {VRF_NAME}",
            f"af {case.vrf_af}",
            f"route-distinguisher {RD}",
            "apply-label per-vrf",
            f"vpn-target {RT} export",
            f"vpn-target {RT} import",
            "exit",
            "exit",
            f"if loop {VRF_LOOP_ID}",
            f"vrf forwarding {VRF_NAME}",
            f"{case.address_command} {case.private_addr} {case.private_len}",
            "exit",
            f"bgp {A_AS}",
            "router-id 1.1.1.1",
            f"neighbor {a_ge1_peer} as {B_AS}",
            f"neighbor {a_ge2_peer} as {B_AS}",
            f"vrf {VRF_NAME}",
            f"af {case.bgp_vrf_af}",
            "import-route connected",
            f"segment-routing srv6 locator {LOCATOR_NAME}",
            "exit",
            "exit",
            f"af {case.vpn_af}",
            f"neighbor {a_ge1_peer} enable",
            f"neighbor {a_ge2_peer} enable",
            "exit",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="b",
        commands=[
            "config",
            f"route static ipv6 {LOCATOR_PREFIX} {LOCATOR_LEN} {locator_nh}",
            f"bgp {B_AS}",
            "router-id 2.2.2.2",
            f"neighbor {b_ge1_peer} as {A_AS}",
            f"neighbor {b_ge2_peer} as {A_AS}",
            f"af {case.vpn_af}",
            # b is a wire-observation node without a private VRF/import RT.
            # Disable the default RT admission filter so its Adj-RIB-In keeps
            # both peers' VPN UPDATEs for label/SID assertions.
            "no policy vpn-target",
            f"neighbor {b_ge1_peer} enable",
            f"neighbor {b_ge2_peer} enable",
            "exit",
            "end",
        ],
    )


def _set_peer_srv6_sid(
    rt: TopologyRuntime,
    *,
    peer: str,
    enabled: bool,
    case: VpnFamilyCase = VPNV4,
) -> None:
    run_cmds(
        rt=rt,
        device="a",
        commands=[
            "config",
            f"bgp {A_AS}",
            f"af {case.vpn_af}",
            f"neighbor {peer} srv6-sid" if enabled else f"no neighbor {peer} srv6-sid",
            "end",
        ],
    )


def _vpn_af_bdr_regex(leaf: str, *, case: VpnFamilyCase = VPNV4) -> str:
    return (
        rf"(?m)^bgp {A_AS}\r?$\n"
        rf"(?:^ .*\r?$\n)*?^ af {case.vpn_af}\r?$\n"
        rf"(?:^  .*\r?$\n)*?^  {re.escape(leaf)}\r?$"
    )


def _bdr_check(
    *,
    a_ge1_peer: str,
    a_ge2_peer: str,
    ge1_sid: bool,
    ge2_sid: bool,
    label: str,
    case: VpnFamilyCase = VPNV4,
) -> dict[str, object]:
    required = [
        _vpn_af_bdr_regex(f"neighbor {a_ge1_peer} enable", case=case),
        _vpn_af_bdr_regex(f"neighbor {a_ge2_peer} enable", case=case),
    ]
    forbidden: list[str] = []
    for peer, enabled in ((a_ge1_peer, ge1_sid), (a_ge2_peer, ge2_sid)):
        pattern = _vpn_af_bdr_regex(f"neighbor {peer} srv6-sid", case=case)
        (required if enabled else forbidden).append(pattern)
    return {
        "device": "a",
        "command": "show current-configuration",
        "regex": required,
        "not_regex": forbidden,
        "label": label,
    }


def _established_check(
    device: str,
    peer: str,
    *,
    case: VpnFamilyCase = VPNV4,
) -> dict[str, object]:
    return {
        "device": device,
        "command": f"show bgp neighbor af {case.vpn_af}",
        "regex": [rf"(?im)^\s*{re.escape(peer)}\s+\S+\s+\S+\s+Established\s*$"],
        "label": f"{device} {case.vpn_af.upper()} peer {peer} is Established",
    }


def _wait_service_sid(
    rt: TopologyRuntime,
    *,
    case: VpnFamilyCase = VPNV4,
) -> str:
    row_re = (
        rf"(?im)^(?P<sid>[0-9a-f:]+)\s+{re.escape(LOCATOR_NAME)}\s+\d+\s+"
        rf"{re.escape(case.endpoint_behavior)}\s+\S+\s+[1-9]\d*\s+installed\s*$"
    )
    wait_check(
        rt,
        device="a",
        command="show srv6 sid",
        timeout=90,
        interval=2,
        regex=[row_re],
        label=f"a preallocates and installs the {case.endpoint_behavior} LocalSID",
    )
    output = cmd(rt, "a", "show srv6 sid", strict=False)
    match = re.search(row_re, output)
    if not match:
        raise RuntimeError(
            f"a {case.endpoint_behavior} LocalSID disappeared after convergence:\n{output}"
        )
    sid = match.group("sid")
    if ipaddress.ip_address(sid) not in ipaddress.ip_network(
        f"{LOCATOR_PREFIX}/{LOCATOR_LEN}"
    ):
        raise RuntimeError(f"service SID {sid} is outside {LOCATOR_PREFIX}/{LOCATOR_LEN}")
    return sid


def _collect_update_groups(
    rt: TopologyRuntime,
    *,
    case: VpnFamilyCase = VPNV4,
) -> tuple[str, dict[int, str]]:
    summary = cmd(
        rt,
        "a",
        f"show bgp update-group af {case.vpn_af}",
        strict=False,
    )
    details: dict[int, str] = {}
    for match in SUMMARY_ROW_RE.finditer(summary):
        group_id = int(match.group(1))
        details[group_id] = cmd(
            rt,
            "a",
            f"show bgp update-group af {case.vpn_af} {group_id}",
            strict=False,
        )
    return summary, details


def _wait_update_group_partition(
    rt: TopologyRuntime,
    *,
    expected: dict[str, bool],
    timeout: float = 60.0,
    interval: float = 1.0,
    case: VpnFamilyCase = VPNV4,
) -> None:
    expected_groups = len(set(expected.values()))
    deadline = time.monotonic() + timeout
    last_summary = ""
    last_details: dict[int, str] = {}

    while time.monotonic() < deadline:
        summary, details = _collect_update_groups(rt, case=case)
        last_summary = summary
        last_details = details
        if len(details) != expected_groups:
            time.sleep(interval)
            continue

        peer_groups: dict[str, int] = {}
        group_states: dict[int, bool] = {}
        valid = True
        for group_id, detail in details.items():
            state_match = UG_SRV6_RE.search(detail)
            if not state_match:
                valid = False
                break
            group_states[group_id] = state_match.group(1).lower() == "yes"
            for peer in expected:
                peer_re = rf"(?im)^\s*{re.escape(peer)}\s+\d+\s+Established\s+\S+\s*$"
                if re.search(peer_re, detail):
                    if peer in peer_groups:
                        valid = False
                        break
                    peer_groups[peer] = group_id
            if not valid:
                break

        if valid and set(peer_groups) == set(expected):
            valid = all(group_states[peer_groups[peer]] == state for peer, state in expected.items())
        else:
            valid = False

        if valid:
            if expected_groups == 1 and len(set(peer_groups.values())) != 1:
                valid = False
            if expected_groups == 2 and len(set(peer_groups.values())) != 2:
                valid = False

        if valid:
            return
        time.sleep(interval)

    detail_dump = "\n".join(
        f"--- group {group_id} ---\n{text}" for group_id, text in last_details.items()
    )
    raise RuntimeError(
        f"{case.vpn_af.upper()} update-group partition did not converge to {expected}\n"
        f"--- summary ---\n{last_summary}\n{detail_dump}"
    )


def _peer_wire_checks(
    *,
    sender_peer: str,
    receiver_peer: str,
    expected_sid: bool,
    service_sid: str,
    transport_nh: str,
    case: VpnFamilyCase = VPNV4,
) -> list[dict[str, object]]:
    prefix_re = re.escape(f"vpn:{RD}:{case.private_addr}/{case.private_len}")
    if expected_sid:
        receive_regex = [
            rf"(?im)^\s*{prefix_re}\s+label=3\s+{re.escape(service_sid)}\s+",
        ]
        receive_not_regex = [rf"(?im)^\s*{prefix_re}\s+label={MPLS_LABEL_RE}\s+"]
        mode = f"label 3 and the {case.endpoint_behavior} Service SID"
    else:
        receive_regex = [
            rf"(?im)^\s*{prefix_re}\s+label={MPLS_LABEL_RE}\s+{re.escape(transport_nh)}\s+",
        ]
        receive_not_regex = [rf"(?im)^\s*{prefix_re}\s+label=3\s+"]
        mode = "a real MPLS VPN label and transport nexthop"

    return [
        {
            "device": "a",
            "command": (
                f"show bgp route af {case.vpn_af} peer {sender_peer} advertise-routes "
                f"{case.private_addr} {case.private_len}"
            ),
            "contains": [
                "BGP Peer Adj-RIB-Out",
                case.private_addr,
                "Total: 1 advertised routes",
            ],
            "label": f"a keeps the exported VPN route in peer {sender_peer}'s update-group",
        },
        {
            "device": "b",
            "command": (
                f"show bgp route af {case.vpn_af} peer {receiver_peer} receive-routes "
                f"{case.private_addr} {case.private_len}"
            ),
            "contains": [
                "BGP Peer Adj-RIB-In",
                case.private_addr,
                "Total: 1 received routes",
            ],
            "regex": receive_regex,
            "not_regex": receive_not_regex,
            "label": f"b receives {mode} from peer {receiver_peer}",
        },
    ]


def _wait_peer_wire_modes(
    rt: TopologyRuntime,
    *,
    a_ge1_peer: str,
    a_ge2_peer: str,
    b_ge1_peer: str,
    b_ge2_peer: str,
    service_sid: str,
    ge1_sid: bool,
    ge2_sid: bool,
    case: VpnFamilyCase = VPNV4,
) -> None:
    wait_checks(
        rt,
        [
            *_peer_wire_checks(
                sender_peer=a_ge1_peer,
                receiver_peer=b_ge1_peer,
                expected_sid=ge1_sid,
                service_sid=service_sid,
                transport_nh=b_ge1_peer,
                case=case,
            ),
            *_peer_wire_checks(
                sender_peer=a_ge2_peer,
                receiver_peer=b_ge2_peer,
                expected_sid=ge2_sid,
                service_sid=service_sid,
                transport_nh=b_ge2_peer,
                case=case,
            ),
        ],
        timeout=90,
        interval=2,
    )


def _run_vpnv6_partition_sequence(
    rt: TopologyRuntime,
    *,
    a_ge1_peer: str,
    a_ge2_peer: str,
    b_ge1_peer: str,
    b_ge2_peer: str,
    locator_nh: str,
) -> None:
    step("Configure one VPNv6 export and two otherwise-identical IPv6 eBGP peers")
    _configure(
        rt,
        a_ge1_peer=a_ge1_peer,
        a_ge2_peer=a_ge2_peer,
        b_ge1_peer=b_ge1_peer,
        b_ge2_peer=b_ge2_peer,
        locator_nh=locator_nh,
        case=VPNV6,
    )
    wait_checks(
        rt,
        [
            _established_check("a", a_ge1_peer, case=VPNV6),
            _established_check("a", a_ge2_peer, case=VPNV6),
            _established_check("b", b_ge1_peer, case=VPNV6),
            _established_check("b", b_ge2_peer, case=VPNV6),
        ],
        timeout=90,
        interval=2,
    )
    service_sid = _wait_service_sid(rt, case=VPNV6)

    step("VPNv6 default MPLS peers share one update-group")
    _wait_update_group_partition(
        rt,
        expected={a_ge1_peer: False, a_ge2_peer: False},
        case=VPNV6,
    )
    wait_checks(
        rt,
        [
            _bdr_check(
                a_ge1_peer=a_ge1_peer,
                a_ge2_peer=a_ge2_peer,
                ge1_sid=False,
                ge2_sid=False,
                label="a BDR keeps both VPNv6 peers in default MPLS mode",
                case=VPNV6,
            )
        ],
        timeout=30,
        interval=2,
    )
    _wait_peer_wire_modes(
        rt,
        a_ge1_peer=a_ge1_peer,
        a_ge2_peer=a_ge2_peer,
        b_ge1_peer=b_ge1_peer,
        b_ge2_peer=b_ge2_peer,
        service_sid=service_sid,
        ge1_sid=False,
        ge2_sid=False,
        case=VPNV6,
    )

    step("Enable VPNv6 srv6-sid only for GE-1; peers split into SID and MPLS update-groups")
    # Exercise DB/runtime identity across equivalent IPv6 spellings: the AF
    # neighbor was enabled with compressed text, while srv6-sid uses exploded
    # text. BDR and update-group output must remain canonical.
    a_ge1_peer_expanded = ipaddress.ip_address(a_ge1_peer).exploded
    _set_peer_srv6_sid(rt, peer=a_ge1_peer_expanded, enabled=True, case=VPNV6)
    _wait_update_group_partition(
        rt,
        expected={a_ge1_peer: True, a_ge2_peer: False},
        case=VPNV6,
    )
    wait_checks(
        rt,
        [
            _bdr_check(
                a_ge1_peer=a_ge1_peer,
                a_ge2_peer=a_ge2_peer,
                ge1_sid=True,
                ge2_sid=False,
                label="a BDR enables SID only for the GE-1 VPNv6 peer",
                case=VPNV6,
            )
        ],
        timeout=30,
        interval=2,
    )
    _wait_peer_wire_modes(
        rt,
        a_ge1_peer=a_ge1_peer,
        a_ge2_peer=a_ge2_peer,
        b_ge1_peer=b_ge1_peer,
        b_ge2_peer=b_ge2_peer,
        service_sid=service_sid,
        ge1_sid=True,
        ge2_sid=False,
        case=VPNV6,
    )

    step("Enable VPNv6 srv6-sid for GE-2; equal SID peers merge into one update-group")
    _set_peer_srv6_sid(rt, peer=a_ge2_peer, enabled=True, case=VPNV6)
    _wait_update_group_partition(
        rt,
        expected={a_ge1_peer: True, a_ge2_peer: True},
        case=VPNV6,
    )
    wait_checks(
        rt,
        [
            _bdr_check(
                a_ge1_peer=a_ge1_peer,
                a_ge2_peer=a_ge2_peer,
                ge1_sid=True,
                ge2_sid=True,
                label="a BDR enables SID for both VPNv6 peers",
                case=VPNV6,
            )
        ],
        timeout=30,
        interval=2,
    )
    _wait_peer_wire_modes(
        rt,
        a_ge1_peer=a_ge1_peer,
        a_ge2_peer=a_ge2_peer,
        b_ge1_peer=b_ge1_peer,
        b_ge2_peer=b_ge2_peer,
        service_sid=service_sid,
        ge1_sid=True,
        ge2_sid=True,
        case=VPNV6,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("a", "b"))

    a_ge1_peer = str(g_top.a.GE_1.peer_ip)
    a_ge2_peer = str(g_top.a.GE_2.peer_ip)
    b_ge1_peer = str(g_top.b.GE_1.peer_ip)
    b_ge2_peer = str(g_top.b.GE_2.peer_ip)
    a_ge1_peer6 = str(g_top.a.GE_1.peer_ip6)
    a_ge2_peer6 = str(g_top.a.GE_2.peer_ip6)
    b_ge1_peer6 = str(g_top.b.GE_1.peer_ip6)
    b_ge2_peer6 = str(g_top.b.GE_2.peer_ip6)

    try:
        _cleanup(rt, locator_nh=b_ge1_peer6)

        step("Configure one VPNv4 export and two otherwise-identical eBGP peers")
        _configure(
            rt,
            a_ge1_peer=a_ge1_peer,
            a_ge2_peer=a_ge2_peer,
            b_ge1_peer=b_ge1_peer,
            b_ge2_peer=b_ge2_peer,
            locator_nh=b_ge1_peer6,
        )
        wait_checks(
            rt,
            [
                _established_check("a", a_ge1_peer),
                _established_check("a", a_ge2_peer),
                _established_check("b", b_ge1_peer),
                _established_check("b", b_ge2_peer),
                {
                    "device": "a",
                    "command": f"show bgp neighbor af vpnv4 {a_ge1_peer}",
                    "regex": [r"(?im)^\s*Extended-Nexthop\s+Yes\s+Yes\s+Yes\s*$"],
                    "label": "a GE-1 VPNv4 peer negotiated the IPv6 nexthop capability",
                },
                {
                    "device": "a",
                    "command": f"show bgp neighbor af vpnv4 {a_ge2_peer}",
                    "regex": [r"(?im)^\s*Extended-Nexthop\s+Yes\s+Yes\s+Yes\s*$"],
                    "label": "a GE-2 VPNv4 peer negotiated the IPv6 nexthop capability",
                },
            ],
            timeout=90,
            interval=2,
        )
        service_sid = _wait_service_sid(rt)

        step("Default MPLS peers share one update-group")
        _wait_update_group_partition(
            rt,
            expected={a_ge1_peer: False, a_ge2_peer: False},
        )
        wait_checks(
            rt,
            [
                _bdr_check(
                    a_ge1_peer=a_ge1_peer,
                    a_ge2_peer=a_ge2_peer,
                    ge1_sid=False,
                    ge2_sid=False,
                    label="a BDR keeps both VPNv4 peers in default MPLS mode",
                )
            ],
            timeout=30,
            interval=2,
        )
        _wait_peer_wire_modes(
            rt,
            a_ge1_peer=a_ge1_peer,
            a_ge2_peer=a_ge2_peer,
            b_ge1_peer=b_ge1_peer,
            b_ge2_peer=b_ge2_peer,
            service_sid=service_sid,
            ge1_sid=False,
            ge2_sid=False,
        )

        step("Enable srv6-sid only for GE-1; peers split into SID and MPLS update-groups")
        _set_peer_srv6_sid(rt, peer=a_ge1_peer, enabled=True)
        _wait_update_group_partition(
            rt,
            expected={a_ge1_peer: True, a_ge2_peer: False},
        )
        wait_checks(
            rt,
            [
                _bdr_check(
                    a_ge1_peer=a_ge1_peer,
                    a_ge2_peer=a_ge2_peer,
                    ge1_sid=True,
                    ge2_sid=False,
                    label="a BDR enables SID only for the GE-1 VPNv4 peer",
                )
            ],
            timeout=30,
            interval=2,
        )
        _wait_peer_wire_modes(
            rt,
            a_ge1_peer=a_ge1_peer,
            a_ge2_peer=a_ge2_peer,
            b_ge1_peer=b_ge1_peer,
            b_ge2_peer=b_ge2_peer,
            service_sid=service_sid,
            ge1_sid=True,
            ge2_sid=False,
        )

        step("Reboot a BGP in mixed mode and verify per-peer grouping and wire encoding recover")
        process_reboot(rt, "a", "bgp", ready_timeout=90)
        wait_checks(
            rt,
            [
                _established_check("a", a_ge1_peer),
                _established_check("a", a_ge2_peer),
                _established_check("b", b_ge1_peer),
                _established_check("b", b_ge2_peer),
                _bdr_check(
                    a_ge1_peer=a_ge1_peer,
                    a_ge2_peer=a_ge2_peer,
                    ge1_sid=True,
                    ge2_sid=False,
                    label="a restores the mixed per-peer SID configuration after BGP reboot",
                ),
            ],
            timeout=120,
            interval=2,
        )
        service_sid = _wait_service_sid(rt)
        _wait_update_group_partition(
            rt,
            expected={a_ge1_peer: True, a_ge2_peer: False},
            timeout=120,
        )
        _wait_peer_wire_modes(
            rt,
            a_ge1_peer=a_ge1_peer,
            a_ge2_peer=a_ge2_peer,
            b_ge1_peer=b_ge1_peer,
            b_ge2_peer=b_ge2_peer,
            service_sid=service_sid,
            ge1_sid=True,
            ge2_sid=False,
        )

        step("Enable srv6-sid for GE-2 as well; equal SID peers merge into one update-group")
        _set_peer_srv6_sid(rt, peer=a_ge2_peer, enabled=True)
        _wait_update_group_partition(
            rt,
            expected={a_ge1_peer: True, a_ge2_peer: True},
        )
        wait_checks(
            rt,
            [
                _bdr_check(
                    a_ge1_peer=a_ge1_peer,
                    a_ge2_peer=a_ge2_peer,
                    ge1_sid=True,
                    ge2_sid=True,
                    label="a BDR enables SID for both VPNv4 peers",
                )
            ],
            timeout=30,
            interval=2,
        )
        _wait_peer_wire_modes(
            rt,
            a_ge1_peer=a_ge1_peer,
            a_ge2_peer=a_ge2_peer,
            b_ge1_peer=b_ge1_peer,
            b_ge2_peer=b_ge2_peer,
            service_sid=service_sid,
            ge1_sid=True,
            ge2_sid=True,
        )

        step("Disable GE-1 srv6-sid; the inverse mixed mode splits the update-groups again")
        _set_peer_srv6_sid(rt, peer=a_ge1_peer, enabled=False)
        _wait_update_group_partition(
            rt,
            expected={a_ge1_peer: False, a_ge2_peer: True},
        )
        _wait_peer_wire_modes(
            rt,
            a_ge1_peer=a_ge1_peer,
            a_ge2_peer=a_ge2_peer,
            b_ge1_peer=b_ge1_peer,
            b_ge2_peer=b_ge2_peer,
            service_sid=service_sid,
            ge1_sid=False,
            ge2_sid=True,
        )

        step("Disable GE-2 srv6-sid; both default peers merge and return to real labels")
        _set_peer_srv6_sid(rt, peer=a_ge2_peer, enabled=False)
        _wait_update_group_partition(
            rt,
            expected={a_ge1_peer: False, a_ge2_peer: False},
        )
        wait_checks(
            rt,
            [
                _bdr_check(
                    a_ge1_peer=a_ge1_peer,
                    a_ge2_peer=a_ge2_peer,
                    ge1_sid=False,
                    ge2_sid=False,
                    label="a BDR removes both peer SID selectors",
                )
            ],
            timeout=30,
            interval=2,
        )
        _wait_peer_wire_modes(
            rt,
            a_ge1_peer=a_ge1_peer,
            a_ge2_peer=a_ge2_peer,
            b_ge1_peer=b_ge1_peer,
            b_ge2_peer=b_ge2_peer,
            service_sid=service_sid,
            ge1_sid=False,
            ge2_sid=False,
        )

        step("Remove the VPNv4 scenario before the VPNv6 partition coverage")
        _cleanup(rt, locator_nh=b_ge1_peer6)

        _run_vpnv6_partition_sequence(
            rt,
            a_ge1_peer=a_ge1_peer6,
            a_ge2_peer=a_ge2_peer6,
            b_ge1_peer=b_ge1_peer6,
            b_ge2_peer=b_ge2_peer6,
            locator_nh=b_ge1_peer6,
        )

        step("Remove the complete scenario and verify BGP/VRF/SRv6 BDR cleanup")
        _cleanup(rt, locator_nh=b_ge1_peer6)
        wait_checks(
            rt,
            [
                {
                    "device": "a",
                    "command": "show current-configuration",
                    "contains": ["sysname a"],
                    "not_regex": [
                        rf"(?m)^bgp {A_AS}\r?$",
                        rf"(?m)^vrf {re.escape(VRF_NAME)}\r?$",
                        rf"(?m)^srv6\r?$",
                    ],
                    "label": "a removes BGP, red VRF, and SRv6 configuration",
                },
                {
                    "device": "b",
                    "command": "show current-configuration",
                    "contains": ["sysname b"],
                    "not_regex": [rf"(?m)^bgp {B_AS}\r?$"],
                    "label": "b removes its BGP configuration",
                },
            ],
            timeout=60,
            interval=2,
        )

        print("Per-peer SRv6 SID update-group split/merge check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt, locator_nh=b_ge1_peer6)
