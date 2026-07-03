#!/usr/bin/env python3
"""
SNMP framework and interface trap check.

Coverage:
1. `snmpwalk` can read system MIB top information from the SNMP module.
2. CLI configures `snmp trap server <server> [port <port>]` in the SNMP module and BDR exports it.
3. IF link events emit standard linkDown/linkUp trap packets to the configured receiver.
4. BGP neighbor up/down emits standard BGP4-MIB notification packets.
5. ISIS neighbor up/down emits standard ISIS-MIB adjacency-change notification packets.
6. `no snmp trap server` removes the configuration.
"""

from __future__ import annotations

import re
import subprocess
import time
from pathlib import PurePosixPath

from module_api import cmd, g_top, mark_step_failed, require_devices, run_cmds, step  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


IF_TRAP_PORT = 55162
BGP_TRAP_PORT = 55163
ISIS_TRAP_PORT = 55164
SNMP_TRAP_FILE_PREFIX = "/tmp/netnexus-snmp-trap-ci"
LINK_DOWN_OID_HEX = "06092b0601060301010503"
LINK_UP_OID_HEX = "06092b0601060301010504"
BGP_ESTABLISHED_OID_HEX = "06082b060102010f0001"
BGP_BACKWARD_OID_HEX = "06082b060102010f0002"
ISIS_ADJ_CHANGE_OID_HEX = "06092b06010201810a0011"
BGP_R1_AS = 65001
BGP_R2_AS = 65002
ISIS_TAG = 1


def _docker_exec(container: str, command: str, *, timeout: int = 10, check: bool = True) -> str:
    proc = subprocess.run(
        ["docker", "exec", container, "sh", "-lc", command],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    if check and proc.returncode != 0:
        raise RuntimeError(f"docker exec failed rc={proc.returncode}: {command}\n{proc.stderr}\n{proc.stdout}")
    return proc.stdout


def _trap_file(port: int) -> str:
    return f"{SNMP_TRAP_FILE_PREFIX}-{port}.log"


def _read_trap_log(container: str, port: int) -> str:
    path = PurePosixPath(_trap_file(port))
    return _docker_exec(container, f"cat {path}", check=False)


def _start_trap_capture(container: str, port: int, *, max_packets: int = 8) -> subprocess.Popen[str]:
    trap_file = _trap_file(port)
    _docker_exec(container, f"rm -f {trap_file}", check=False)
    command = (
        f"timeout 60 tcpdump -l -ni any -XX -s 0 "
        f"'udp dst port {port}' -c {max_packets} > {trap_file} 2>&1"
    )
    proc = subprocess.Popen(
        ["docker", "exec", container, "sh", "-lc", command],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            _, err = proc.communicate(timeout=1)
            out = _read_trap_log(container, port)
            raise RuntimeError(f"trap capture exited early rc={proc.returncode}: {err}\n{out}")
        if "listening on" in _read_trap_log(container, port):
            return proc
        time.sleep(0.1)
    proc.terminate()
    raise RuntimeError("trap capture did not become ready")


def _stop_trap_capture(container: str, proc: subprocess.Popen[str], port: int) -> None:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
    _docker_exec(container, f"pkill -f 'tcpdump.*{port}' || true", check=False)


def _compact_tcpdump_hex(text: str) -> str:
    chunks: list[str] = []
    for line in text.splitlines():
        m = re.match(r"^\s*0x[0-9a-fA-F]+:\s+([0-9a-fA-F ]+?)(?:\s{2,}.*)?$", line)
        if m:
            chunks.append(re.sub(r"[^0-9a-fA-F]", "", m.group(1)).lower())
    return "".join(chunks)


def _wait_snmpwalk_system(container: str, *, timeout: float = 30.0) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    command = "snmpwalk -v2c -c public -On -t 2 -r 1 127.0.0.1 .1.3.6.1.2.1.1"
    while time.monotonic() < deadline:
        last = _docker_exec(container, command, timeout=8, check=False)
        if ".1.3.6.1.2.1.1.1.0" in last and "NetNexus SNMP agent" in last:
            return last
        time.sleep(1)
    mark_step_failed()
    raise AssertionError(f"timeout waiting for SNMP system walk; last output:\n{last}")


def _wait_trap_packet(
    container: str,
    proc: subprocess.Popen[str],
    port: int,
    expected_oid_hexes: tuple[str, ...],
    *,
    timeout: float = 45.0,
) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        last = _read_trap_log(container, port)
        compact = _compact_tcpdump_hex(last)
        if any(oid_hex in compact for oid_hex in expected_oid_hexes):
            return last
        if proc.poll() is not None:
            mark_step_failed()
            raise AssertionError(f"trap capture ended before expected OID {expected_oid_hexes!r} appeared:\n{last}")
        time.sleep(0.5)
    mark_step_failed()
    raise AssertionError(f"timeout waiting for SNMP trap OID {expected_oid_hexes!r}; current capture:\n{last}")


def _cleanup(rt: TopologyRuntime) -> None:
    for dev in ("r1", "r2"):
        run_cmds(
            rt,
            dev,
            strict=False,
            commands=[
                "end",
                "config",
                "no bgp",
                f"no isis {ISIS_TAG}",
                "if GE-1",
                "no shutdown",
                f"no isis enable {ISIS_TAG}",
                "exit",
                "no snmp trap server",
                "end",
            ],
        )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_container = rt.container_name("r1")
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)
    capture: subprocess.Popen[str] | None = None
    capture_port: int | None = None

    try:
        _cleanup(rt)

        step("Configure SNMP trap receiver on r1")
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"snmp trap server 127.0.0.1 port {IF_TRAP_PORT}",
                "end",
            ],
        )
        current = cmd(rt, "r1", "show current-configuration")
        expected = f"snmp trap server 127.0.0.1 port {IF_TRAP_PORT}"
        if expected not in current:
            raise AssertionError(f"SNMP trap config missing from current configuration:\n{current}")

        step("Verify SNMP system walk")
        walk = _wait_snmpwalk_system(r1_container)
        if ".1.3.6.1.2.1.1.5.0" not in walk:
            raise AssertionError(f"SNMP system walk missing sysName:\n{walk}")

        step("Verify IF link trap packet")
        capture_port = IF_TRAP_PORT
        capture = _start_trap_capture(r1_container, capture_port)
        for _ in range(3):
            run_cmds(
                rt,
                "r1",
                strict=False,
                commands=[
                    "config",
                    "if GE-1",
                    "shutdown",
                    "no shutdown",
                    "end",
                ],
            )
            time.sleep(1)
            if capture.poll() is not None:
                break
        _wait_trap_packet(r1_container, capture, capture_port, (LINK_DOWN_OID_HEX, LINK_UP_OID_HEX))
        _stop_trap_capture(r1_container, capture, capture_port)
        capture = None
        capture_port = None

        step("Verify BGP neighbor up trap packet")
        run_cmds(rt, "r1", commands=["config", f"snmp trap server 127.0.0.1 port {BGP_TRAP_PORT}", "end"])
        capture_port = BGP_TRAP_PORT
        capture = _start_trap_capture(r1_container, capture_port)
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"bgp {BGP_R1_AS}",
                "router-id 1.1.1.1",
                f"neighbor {r1_peer_ip} as {BGP_R2_AS}",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "end",
            ],
        )
        run_cmds(
            rt,
            "r2",
            commands=[
                "config",
                f"bgp {BGP_R2_AS}",
                "router-id 2.2.2.2",
                f"neighbor {r2_peer_ip} as {BGP_R1_AS}",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "end",
            ],
        )
        _wait_trap_packet(r1_container, capture, capture_port, (BGP_ESTABLISHED_OID_HEX,), timeout=60)
        _stop_trap_capture(r1_container, capture, capture_port)
        capture = None
        capture_port = None

        step("Verify BGP neighbor down trap packet")
        capture_port = BGP_TRAP_PORT
        capture = _start_trap_capture(r1_container, capture_port)
        run_cmds(rt, "r1", commands=["config", "no bgp", "end"])
        _wait_trap_packet(r1_container, capture, capture_port, (BGP_BACKWARD_OID_HEX,), timeout=45)
        _stop_trap_capture(r1_container, capture, capture_port)
        capture = None
        capture_port = None
        run_cmds(rt, "r2", strict=False, commands=["config", "no bgp", "end"])

        step("Verify ISIS neighbor up trap packet")
        run_cmds(rt, "r1", commands=["config", f"snmp trap server 127.0.0.1 port {ISIS_TRAP_PORT}", "end"])
        capture_port = ISIS_TRAP_PORT
        capture = _start_trap_capture(r1_container, capture_port)
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"isis {ISIS_TAG}",
                "net 49.0001.0000.0000.0001.00",
                "cost-style wide",
                "af ipv4",
                "exit",
                "if GE-1",
                f"isis enable {ISIS_TAG}",
                "end",
            ],
        )
        run_cmds(
            rt,
            "r2",
            commands=[
                "config",
                f"isis {ISIS_TAG}",
                "net 49.0001.0000.0000.0002.00",
                "cost-style wide",
                "af ipv4",
                "exit",
                "if GE-1",
                f"isis enable {ISIS_TAG}",
                "end",
            ],
        )
        _wait_trap_packet(r1_container, capture, capture_port, (ISIS_ADJ_CHANGE_OID_HEX,), timeout=60)
        _stop_trap_capture(r1_container, capture, capture_port)
        capture = None
        capture_port = None

        step("Verify ISIS neighbor down trap packet")
        capture_port = ISIS_TRAP_PORT
        capture = _start_trap_capture(r1_container, capture_port)
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                "if GE-1",
                f"no isis enable {ISIS_TAG}",
                "end",
            ],
        )
        _wait_trap_packet(r1_container, capture, capture_port, (ISIS_ADJ_CHANGE_OID_HEX,), timeout=45)
        _stop_trap_capture(r1_container, capture, capture_port)
        capture = None
        capture_port = None

        step("Disable SNMP trap receiver")
        run_cmds(rt, "r1", commands=["config", "no snmp trap server", "end"])
        current = cmd(rt, "r1", "show current-configuration")
        if "snmp trap server" in current:
            raise AssertionError(f"SNMP trap config should be removed:\n{current}")

        print("SNMP reporting check passed.")
    finally:
        _cleanup(rt)
        if capture is not None and capture_port is not None:
            _stop_trap_capture(r1_container, capture, capture_port)
