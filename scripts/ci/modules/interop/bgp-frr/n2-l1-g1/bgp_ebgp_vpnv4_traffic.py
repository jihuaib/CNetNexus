#!/usr/bin/env python3
"""
NetNexus <-> FRR direct eBGP vpnv4 L3VPN traffic interop.

Topology:
  r1(NetNexus PE, AS 65001) -- f1(FRR PE, AS 65002)

Coverage:
- No ISIS/LDP underlay is configured.
- Directly connected eBGP vpnv4 session exchanges VRF red routes.
- NetNexus imports the FRR VRF route through the eBGP-vpnv4 adjacency tunnel.
- VRF red customer loopbacks are reachable in both directions.
"""

from __future__ import annotations

import re
import shlex
import subprocess
import time

from module_api import (  # noqa: E402
    frr_config,
    g_top,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


NN_AS = 65001
FRR_AS = 65002
VRF_NAME = "red"
RT = "65001:100"
LEN = 32

NN_RD = "65001:1"
FRR_RD = "65002:1"

NN_LOOP, NN_CUST = 110, "100.1.1.1"
FRR_CUST_IF, FRR_CUST = "red110", "100.2.2.2"
FRR_CORE_IF = "eth1"


def _docker_exec(rt: TopologyRuntime, device: str, sh_cmd: str, *, timeout: int = 30) -> tuple[int, str]:
    proc = subprocess.run(
        ["docker", "exec", rt.container_name(device), "/bin/sh", "-lc", sh_cmd],
        text=True,
        capture_output=True,
        check=False,
        timeout=timeout,
    )
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def _docker_exec_ok(rt: TopologyRuntime, device: str, sh_cmd: str, *, timeout: int = 30) -> str:
    rc, out = _docker_exec(rt, device, sh_cmd, timeout=timeout)
    if rc != 0:
        raise RuntimeError(f"{device}: shell command failed ({rc}): {sh_cmd}\n{out}")
    return out


def _ping_ok(output: str) -> bool:
    if "bytes from" not in output:
        return False
    m = re.search(r"(\d+)%\s*packet loss", output)
    return not (m and int(m.group(1)) >= 100)


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup direct eBGP vpnv4 interop config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"no if loop {NN_LOOP}",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )
    frr_config(
        rt,
        "f1",
        [
            f"interface {FRR_CORE_IF}",
            "no mpls enable",
            "exit",
            f"no router bgp {FRR_AS} vrf {VRF_NAME}",
            f"no router bgp {FRR_AS}",
        ],
        strict=False,
    )
    _docker_exec(
        rt,
        "f1",
        "for l in $(ip -M route show 2>/dev/null | awk '/ dev red( |$)/ {print $1}'); do "
        'ip -M route del "$l" 2>/dev/null || true; '
        "done; "
        f"ip route del vrf {shlex.quote(VRF_NAME)} {NN_CUST}/{LEN} 2>/dev/null || true; "
        "ip link del " + shlex.quote(FRR_CUST_IF) + " 2>/dev/null || true; "
        "ip link del " + shlex.quote(VRF_NAME) + " 2>/dev/null || true",
        timeout=5,
    )


def _configure_netnexus(rt: TopologyRuntime, peer_ip: str) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"route-distinguisher {NN_RD}",
            "apply-label per-vrf",
            f"vpn-target {RT} export",
            f"vpn-target {RT} import",
            "exit",
            "exit",
            f"if loop {NN_LOOP}",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {NN_CUST} {LEN}",
            "exit",
            f"bgp {NN_AS}",
            "router-id 1.1.1.1",
            f"neighbor {peer_ip} as {FRR_AS}",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "import-route connected",
            "exit",
            "exit",
            "af vpnv4",
            f"neighbor {peer_ip} enable",
            "exit",
            "end",
        ],
    )


def _configure_frr_linux_vrf(rt: TopologyRuntime) -> None:
    _docker_exec_ok(
        rt,
        "f1",
        "sysctl -w net.mpls.platform_labels=20000 "
        f"net.mpls.conf.{shlex.quote(FRR_CORE_IF)}.input=1 "
        "net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.default.rp_filter=0 >/dev/null; "
        f"ip link add {shlex.quote(VRF_NAME)} type vrf table 1001 2>/dev/null || true; "
        f"ip link set dev {shlex.quote(VRF_NAME)} up; "
        f"ip link add {shlex.quote(FRR_CUST_IF)} type dummy 2>/dev/null || true; "
        f"ip link set dev {shlex.quote(FRR_CUST_IF)} master {shlex.quote(VRF_NAME)}; "
        f"ip link set dev {shlex.quote(FRR_CUST_IF)} up; "
        f"ip addr replace {FRR_CUST}/{LEN} dev {shlex.quote(FRR_CUST_IF)}",
    )


def _configure_frr(rt: TopologyRuntime, peer_ip: str) -> None:
    _configure_frr_linux_vrf(rt)
    frr_config(
        rt,
        "f1",
        [
            f"router bgp {FRR_AS}",
            "bgp router-id 2.2.2.2",
            "no bgp ebgp-requires-policy",
            f"neighbor {peer_ip} remote-as {NN_AS}",
            "address-family ipv4 vpn",
            f"neighbor {peer_ip} activate",
            "exit-address-family",
            f"router bgp {FRR_AS} vrf {VRF_NAME}",
            "bgp router-id 2.2.2.2",
            "address-family ipv4 unicast",
            f"rd vpn export {FRR_RD}",
            f"rt vpn both {RT}",
            "label vpn export auto",
            "redistribute connected",
            "import vpn",
            "export vpn",
            "exit-address-family",
        ],
    )


def _ensure_frr_vpn_label_route(rt: TopologyRuntime) -> None:
    out = _docker_exec_ok(rt, "f1", f"vtysh -c 'show bgp ipv4 vpn rd {FRR_RD} {FRR_CUST}/{LEN}'")
    m = re.search(r"(?im)\bRemote label:\s*(\d+)\b", out)
    if not m:
        m = re.search(r"(?im)\blabel=(\d+)\b", out)
    if not m:
        raise RuntimeError(f"FRR did not expose VPN label for {FRR_CUST}/{LEN}:\n{out}")

    label = int(m.group(1))
    if label <= 0:
        raise RuntimeError(f"FRR returned invalid VPN label {label} for {FRR_CUST}/{LEN}:\n{out}")

    _docker_exec_ok(
        rt,
        "f1",
        "sysctl -w net.mpls.platform_labels=20000 "
        f"net.mpls.conf.{shlex.quote(FRR_CORE_IF)}.input=1 >/dev/null; "
        f"ip -M route replace {label} dev {shlex.quote(VRF_NAME)}; "
        "ip -M route show",
    )


def _frr_received_nn_label(rt: TopologyRuntime) -> int:
    out = _docker_exec_ok(rt, "f1", f"vtysh -c 'show bgp ipv4 vpn rd {NN_RD} {NN_CUST}/{LEN}'")
    m = re.search(r"(?im)\bRemote label:\s*(\d+)\b", out)
    if not m:
        m = re.search(r"(?im)\blabel=(\d+)\b", out)
    if not m:
        raise RuntimeError(f"FRR did not expose received NetNexus VPN label for {NN_CUST}/{LEN}:\n{out}")

    label = int(m.group(1))
    if label <= 0:
        raise RuntimeError(f"FRR received invalid NetNexus VPN label {label} for {NN_CUST}/{LEN}:\n{out}")
    return label


def _install_frr_vrf_mpls_route(rt: TopologyRuntime, *, peer_ip: str) -> None:
    label = _frr_received_nn_label(rt)
    _docker_exec_ok(
        rt,
        "f1",
        f"ip route replace vrf {shlex.quote(VRF_NAME)} {NN_CUST}/{LEN} "
        f"encap mpls {label} via inet {shlex.quote(peer_ip)} dev {shlex.quote(FRR_CORE_IF)}; "
        f"ip route show vrf {shlex.quote(VRF_NAME)} {NN_CUST}/{LEN}",
    )


def _nn_fib_tunnel_check() -> dict:
    return {
        "device": "r1",
        "command": f"show fib ipv4 vrf {VRF_NAME} {FRR_CUST} {LEN}",
        "contains": [f"Routing entry for {FRR_CUST}/{LEN}"],
        "regex": [
            r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
            r"(?im)^\s*Out-Label\s*:\s*[1-9]\d*\s*$",
            r"(?im)^\s*Installed\s*:\s*yes\s*$",
        ],
        "label": "NetNexus installs FRR VRF route through eBGP-vpnv4 adjacency tunnel",
    }


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "f1"))
    nn_peer_ip = str(g_top.r1.GE_1.peer_ip)
    frr_peer_ip = str(g_top.f1.GE_1.peer_ip)

    try:
        _cleanup(rt)

        step("Configure NetNexus and FRR as directly connected eBGP vpnv4 PEs")
        _configure_netnexus(rt, nn_peer_ip)
        _configure_frr(rt, frr_peer_ip)
        time.sleep(2)

        step("Wait direct eBGP vpnv4 session Established")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af vpnv4",
                    "regex": [rf"(?im)^\s*{re.escape(nn_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "NetNexus vpnv4 session to FRR established",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show bgp ipv4 vpn summary json'",
                    "contains": [frr_peer_ip],
                    "regex": [r'"state"\s*:\s*"Established"'],
                    "label": "FRR vpnv4 session to NetNexus established",
                },
            ],
            timeout=80,
            interval=3,
        )

        step("Verify vpnv4 exchange and VRF imports on both sides")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp route af vpnv4",
                    "contains": [NN_CUST, FRR_CUST],
                    "label": "NetNexus vpnv4 RIB has local and FRR customer routes",
                },
                {
                    "device": "r1",
                    "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                    "contains": [FRR_CUST],
                    "label": "NetNexus VRF red learned FRR customer route",
                },
                {
                    "device": "f1",
                    "command": f"vtysh -c 'show bgp ipv4 vpn rd {NN_RD} {NN_CUST}/{LEN}'",
                    "contains": [NN_CUST, "Remote label:"],
                    "label": "FRR vpnv4 RIB learned NetNexus customer route and label",
                },
                _nn_fib_tunnel_check(),
            ],
            timeout=80,
            interval=3,
        )

        step("Ensure FRR Linux kernel terminates exported VPN label into VRF red")
        _ensure_frr_vpn_label_route(rt)

        step("Install FRR Linux VRF MPLS route using received NetNexus VPN label")
        _install_frr_vrf_mpls_route(rt, peer_ip=frr_peer_ip)

        step("Bidirectional VRF red traffic over direct eBGP vpnv4")

        def _ping_r1_to_f1() -> bool:
            out = rt.exec_cmd("r1", f"ping {FRR_CUST} -a {NN_CUST} vrf {VRF_NAME}", timeout=25)
            print(out, flush=True)
            return _ping_ok(out)

        ok = False
        deadline = time.time() + 45
        while time.time() < deadline:
            if _ping_r1_to_f1():
                ok = True
                break
            time.sleep(4)
        if not ok:
            raise RuntimeError("r1 -> f1 direct eBGP vpnv4 L3VPN ping failed")

        out = rt.exec_cmd("f1", f"ip vrf exec {VRF_NAME} ping -c 3 -W 3 -I {FRR_CUST} {NN_CUST}", timeout=25)
        print(out, flush=True)
        if not _ping_ok(out):
            raise RuntimeError("f1 -> r1 direct eBGP vpnv4 L3VPN ping failed")

        print("NetNexus/FRR direct eBGP vpnv4 bidirectional traffic check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
