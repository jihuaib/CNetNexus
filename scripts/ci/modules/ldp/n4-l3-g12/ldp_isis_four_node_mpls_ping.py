#!/usr/bin/env python3
"""
Four-node NetNexus LDP/ISIS MPLS data-plane validation.

Topology:
  r1 -- GE-1/GE-1 -- r2 -- GE-2/GE-1 -- r3 -- GE-2/GE-1 -- r4

Coverage:
- ISIS advertises loopback host routes across a three-hop path.
- LDP programs an ingress FTN on r1, a SWAP ILM on r2, and a POP ILM on r3.
- r1 explicit `ping mpls` reaches r4 loopback.
- Packet captures prove r2 forwards MPLS onward and r3 performs PHP before r4.
"""

from __future__ import annotations

import re
import shlex
import subprocess
import time

from module_api import (  # noqa: E402
    g_top,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


TAG = 1

R1_R2_IF = "GE-1"
R2_R1_IF = "GE-1"
R2_R3_IF = "GE-2"
R3_R2_IF = "GE-1"
R3_R4_IF = "GE-2"
R4_R3_IF = "GE-1"

R2_R3_LINUX_IF = "eth2"
R4_R3_LINUX_IF = "eth1"

R1_NET = "49.0001.0000.0000.0001.00"
R2_NET = "49.0001.0000.0000.0002.00"
R3_NET = "49.0001.0000.0000.0003.00"
R4_NET = "49.0001.0000.0000.0004.00"

R1_LSR_ID = "1.1.1.1"
R2_LSR_ID = "2.2.2.2"
R3_LSR_ID = "3.3.3.3"
R4_LSR_ID = "4.4.4.4"

R1_LOOP_ID = 11
R2_LOOP_ID = 22
R3_LOOP_ID = 33
R4_LOOP_ID = 44
R1_LOOP_V4 = R1_LSR_ID
R2_LOOP_V4 = R2_LSR_ID
R3_LOOP_V4 = R3_LSR_ID
R4_LOOP_V4 = R4_LSR_ID
LOOP_V4_LEN = 32

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
    step("Cleanup LDP/ISIS/loopback config")
    for dev, ifaces, loop in (
        ("r1", (R1_R2_IF,), R1_LOOP_ID),
        ("r2", (R2_R1_IF, R2_R3_IF), R2_LOOP_ID),
        ("r3", (R3_R2_IF, R3_R4_IF), R3_LOOP_ID),
        ("r4", (R4_R3_IF,), R4_LOOP_ID),
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

    for dev in ("r2", "r4"):
        _docker_exec(rt, dev, "rm -f /tmp/ldp_isis_four_node_*.tcpdump /tmp/ldp_isis_four_node_*.rc", timeout=5)


def _configure_router(
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


def _wait_baseline_links(rt: TopologyRuntime) -> None:
    checks = []
    for dev, ifname, ip, prefix in (
        ("r1", R1_R2_IF, str(g_top.r1.GE_1.ip), int(g_top.r1.GE_1.prefix)),
        ("r2", R2_R1_IF, str(g_top.r2.GE_1.ip), int(g_top.r2.GE_1.prefix)),
        ("r2", R2_R3_IF, str(g_top.r2.GE_2.ip), int(g_top.r2.GE_2.prefix)),
        ("r3", R3_R2_IF, str(g_top.r3.GE_1.ip), int(g_top.r3.GE_1.prefix)),
        ("r3", R3_R4_IF, str(g_top.r3.GE_2.ip), int(g_top.r3.GE_2.prefix)),
        ("r4", R4_R3_IF, str(g_top.r4.GE_1.ip), int(g_top.r4.GE_1.prefix)),
    ):
        checks.append(
            {
                "device": dev,
                "command": f"show if {ifname}",
                "contains": [f"Interface {ifname} Detail:", "State      : UP", f"IPv4 Addr  : {ip}/{prefix}"],
                "label": f"{dev} {ifname} up",
            }
        )
    wait_checks(rt, checks, timeout=30, interval=2)


def _wait_isis_and_ldp(rt: TopologyRuntime) -> None:
    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": f"show isis neighbor {TAG}",
                "contains": [R1_R2_IF, "Up"],
                "label": "r1 ISIS neighbor r2 up",
            },
            {
                "device": "r2",
                "command": f"show isis neighbor {TAG}",
                "contains": [R2_R1_IF, R2_R3_IF, "Up"],
                "label": "r2 ISIS neighbors r1/r3 up",
            },
            {
                "device": "r3",
                "command": f"show isis neighbor {TAG}",
                "contains": [R3_R2_IF, R3_R4_IF, "Up"],
                "label": "r3 ISIS neighbors r2/r4 up",
            },
            {
                "device": "r4",
                "command": f"show isis neighbor {TAG}",
                "contains": [R4_R3_IF, "Up"],
                "label": "r4 ISIS neighbor r3 up",
            },
            {
                "device": "r1",
                "command": "show route ipv4 proto isis",
                "contains": [f"{R4_LOOP_V4}/{LOOP_V4_LEN}"],
                "label": "r1 learned r4 loopback via ISIS",
            },
            {
                "device": "r2",
                "command": "show route ipv4 proto isis",
                "contains": [f"{R4_LOOP_V4}/{LOOP_V4_LEN}"],
                "label": "r2 learned r4 loopback via ISIS",
            },
            {
                "device": "r3",
                "command": "show route ipv4 proto isis",
                "contains": [f"{R4_LOOP_V4}/{LOOP_V4_LEN}"],
                "label": "r3 learned r4 loopback via ISIS",
            },
            {
                "device": "r4",
                "command": "show route ipv4 proto isis",
                "contains": [f"{R1_LOOP_V4}/{LOOP_V4_LEN}"],
                "label": "r4 learned r1 loopback via ISIS",
            },
        ],
        timeout=120,
        interval=3,
    )

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show ldp neighbor",
                "contains": [R2_LSR_ID, R1_R2_IF, "OPERATIONAL"],
                "label": "r1 sees r2 LDP operational",
            },
            {
                "device": "r2",
                "command": "show ldp neighbor",
                "contains": [R1_LSR_ID, R2_R1_IF, R3_LSR_ID, R2_R3_IF, "OPERATIONAL"],
                "label": "r2 sees r1/r3 LDP operational",
            },
            {
                "device": "r3",
                "command": "show ldp neighbor",
                "contains": [R2_LSR_ID, R3_R2_IF, R4_LSR_ID, R3_R4_IF, "OPERATIONAL"],
                "label": "r3 sees r2/r4 LDP operational",
            },
            {
                "device": "r4",
                "command": "show ldp neighbor",
                "contains": [R3_LSR_ID, R4_R3_IF, "OPERATIONAL"],
                "label": "r4 sees r3 LDP operational",
            },
        ],
        timeout=90,
        interval=2,
    )


def _wait_mpls_programming(rt: TopologyRuntime) -> None:
    r1_to_r2 = str(g_top.r1.GE_1.peer_ip)
    r2_to_r3 = str(g_top.r2.GE_2.peer_ip)
    r3_to_r4 = str(g_top.r3.GE_2.peer_ip)

    wait_checks(
        rt,
        [
            {
                "device": "r1",
                "command": "show ldp binding",
                "regex": [
                    rf"(?im)^\s*{re.escape(R2_LSR_ID)}\s+{re.escape(R4_LOOP_V4)}/{LOOP_V4_LEN}\s+[1-9]\d+\s*$"
                ],
                "not_contains": ["No LDP label binding"],
                "label": "r1 has r2 label for r4 loopback",
            },
            {
                "device": "r2",
                "command": "show ldp binding",
                "regex": [
                    rf"(?im)^\s*{re.escape(R3_LSR_ID)}\s+{re.escape(R4_LOOP_V4)}/{LOOP_V4_LEN}\s+[1-9]\d+\s*$"
                ],
                "not_contains": ["No LDP label binding"],
                "label": "r2 has r3 label for r4 loopback",
            },
            {
                "device": "r3",
                "command": "show ldp binding",
                "regex": [
                    rf"(?im)^\s*{re.escape(R4_LSR_ID)}\s+{re.escape(R4_LOOP_V4)}/{LOOP_V4_LEN}\s+3\s*$"
                ],
                "not_contains": ["No LDP label binding"],
                "label": "r3 has r4 implicit-null for r4 loopback",
            },
            {
                "device": "r1",
                "command": "show tunnel ftn",
                "regex": [
                    rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+fec\s+{re.escape(R4_LOOP_V4)}/{LOOP_V4_LEN}\s+"
                    r"->\s+nhlfe\s+[1-9]\d*\s+src\s+ldp\s+state\s+up\s*$"
                ],
                "label": "r1 FTN maps r4 loopback to LDP NHLFE",
            },
            {
                "device": "r1",
                "command": "show tunnel nhlfe",
                "regex": [
                    rf"(?im)^\s*id\s+[1-9]\d*\s+endpoint\s+{re.escape(R4_LOOP_V4)}\s+relay\s+"
                    rf"{re.escape(r1_to_r2)}\s+oif\s+\d+\s+src\s+ldp\s+labels\s+\[[1-9]\d*\]\s*$"
                ],
                "label": "r1 NHLFE pushes label toward r2",
            },
            {
                "device": "r2",
                "command": "show tunnel candidate",
                "regex": [
                    rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+endpoint\s+{re.escape(R4_LOOP_V4)}\s+nh\s+"
                    rf"{re.escape(r2_to_r3)}\s+relay\s+{re.escape(r2_to_r3)}\s+oif\s+\d+\s+src\s+ldp\s+"
                    r"pref\s+\d+\s+labels\s+\[[1-9]\d*\]\s*$"
                ],
                "label": "r2 candidate swaps toward r3",
            },
            {
                "device": "r2",
                "command": "show tunnel ilm",
                "regex": [
                    r"(?im)^\s*vrf\s+0\s+label\s+[1-9]\d+\s+->\s+nhlfe\s+[1-9]\d*\s+"
                    r"action\s+swap\(2\)\s+state\s+up\s*$"
                ],
                "label": "r2 tunnel ILM SWAP programmed",
            },
            {
                "device": "r2",
                "command": "show fib mpls",
                "regex": [
                    rf"(?im)^\s*0\s+[1-9]\d*\s+swap\s+[1-9]\d*\s+up\s+\d+\s+"
                    rf"{re.escape(r2_to_r3)}\s+[1-9]\d*\s+yes\s+yes\s*$"
                ],
                "label": "r2 FIB MPLS SWAP installed",
            },
            {
                "device": "r2",
                "command": "show fib mpls os",
                "regex": [
                    rf"(?im)^\s*main\s+unicast\s+[1-9]\d*\s+{re.escape(r2_to_r3)}\s+\S+\s+"
                    r"static\s+\d+\s+swap\[[1-9]\d*\]\s*$"
                ],
                "label": "r2 Linux MPLS route swaps label",
            },
            {
                "device": "r3",
                "command": "show tunnel candidate",
                "regex": [
                    rf"(?im)^\s*vrf\s+0\s+afi\s+1\s+endpoint\s+{re.escape(R4_LOOP_V4)}\s+nh\s+"
                    rf"{re.escape(r3_to_r4)}\s+relay\s+{re.escape(r3_to_r4)}\s+oif\s+\d+\s+src\s+ldp\s+"
                    r"pref\s+\d+\s+labels\s+\[\]\s*$"
                ],
                "label": "r3 candidate is PHP toward r4",
            },
            {
                "device": "r3",
                "command": "show tunnel ilm",
                "regex": [
                    r"(?im)^\s*vrf\s+0\s+label\s+[1-9]\d+\s+->\s+nhlfe\s+[1-9]\d*\s+"
                    r"action\s+pop\(3\)\s+state\s+up\s*$"
                ],
                "label": "r3 tunnel ILM POP programmed",
            },
            {
                "device": "r3",
                "command": "show fib mpls",
                "regex": [
                    rf"(?im)^\s*0\s+[1-9]\d*\s+pop\s+[1-9]\d*\s+up\s+\d+\s+"
                    rf"{re.escape(r3_to_r4)}\s+-\s+yes\s+yes\s*$"
                ],
                "label": "r3 FIB MPLS POP installed",
            },
            {
                "device": "r3",
                "command": "show fib mpls os",
                "regex": [
                    rf"(?im)^\s*main\s+unicast\s+[1-9]\d*\s+{re.escape(r3_to_r4)}\s+\S+\s+"
                    r"static\s+\d+\s+pop\s*$"
                ],
                "label": "r3 Linux MPLS route pops label",
            },
        ],
        timeout=90,
        interval=3,
    )


def _assert_mpls_ping_and_captures(rt: TopologyRuntime) -> None:
    r4_mac = _docker_exec_ok(rt, "r4", f"cat /sys/class/net/{R4_R3_LINUX_IF}/address", timeout=3).strip()

    _capture_start(
        rt,
        device="r2",
        name="ldp_isis_four_node_r2_r3_mpls",
        iface=R2_R3_LINUX_IF,
        packet_filter="mpls",
        seconds=10,
    )
    _capture_start(
        rt,
        device="r4",
        name="ldp_isis_four_node_r3_r4_icmp",
        iface=R4_R3_LINUX_IF,
        packet_filter=f"icmp and host {R4_LOOP_V4}",
        seconds=10,
    )
    _capture_start(
        rt,
        device="r4",
        name="ldp_isis_four_node_r3_r4_mpls",
        iface=R4_R3_LINUX_IF,
        packet_filter=f"ether dst {r4_mac} and mpls",
        seconds=10,
    )
    _docker_exec_ok(rt, "r2", "sleep 1", timeout=3)

    out = rt.exec_cmd("r1", f"ping mpls ipv4 {R4_LOOP_V4}/{LOOP_V4_LEN} -a {R1_LOOP_V4}", timeout=20)
    if "0% packet loss" not in out or "100% packet loss" in out:
        raise AssertionError(f"r1 MPLS ping to r4 loopback failed:\n{out}")

    r2_r3_mpls = _capture_wait(rt, device="r2", name="ldp_isis_four_node_r2_r3_mpls", expect_packet=True)
    if "MPLS" not in r2_r3_mpls:
        raise AssertionError(f"r2-r3 capture did not decode MPLS:\n{r2_r3_mpls}")

    r3_r4_icmp = _capture_wait(rt, device="r4", name="ldp_isis_four_node_r3_r4_icmp", expect_packet=True)
    if "ICMP" not in r3_r4_icmp and "icmp" not in r3_r4_icmp:
        raise AssertionError(f"r3-r4 capture did not decode ICMP:\n{r3_r4_icmp}")

    _capture_wait(rt, device="r4", name="ldp_isis_four_node_r3_r4_mpls", expect_packet=False)


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2", "r3", "r4"))

    try:
        _cleanup(rt)

        step("Ensure baseline links are up")
        _wait_baseline_links(rt)

        step("Configure ISIS and LDP on r1/r2/r3/r4")
        _configure_router(
            rt,
            device="r1",
            net=R1_NET,
            lsr_id=R1_LSR_ID,
            loop_id=R1_LOOP_ID,
            loop_v4=R1_LOOP_V4,
            ifaces=(R1_R2_IF,),
        )
        _configure_router(
            rt,
            device="r2",
            net=R2_NET,
            lsr_id=R2_LSR_ID,
            loop_id=R2_LOOP_ID,
            loop_v4=R2_LOOP_V4,
            ifaces=(R2_R1_IF, R2_R3_IF),
        )
        _configure_router(
            rt,
            device="r3",
            net=R3_NET,
            lsr_id=R3_LSR_ID,
            loop_id=R3_LOOP_ID,
            loop_v4=R3_LOOP_V4,
            ifaces=(R3_R2_IF, R3_R4_IF),
        )
        _configure_router(
            rt,
            device="r4",
            net=R4_NET,
            lsr_id=R4_LSR_ID,
            loop_id=R4_LOOP_ID,
            loop_v4=R4_LOOP_V4,
            ifaces=(R4_R3_IF,),
        )

        step("Wait ISIS/LDP convergence")
        _wait_isis_and_ldp(rt)

        step("Verify MPLS tunnel, FIB, and Linux kernel programming")
        _wait_mpls_programming(rt)

        step("Verify MPLS ping crosses r2 as MPLS and exits r3 as IPv4")
        _assert_mpls_ping_and_captures(rt)

        print("Four-node LDP ISIS MPLS ping data-plane check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
