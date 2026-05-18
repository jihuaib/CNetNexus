#!/usr/bin/env python3
"""
NetNexus -- NetNexus -- FRR LDP/ISIS data-plane interop.

Topology:
  r1(NetNexus) -- GE-1/GE-1 -- r2(NetNexus) -- GE-2/eth1 -- f1(FRR)

Coverage:
- ISIS advertises loopback host routes across all three nodes.
- LDP forms r1-r2 and r2-f1 sessions and builds the label chain for f1 loopback.
- r1 forwards explicit `ping mpls` traffic to f1 loopback with an MPLS label on r1-r2.
- r2 performs PHP toward f1, so r2-f1 carries plain IPv4/ICMP.
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
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


TAG = 1

R1_R2_IF = "GE-1"
R2_R1_IF = "GE-1"
R2_F1_IF = "GE-2"
FRR_IF = "eth1"

R1_AB_LINUX_IF = "eth1"
R2_AB_LINUX_IF = "eth1"
R2_BC_LINUX_IF = "eth2"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"
FRR_NET = "49.0001.0000.0000.0003.00"

R1_LSR_ID = "1.1.1.1"
R2_LSR_ID = "2.2.2.2"
FRR_LSR_ID = "3.3.3.3"

R1_LOOP_ID = 11
R2_LOOP_ID = 22
R1_LOOP_V4 = R1_LSR_ID
R2_LOOP_V4 = R2_LSR_ID
FRR_LOOP_V4 = FRR_LSR_ID
LOOP_V4_LEN = 32

R1_LINK_V4 = "10.12.0.1"
R2_AB_LINK_V4 = "10.12.0.2"
R2_BC_LINK_V4 = "10.23.0.1"
FRR_LINK_V4 = "10.23.0.2"

HELLO_INTERVAL_MS = 1000
HOLD_TIME_MS = 3000
KEEPALIVE_INTERVAL_MS = 3000


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


def _capture_start(
    rt: TopologyRuntime,
    *,
    device: str,
    name: str,
    iface: str,
    packet_filter: str,
    seconds: int = 10,
) -> None:
    out_path = f"/tmp/{name}.tcpdump"
    rc_path = f"/tmp/{name}.rc"
    cmd = (
        f"rm -f {shlex.quote(out_path)} {shlex.quote(rc_path)}; "
        f"( timeout {seconds} tcpdump -ni {shlex.quote(iface)} -c 1 -vvv -e "
        f"{shlex.quote(packet_filter)} > {shlex.quote(out_path)} 2>&1; "
        f"echo $? > {shlex.quote(rc_path)} ) &"
    )
    _docker_exec_ok(rt, device, cmd)


def _capture_wait(
    rt: TopologyRuntime,
    *,
    device: str,
    name: str,
    expect_packet: bool,
    timeout: int = 15,
) -> str:
    out_path = f"/tmp/{name}.tcpdump"
    rc_path = f"/tmp/{name}.rc"
    deadline = time.time() + timeout
    while time.time() < deadline:
        rc, _ = _docker_exec(rt, device, f"test -f {shlex.quote(rc_path)}")
        if rc == 0:
            break
        time.sleep(1)

    rc, combined = _docker_exec(
        rt,
        device,
        f"printf 'rc='; cat {shlex.quote(rc_path)} 2>/dev/null || true; "
        f"printf '\\n--- tcpdump ---\\n'; cat {shlex.quote(out_path)} 2>/dev/null || true",
    )
    if rc != 0:
        raise RuntimeError(f"{device}: failed to collect tcpdump result for {name}\n{combined}")

    rc_match = re.search(r"(?m)^rc=(\d+)\s*$", combined)
    if not rc_match:
        raise RuntimeError(f"{device}: tcpdump result for {name} did not finish\n{combined}")
    tcpdump_rc = int(rc_match.group(1))

    if expect_packet and tcpdump_rc != 0:
        raise AssertionError(f"{device}: expected tcpdump packet for {name}, rc={tcpdump_rc}\n{combined}")
    if not expect_packet and tcpdump_rc == 0:
        raise AssertionError(f"{device}: unexpected tcpdump packet for {name}\n{combined}")
    if not expect_packet and tcpdump_rc != 124:
        raise AssertionError(f"{device}: tcpdump absence check for {name} failed rc={tcpdump_rc}\n{combined}")
    return combined


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup NetNexus/FRR LDP+ISIS traffic config")
    for dev, ifaces, loop in (
        ("r1", (R1_R2_IF,), R1_LOOP_ID),
        ("r2", (R2_R1_IF, R2_F1_IF), R2_LOOP_ID),
    ):
        commands = ["end", "config"]
        for ifname in ifaces:
            commands.extend(
                [
                    f"if {ifname}",
                    "no ldp enable",
                    f"no isis enable {TAG}",
                    "exit",
                ]
            )
        commands.extend(
            [
                f"if loop {loop}",
                f"no isis enable {TAG}",
                "exit",
                "no ldp",
                f"no isis {TAG}",
                f"no if loop {loop}",
                "end",
            ]
        )
        run_cmds(rt=rt, device=dev, commands=commands, strict=False)

    frr_config(
        rt,
        "f1",
        [
            "no mpls ldp",
            f"interface {FRR_IF}",
            f"no ip router isis {TAG}",
            "exit",
            "interface lo",
            f"no ip router isis {TAG}",
            "exit",
            f"no router isis {TAG}",
        ],
        strict=False,
    )
    rt.exec_cmd("f1", f"ip addr del {FRR_LOOP_V4}/{LOOP_V4_LEN} dev lo", strict=False)
    _docker_exec(rt, "r2", "rm -f /tmp/ldp_isis_php_*.tcpdump /tmp/ldp_isis_php_*.rc", timeout=5)
    _docker_exec(rt, "f1", "rm -f /tmp/ldp_isis_php_*.tcpdump /tmp/ldp_isis_php_*.rc", timeout=5)


def _configure_netnexus(
    rt: TopologyRuntime,
    *,
    device: str,
    net: str,
    lsr_id: str,
    loop_id: int,
    loop_v4: str,
    ifaces: tuple[str, ...],
) -> None:
    commands = [
        "config",
        f"if loop {loop_id}",
        f"ip address {loop_v4} {LOOP_V4_LEN}",
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
            f"hello-interval {HELLO_INTERVAL_MS}",
            f"hold-time {HOLD_TIME_MS}",
            f"keepalive-interval {KEEPALIVE_INTERVAL_MS}",
            "exit",
        ]
    )
    for ifname in ifaces:
        commands.extend([f"if {ifname}", "ldp enable", "exit"])
    commands.append("end")
    run_cmds(rt=rt, device=device, commands=commands)


def _configure_frr(rt: TopologyRuntime) -> None:
    rt.exec_cmd("f1", "ip link set dev lo up")
    rt.exec_cmd("f1", f"ip addr replace {FRR_LOOP_V4}/{LOOP_V4_LEN} dev lo")
    frr_config(
        rt,
        "f1",
        [
            f"router isis {TAG}",
            f"net {FRR_NET}",
            "is-type level-1-2",
            "metric-style wide",
            "exit",
            f"interface {FRR_IF}",
            f"ip router isis {TAG}",
            "isis hello-interval 3",
            "isis hello-multiplier 3",
            "exit",
            "interface lo",
            f"ip router isis {TAG}",
            "isis passive",
            "exit",
            "mpls ldp",
            f"router-id {FRR_LSR_ID}",
            "address-family ipv4",
            f"discovery transport-address {FRR_LINK_V4}",
            "discovery hello interval 1",
            "discovery hello holdtime 3",
            "session holdtime 15",
            "label local allocate host-routes",
            f"interface {FRR_IF}",
            "exit",
            "exit-address-family",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "f1"))

    try:
        _cleanup(rt)

        step("Ensure baseline links are up")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {R1_R2_IF}",
                    "contains": ["State      : UP", f"IPv4 Addr  : {R1_LINK_V4}/30"],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "r2",
                    "command": f"show if {R2_R1_IF}",
                    "contains": ["State      : UP", f"IPv4 Addr  : {R2_AB_LINK_V4}/30"],
                    "label": "r2 GE-1 up",
                },
                {
                    "device": "r2",
                    "command": f"show if {R2_F1_IF}",
                    "contains": ["State      : UP", f"IPv4 Addr  : {R2_BC_LINK_V4}/30"],
                    "label": "r2 GE-2 up",
                },
                {
                    "device": "f1",
                    "command": f"ip -4 addr show dev {FRR_IF}",
                    "contains": [f"{FRR_LINK_V4}/30"],
                    "label": "f1 eth1 has IPv4 address",
                },
                {
                    "device": "f1",
                    "command": f"ping -c 1 -W 2 {R2_BC_LINK_V4}",
                    "contains": ["1 received"],
                    "label": "f1 can ping r2 link IP",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure ISIS and LDP on r1/r2/f1")
        _configure_netnexus(
            rt,
            device="r1",
            net=R1_NET,
            lsr_id=R1_LSR_ID,
            loop_id=R1_LOOP_ID,
            loop_v4=R1_LOOP_V4,
            ifaces=(R1_R2_IF,),
        )
        _configure_netnexus(
            rt,
            device="r2",
            net=R2_NET,
            lsr_id=R2_LSR_ID,
            loop_id=R2_LOOP_ID,
            loop_v4=R2_LOOP_V4,
            ifaces=(R2_R1_IF, R2_F1_IF),
        )
        _configure_frr(rt)

        step("Wait ISIS convergence for f1 loopback reachability")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis neighbor {TAG}",
                    "contains": [R1_R2_IF, "Up"],
                    "label": "r1 sees r2 ISIS up",
                },
                {
                    "device": "r2",
                    "command": f"show isis neighbor {TAG}",
                    "contains": [R2_R1_IF, R2_F1_IF, "Up"],
                    "label": "r2 sees r1 and f1 ISIS up",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show isis neighbor'",
                    "regex": [rf"(?im)^\s*\S+\s+{re.escape(FRR_IF)}\s+[12]\s+Up\b"],
                    "label": "FRR sees r2 ISIS up",
                },
                {
                    "device": "r1",
                    "command": "show route ipv4 proto isis",
                    "contains": [f"{FRR_LOOP_V4}/{LOOP_V4_LEN}"],
                    "label": "r1 has f1 loopback via ISIS",
                },
                {
                    "device": "r2",
                    "command": "show route ipv4 proto isis",
                    "contains": [f"{FRR_LOOP_V4}/{LOOP_V4_LEN}"],
                    "label": "r2 has f1 loopback via ISIS",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show ip route isis'",
                    "contains": [f"{R1_LOOP_V4}/{LOOP_V4_LEN}", f"{R2_LOOP_V4}/{LOOP_V4_LEN}"],
                    "label": "f1 has NetNexus loopbacks via ISIS",
                },
            ],
            timeout=120,
            interval=3,
        )

        step("Wait LDP sessions r1-r2 and r2-f1 OPERATIONAL")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show ldp neighbor",
                    "contains": [R2_LSR_ID, R1_R2_IF, R2_AB_LINK_V4, "OPERATIONAL"],
                    "label": "r1 sees r2 LDP operational",
                },
                {
                    "device": "r2",
                    "command": "show ldp neighbor",
                    "contains": [R1_LSR_ID, R2_R1_IF, R1_LINK_V4, FRR_LSR_ID, R2_F1_IF, FRR_LINK_V4, "OPERATIONAL"],
                    "label": "r2 sees r1 and f1 LDP operational",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show mpls ldp neighbor'",
                    "contains": ["ipv4", R2_LSR_ID, R2_BC_LINK_V4, "OPERATIONAL"],
                    "label": "FRR sees r2 LDP operational",
                },
            ],
            timeout=90,
            interval=2,
        )

        step("Verify label chain: f1 imp-null, r2 advertises normal label to r1")
        wait_checks(
            rt,
            [
                {
                    "device": "f1",
                    "command": "vtysh -c 'show mpls ldp binding'",
                    "regex": [rf"(?im)^ipv4\s+{re.escape(FRR_LOOP_V4)}/{LOOP_V4_LEN}\s+.*\s+imp-null\s+"],
                    "label": "FRR advertises f1 loopback as implicit-null",
                },
                {
                    "device": "r2",
                    "command": "show ldp binding",
                    "regex": [
                        rf"(?im)^\s*{re.escape(FRR_LOOP_V4)}/{LOOP_V4_LEN}\s+[1-9]\d+\s*$",
                        rf"(?im)^\s*{re.escape(FRR_LSR_ID)}\s+{re.escape(FRR_LOOP_V4)}/{LOOP_V4_LEN}\s+3\s*$",
                    ],
                    "not_contains": ["No LDP label binding"],
                    "label": "r2 has local normal label and remote imp-null for f1 loopback",
                },
                {
                    "device": "r1",
                    "command": "show ldp binding",
                    "regex": [
                        rf"(?im)^\s*{re.escape(R2_LSR_ID)}\s+{re.escape(FRR_LOOP_V4)}/{LOOP_V4_LEN}\s+[1-9]\d+\s*$"
                    ],
                    "not_contains": ["No LDP label binding"],
                    "label": "r1 received normal label for f1 loopback from r2",
                },
            ],
            timeout=90,
            interval=3,
        )

        step("Verify NetNexus tunnel programming for f1 loopback while IP FIB remains plain")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show tunnel ftn",
                    "regex": [
                        rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+fec\s+{re.escape(FRR_LOOP_V4)}/{LOOP_V4_LEN}\s+"
                        r"->\s+nhlfe\s+[1-9]\d*\s+src\s+ldp\s+state\s+up\s*$"
                    ],
                    "label": "r1 FTN maps f1 loopback to LDP NHLFE",
                },
                {
                    "device": "r1",
                    "command": f"show fib ipv4 {FRR_LOOP_V4} {LOOP_V4_LEN}",
                    "contains": [
                        f"FIB Route Detail: {FRR_LOOP_V4}/{LOOP_V4_LEN}",
                        "NH-Type   : ip",
                    ],
                    "not_contains": ["NH-Type   : tunnel", "Tunnel    : state=up"],
                    "label": "r1 FIB route remains a plain IP route",
                },
                {
                    "device": "r2",
                    "command": "show tunnel ilm",
                    "regex": [
                        r"(?im)^\s*vrf\s+0\s+label\s+[1-9]\d+\s+->\s+nhlfe\s+[1-9]\d*\s+action\s+"
                        r"(pop|swap)\(\d+\)\s+state\s+up\s*$"
                    ],
                    "label": "r2 ILM is ready for the label advertised to r1",
                },
            ],
            timeout=60,
            interval=2,
        )

        step("Capture r1->f1 MPLS ping: r1-r2 MPLS, r2-f1 IPv4 after PHP")
        f1_mac = _docker_exec_ok(rt, "f1", f"cat /sys/class/net/{FRR_IF}/address", timeout=3).strip()
        _capture_start(
            rt,
            device="r2",
            name="ldp_isis_php_ab_mpls",
            iface=R2_AB_LINUX_IF,
            packet_filter="mpls",
            seconds=10,
        )
        _capture_start(
            rt,
            device="f1",
            name="ldp_isis_php_bc_icmp",
            iface=FRR_IF,
            packet_filter=f"icmp and host {FRR_LOOP_V4}",
            seconds=10,
        )
        _capture_start(
            rt,
            device="f1",
            name="ldp_isis_php_bc_mpls",
            iface=FRR_IF,
            packet_filter=f"ether dst {f1_mac} and mpls",
            seconds=10,
        )
        _docker_exec_ok(rt, "r2", "sleep 1", timeout=3)

        ping_out = rt.exec_cmd("r1", f"ping mpls ipv4 {FRR_LOOP_V4}/{LOOP_V4_LEN} -a {R1_LOOP_V4}", timeout=20)
        if "0% packet loss" not in ping_out or "100% packet loss" in ping_out:
            raise AssertionError(f"r1 MPLS loopback ping to f1 loopback failed:\n{ping_out}")

        ab_mpls = _capture_wait(rt, device="r2", name="ldp_isis_php_ab_mpls", expect_packet=True)
        if "MPLS" not in ab_mpls:
            raise AssertionError(f"r1-r2 capture did not decode MPLS:\n{ab_mpls}")

        bc_icmp = _capture_wait(rt, device="f1", name="ldp_isis_php_bc_icmp", expect_packet=True)
        if "ICMP" not in bc_icmp and "icmp" not in bc_icmp:
            raise AssertionError(f"r2-f1 capture did not decode ICMP:\n{bc_icmp}")

        _capture_wait(rt, device="f1", name="ldp_isis_php_bc_mpls", expect_packet=False)

        print("NetNexus/FRR LDP ISIS PHP traffic check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
