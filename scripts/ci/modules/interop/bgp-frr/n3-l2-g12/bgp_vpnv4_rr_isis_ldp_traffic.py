#!/usr/bin/env python3
"""
NetNexus -- NetNexus(RR) -- FRR iBGP vpnv4 L3VPN interop.

Topology:
  r1(NetNexus PE) -- r2(NetNexus RR/P) -- r3(FRR PE), all in AS 65001.

Coverage:
- ISIS advertises PE/RR loopbacks across all three nodes.
- LDP forms r1-r2 and r2-r3 sessions for public loopback transport.
- r2 reflects vpnv4 routes between r1 and r3.
- VRF red customer loopbacks on r1 and r3 are reachable in both directions.
"""

from __future__ import annotations

import re
import shlex
import subprocess
import time

from module_api import (  # noqa: E402
    frr_config,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


AS = 65001
TAG = 1
VRF_NAME = "red"
RT = "65001:100"
LEN = 32

R1_CORE_IF = "GE-1"
R2_R1_IF = "GE-1"
R2_R3_IF = "GE-2"
FRR_CORE_IF = "eth1"

R1_LOOP, R1_ID = 11, "1.1.1.1"
R2_LOOP, R2_ID = 22, "2.2.2.2"
R3_ID = "3.3.3.3"

R1_CUST_LOOP, R1_CUST = 110, "100.1.1.1"
R3_CUST_IF, R3_CUST = "red110", "100.3.3.3"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"
R3_NET = "49.0001.0000.0000.0003.00"

R2_R3_LINK = "10.23.0.1"
FRR_LINK = "10.23.0.2"


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
    if "bytes from" not in output and " 0% packet loss" not in output:
        return False
    m = re.search(r"(\d+)%\s*packet loss", output)
    return not (m and int(m.group(1)) >= 100)


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup NetNexus/FRR vpnv4 RR interop config")
    for dev, ifaces, loops in (
        ("r1", (R1_CORE_IF,), (R1_LOOP, R1_CUST_LOOP)),
        ("r2", (R2_R1_IF, R2_R3_IF), (R2_LOOP,)),
    ):
        cmds = ["end", "config", "no bgp"]
        for ifname in ifaces:
            cmds.extend([f"if {ifname}", "no ldp enable", f"no isis enable {TAG}", "exit"])
        cmds.extend(["no ldp", f"no isis {TAG}"])
        for loop in loops:
            cmds.append(f"no if loop {loop}")
        cmds.extend([f"no vrf {VRF_NAME}", "end"])
        run_cmds(rt=rt, device=dev, commands=cmds, strict=False)

    frr_config(
        rt,
        "r3",
        [
            f"no router bgp {AS} vrf {VRF_NAME}",
            f"no router bgp {AS}",
            "no mpls ldp",
            f"interface {FRR_CORE_IF}",
            "no mpls enable",
            f"no ip router isis {TAG}",
            "exit",
            "interface lo",
            f"no ip router isis {TAG}",
            "exit",
            f"no router isis {TAG}",
        ],
        strict=False,
    )
    _docker_exec(rt, "r3", f"ip addr del {R3_ID}/{LEN} dev lo 2>/dev/null || true", timeout=5)
    _docker_exec(
        rt,
        "r3",
        "for l in $(ip -M route show 2>/dev/null | awk '/ dev red( |$)/ {print $1}'); do "
        'ip -M route del "$l" 2>/dev/null || true; '
        "done; "
        "ip link del " + shlex.quote(R3_CUST_IF) + " 2>/dev/null || true; "
        "ip link del " + shlex.quote(VRF_NAME) + " 2>/dev/null || true",
        timeout=5,
    )


def _configure_netnexus_core(
    rt: TopologyRuntime,
    *,
    device: str,
    net: str,
    lsr_id: str,
    loop_id: int,
    ifaces: tuple[str, ...],
) -> None:
    commands = [
        "config",
        f"if loop {loop_id}",
        f"ip address {lsr_id} {LEN}",
        "exit",
        f"isis {TAG}",
        f"net {net}",
        "is-type level-1-2",
        "cost-style wide",
        "af ipv4",
        "exit",
    ]
    for ifname in ifaces:
        commands.extend(
            [
                f"if {ifname}",
                f"isis enable {TAG}",
                f"isis hello-interval {TAG} 3",
                f"isis hold-multiplier {TAG} 3",
                "exit",
            ]
        )
    commands.extend(
        [
            f"if loop {loop_id}",
            f"isis enable {TAG}",
            f"isis passive {TAG}",
            "exit",
            "ldp",
            f"lsr-id {lsr_id}",
            "hello-interval 1000",
            "hold-time 3000",
            "keepalive-interval 3000",
            "exit",
        ]
    )
    for ifname in ifaces:
        commands.extend([f"if {ifname}", "ldp enable", "exit"])
    commands.append("end")
    run_cmds(rt=rt, device=device, commands=commands)


def _configure_netnexus_vrf(rt: TopologyRuntime, device: str, *, rd: str) -> None:
    run_cmds(
        rt=rt,
        device=device,
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            f"route-distinguisher {rd}",
            "apply-label per-vrf",
            f"vpn-target {RT} export",
            f"vpn-target {RT} import",
            "exit",
            "exit",
            "end",
        ],
    )


def _configure_r1_bgp(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if loop {R1_CUST_LOOP}",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {R1_CUST} {LEN}",
            "exit",
            f"bgp {AS}",
            f"router-id {R1_ID}",
            f"neighbor {R2_ID} as {AS}",
            f"neighbor {R2_ID} source-interface loop{R1_LOOP}",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "import-route connected",
            "exit",
            "exit",
            "af vpnv4",
            f"neighbor {R2_ID} enable",
            "exit",
            "end",
        ],
    )


def _configure_r2_bgp_rr(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r2",
        commands=[
            "config",
            f"bgp {AS}",
            f"router-id {R2_ID}",
            f"neighbor {R1_ID} as {AS}",
            f"neighbor {R1_ID} source-interface loop{R2_LOOP}",
            f"neighbor {R3_ID} as {AS}",
            f"neighbor {R3_ID} source-interface loop{R2_LOOP}",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "exit",
            "exit",
            "af vpnv4",
            f"neighbor {R1_ID} enable",
            f"neighbor {R1_ID} reflect-client",
            f"neighbor {R3_ID} enable",
            f"neighbor {R3_ID} reflect-client",
            "exit",
            "end",
        ],
    )


def _configure_frr_linux_vrf(rt: TopologyRuntime) -> None:
    _docker_exec_ok(
        rt,
        "r3",
        "sysctl -w net.mpls.platform_labels=20000 "
        f"net.mpls.conf.{shlex.quote(FRR_CORE_IF)}.input=1 "
        "net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.default.rp_filter=0 >/dev/null; "
        "ip link set dev lo up; "
        f"ip addr replace {R3_ID}/{LEN} dev lo; "
        f"ip link add {shlex.quote(VRF_NAME)} type vrf table 1001 2>/dev/null || true; "
        f"ip link set dev {shlex.quote(VRF_NAME)} up; "
        f"ip link add {shlex.quote(R3_CUST_IF)} type dummy 2>/dev/null || true; "
        f"ip link set dev {shlex.quote(R3_CUST_IF)} master {shlex.quote(VRF_NAME)}; "
        f"ip link set dev {shlex.quote(R3_CUST_IF)} up; "
        f"ip addr replace {R3_CUST}/{LEN} dev {shlex.quote(R3_CUST_IF)}",
    )


def _configure_frr(rt: TopologyRuntime) -> None:
    _configure_frr_linux_vrf(rt)
    frr_config(
        rt,
        "r3",
        [
            f"router isis {TAG}",
            f"net {R3_NET}",
            "is-type level-1-2",
            "metric-style wide",
            "exit",
            f"interface {FRR_CORE_IF}",
            f"ip router isis {TAG}",
            "isis hello-interval 3",
            "isis hello-multiplier 3",
            "exit",
            "interface lo",
            f"ip router isis {TAG}",
            "isis passive",
            "exit",
            "mpls ldp",
            f"router-id {R3_ID}",
            "address-family ipv4",
            f"discovery transport-address {FRR_LINK}",
            "discovery hello interval 1",
            "discovery hello holdtime 3",
            "session holdtime 15",
            "label local allocate host-routes",
            f"interface {FRR_CORE_IF}",
            "exit",
            "exit-address-family",
            f"router bgp {AS}",
            f"bgp router-id {R3_ID}",
            "no bgp ebgp-requires-policy",
            f"neighbor {R2_ID} remote-as {AS}",
            f"neighbor {R2_ID} update-source lo",
            "address-family ipv4 vpn",
            f"neighbor {R2_ID} activate",
            "exit-address-family",
            f"router bgp {AS} vrf {VRF_NAME}",
            f"bgp router-id {R3_ID}",
            "address-family ipv4 unicast",
            f"rd vpn export {AS}:3",
            f"rt vpn both {RT}",
            "label vpn export auto",
            "redistribute connected",
            "import vpn",
            "export vpn",
            "exit-address-family",
        ],
    )


def _ensure_frr_vpn_label_route(rt: TopologyRuntime) -> None:
    out = _docker_exec_ok(
        rt,
        "r3",
        f"vtysh -c 'show bgp ipv4 vpn rd {AS}:3 {R3_CUST}/{LEN}'",
    )
    m = re.search(r"(?im)\bRemote label:\s*(\d+)\b", out)
    if not m:
        m = re.search(r"(?im)\blabel=(\d+)\b", out)
    if not m:
        raise RuntimeError(f"FRR did not expose VPN label for {R3_CUST}/{LEN}:\n{out}")

    label = int(m.group(1))
    if label <= 0:
        raise RuntimeError(f"FRR returned invalid VPN label {label} for {R3_CUST}/{LEN}:\n{out}")

    _docker_exec_ok(
        rt,
        "r3",
        "sysctl -w net.mpls.platform_labels=20000 "
        f"net.mpls.conf.{shlex.quote(FRR_CORE_IF)}.input=1 >/dev/null; "
        f"ip -M route replace {label} dev {shlex.quote(VRF_NAME)}; "
        "ip -M route show",
    )


def _fib_tunnel_check(device: str, dst: str, label: str) -> dict:
    return {
        "device": device,
        "command": f"show fib ipv4 vrf {VRF_NAME} {dst} {LEN}",
        "contains": [f"Routing entry for {dst}/{LEN}"],
        "regex": [
            r"(?im)^\s*NH-Type\s*:\s*tunnel\s*$",
            r"(?im)^\s*Out-Label\s*:\s*[1-9]\d*\s*$",
            r"(?im)^\s*Installed\s*:\s*yes\s*$",
        ],
        "label": label,
    }


def _nn_vpnv4_route_check(device: str, prefix: str, label: str) -> dict:
    return {
        "device": device,
        "command": "show bgp route af vpnv4",
        "contains": [prefix],
        "label": label,
    }


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3"))

    try:
        _cleanup(rt)

        step("Configure ISIS and LDP underlay on r1/r2/r3")
        _configure_netnexus_core(rt, device="r1", net=R1_NET, lsr_id=R1_ID, loop_id=R1_LOOP, ifaces=(R1_CORE_IF,))
        _configure_netnexus_core(
            rt,
            device="r2",
            net=R2_NET,
            lsr_id=R2_ID,
            loop_id=R2_LOOP,
            ifaces=(R2_R1_IF, R2_R3_IF),
        )
        _configure_frr(rt)

        step("Configure VRF red and vpnv4 BGP: r2 as route reflector")
        _configure_netnexus_vrf(rt, "r1", rd=f"{AS}:1")
        _configure_netnexus_vrf(rt, "r2", rd=f"{AS}:2")
        time.sleep(2)
        _configure_r1_bgp(rt)
        _configure_r2_bgp_rr(rt)

        step("Wait ISIS convergence and LDP sessions")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis neighbor {TAG}",
                    "contains": [R1_CORE_IF, "Up"],
                    "label": "r1 sees r2 ISIS up",
                },
                {
                    "device": "r2",
                    "command": f"show isis neighbor {TAG}",
                    "contains": [R2_R1_IF, R2_R3_IF, "Up"],
                    "label": "r2 sees r1 and r3 ISIS up",
                },
                {
                    "device": "r3",
                    "command": "vtysh -c 'show isis neighbor'",
                    "regex": [rf"(?im)^\s*\S+\s+{re.escape(FRR_CORE_IF)}\s+[12]\s+Up\b"],
                    "label": "FRR sees r2 ISIS up",
                },
                {
                    "device": "r1",
                    "command": "show route ipv4 proto isis",
                    "contains": [f"{R2_ID}/{LEN}", f"{R3_ID}/{LEN}"],
                    "label": "r1 has r2/r3 loopbacks via ISIS",
                },
                {
                    "device": "r3",
                    "command": "vtysh -c 'show ip route isis'",
                    "contains": [f"{R1_ID}/{LEN}", f"{R2_ID}/{LEN}"],
                    "label": "FRR has r1/r2 loopbacks via ISIS",
                },
                {
                    "device": "r1",
                    "command": "show ldp neighbor",
                    "contains": [R2_ID, R1_CORE_IF, "OPERATIONAL"],
                    "label": "r1 LDP to r2 operational",
                },
                {
                    "device": "r2",
                    "command": "show ldp neighbor",
                    "contains": [R1_ID, R3_ID, "OPERATIONAL"],
                    "label": "r2 LDP to r1/r3 operational",
                },
                {
                    "device": "r3",
                    "command": "vtysh -c 'show mpls ldp neighbor'",
                    "contains": ["ipv4", R2_ID, R2_R3_LINK, "OPERATIONAL"],
                    "label": "FRR LDP to r2 operational",
                },
            ],
            timeout=140,
            interval=3,
        )

        step("Wait vpnv4 iBGP sessions Established")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af vpnv4",
                    "regex": [rf"(?im)^\s*{re.escape(R2_ID)}\s+\S+\s+\S+\s+Established\s*$"],
                    "label": "r1 vpnv4 to r2 established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af vpnv4",
                    "regex": [
                        rf"(?im)^\s*{re.escape(R1_ID)}\s+\S+\s+\S+\s+Established\s*$",
                        rf"(?im)^\s*{re.escape(R3_ID)}\s+\S+\s+\S+\s+Established\s*$",
                    ],
                    "label": "r2 vpnv4 sessions to r1/r3 established",
                },
                {
                    "device": "r3",
                    "command": "vtysh -c 'show bgp ipv4 vpn summary json'",
                    "contains": [R2_ID],
                    "regex": [r'"state"\s*:\s*"Established"'],
                    "label": "FRR vpnv4 to r2 established",
                },
            ],
            timeout=100,
            interval=3,
        )

        step("Verify local vpnv4 export and RR vpnv4 RIB reception")
        wait_checks(
            rt,
            [
                _nn_vpnv4_route_check("r1", R1_CUST, "r1 exports local customer route into vpnv4"),
                {
                    "device": "r3",
                    "command": "vtysh -c 'show bgp ipv4 vpn'",
                    "contains": [R3_CUST],
                    "label": "FRR exports local customer route into vpnv4",
                },
                _nn_vpnv4_route_check("r2", R1_CUST, "r2 RR received r1 vpnv4 route"),
                _nn_vpnv4_route_check("r2", R3_CUST, "r2 RR received FRR vpnv4 route"),
            ],
            timeout=80,
            interval=3,
        )

        step("Verify r2 reflects vpnv4 routes between NetNexus and FRR")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                    "contains": [R3_CUST],
                    "label": "r1 VRF red learned FRR customer route",
                },
                {
                    "device": "r3",
                    "command": f"vtysh -c 'show ip route vrf {VRF_NAME} {R1_CUST}'",
                    "contains": [R1_CUST],
                    "label": "FRR VRF red learned NetNexus customer route",
                },
                _fib_tunnel_check("r1", R3_CUST, "r1 FIB to FRR customer via LDP tunnel+VPN label"),
            ],
            timeout=120,
            interval=3,
        )

        step("Ensure FRR Linux kernel terminates exported VPN label into VRF red")
        _ensure_frr_vpn_label_route(rt)

        step("Bidirectional data-plane ping in VRF red")

        def _ping_r1_to_r3() -> bool:
            out = rt.exec_cmd("r1", f"ping {R3_CUST} -a {R1_CUST} vrf {VRF_NAME}", timeout=25)
            print(out, flush=True)
            return _ping_ok(out)

        ok = False
        deadline = time.time() + 45
        while time.time() < deadline:
            if _ping_r1_to_r3():
                ok = True
                break
            time.sleep(4)
        if not ok:
            raise RuntimeError("r1 -> r3 vpnv4 L3VPN ping failed (no reply / 100% loss)")

        out = rt.exec_cmd("r3", f"ip vrf exec {VRF_NAME} ping -c 3 -W 3 -I {R3_CUST} {R1_CUST}", timeout=25)
        print(out, flush=True)
        if not _ping_ok(out):
            raise RuntimeError("r3 -> r1 vpnv4 L3VPN ping failed (no reply / 100% loss)")

        print("NetNexus/FRR vpnv4 RR ISIS+LDP bidirectional traffic check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
