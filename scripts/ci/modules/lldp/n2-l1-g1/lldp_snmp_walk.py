#!/usr/bin/env python3
"""
LLDP-MIB SNMP walk topology coverage.

Topology: r1 --- GE-1 --- r2

Coverage:
- LLDP module publishes local LLDP-MIB system and port data to SNMP.
- LLDP remote neighbor table exposes learned peer topology information.
"""

from __future__ import annotations

import subprocess
import time
import re
from pathlib import PurePosixPath

from module_api import cmd, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"
R1_SYSNAME = "LLDPSNMP-R1"
R2_SYSNAME = "LLDPSNMP-R2"
PORT_DESC_R1 = "snmp-r1-to-r2"
PORT_DESC_R2 = "snmp-r2-to-r1"
SNMP_TRAP_PORT = 55166
SYSLOG_PORT = 55167
SYSLOG_FILE = "/tmp/netnexus-lldp-syslog-ci.log"
SYSLOG_MAX_MESSAGES = 40
LLDP_MIB_ROOT = ".1.0.8802.1.1.2"
LLDP_LOC_SYS_NAME = ".1.0.8802.1.1.2.1.3.3.0"
LLDP_LOC_PORT_ID_PREFIX = ".1.0.8802.1.1.2.1.3.7.1.3"
LLDP_REM_PORT_ID_PREFIX = ".1.0.8802.1.1.2.1.4.1.1.7"
LLDP_REM_PORT_DESC_PREFIX = ".1.0.8802.1.1.2.1.4.1.1.8"
LLDP_REM_SYS_NAME_PREFIX = ".1.0.8802.1.1.2.1.4.1.1.9"
IF_MIB_ROOT = ".1.3.6.1.2.1"
IF_NAME_PREFIX = ".1.3.6.1.2.1.31.1.1.1.1"
IF_DESCR_PREFIX = ".1.3.6.1.2.1.2.2.1.2"
IF_ADMIN_STATUS_PREFIX = ".1.3.6.1.2.1.2.2.1.7"
IF_OPER_STATUS_PREFIX = ".1.3.6.1.2.1.2.2.1.8"


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
    if proc.returncode != 0 and proc.stderr:
        return f"{proc.stdout}\n[stderr]\n{proc.stderr}"
    return proc.stdout


def _start_udp_collector(container: str) -> subprocess.Popen[str]:
    _docker_exec(container, f"rm -f {SYSLOG_FILE}", check=False)
    command = (
        f"timeout 90 tcpdump -l -ni any -A -s 0 "
        f"'udp dst port {SYSLOG_PORT}' -c {SYSLOG_MAX_MESSAGES} > {SYSLOG_FILE} 2>&1"
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
            out = _read_syslog(container)
            raise RuntimeError(f"syslog collector exited early rc={proc.returncode}: {err}\n{out}")
        out = _read_syslog(container)
        if "listening on" in out:
            return proc
        time.sleep(0.1)
    proc.terminate()
    raise RuntimeError("syslog collector did not become ready")


def _stop_collector(container: str, proc: subprocess.Popen[str] | None) -> None:
    if not proc:
        return
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3)
    _docker_exec(container, f"pkill -f 'tcpdump.*{SYSLOG_PORT}' || true", check=False)


def _read_syslog(container: str) -> str:
    path = PurePosixPath(SYSLOG_FILE)
    return _docker_exec(container, f"cat {path}", check=False)


def _wait_syslog(container: str, needles: tuple[str, ...], *, timeout: float = 45.0) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        last = _read_syslog(container)
        if all(needle in last for needle in needles):
            return last
        time.sleep(0.5)
    raise AssertionError(f"timeout waiting LLDP syslog needles {needles!r}; current syslog:\n{last}")


def _cleanup(rt: TopologyRuntime, *, restore_topology_sysname: bool = False) -> None:
    step("Cleanup LLDP/SNMP config")
    for dev in ("r1", "r2"):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"if {GE_IF}",
                "no lldp port-description",
                "no lldp admin-status",
                "lldp enable",
                "exit",
                "no lldp hold-multiplier",
                "no lldp timer",
                "no lldp",
                "no snmp trap server",
                "no syslog server",
                "no sysname",
                "end",
            ],
        )
        if restore_topology_sysname:
            run_cmds(rt=rt, device=dev, strict=False, commands=["end", "config", f"sysname {dev}", "end"])


def _configure(rt: TopologyRuntime) -> None:
    configs = {
        "r1": (R1_SYSNAME, PORT_DESC_R1),
        "r2": (R2_SYSNAME, PORT_DESC_R2),
    }
    for dev, (sysname, port_desc) in configs.items():
        run_cmds(
            rt=rt,
            device=dev,
            commands=[
                "config",
                f"sysname {sysname}",
                "lldp",
                "lldp timer 5",
                "lldp hold-multiplier 2",
                f"if {GE_IF}",
                "lldp enable",
                "lldp admin-status txrx",
                f"lldp port-description {port_desc}",
                "exit",
                "end",
            ],
        )


def _wait_snmpwalk_lldp(container: str, *, timeout: float = 60.0) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    command = f"snmpwalk -v2c -c public -On -t 2 -r 1 127.0.0.1 {LLDP_MIB_ROOT}"
    while time.monotonic() < deadline:
        last = _docker_exec(container, command, timeout=8, check=False)
        if (
            LLDP_LOC_SYS_NAME in last
            and R1_SYSNAME in last
            and LLDP_REM_SYS_NAME_PREFIX in last
            and R2_SYSNAME in last
            and LLDP_REM_PORT_ID_PREFIX in last
            and GE_IF in last
            and LLDP_REM_PORT_DESC_PREFIX in last
            and PORT_DESC_R2 in last
        ):
            return last
        time.sleep(2)
    raise AssertionError(f"timeout waiting for LLDP SNMP walk data; last output:\n{last}")


def _wait_snmpwalk_if(container: str, *, timeout: float = 30.0) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    command = f"snmpwalk -v2c -c public -On -t 2 -r 1 127.0.0.1 {IF_MIB_ROOT}"
    while time.monotonic() < deadline:
        last = _docker_exec(container, command, timeout=8, check=False)
        if (
            IF_NAME_PREFIX in last
            and IF_DESCR_PREFIX in last
            and GE_IF in last
            and IF_ADMIN_STATUS_PREFIX in last
            and IF_OPER_STATUS_PREFIX in last
        ):
            return last
        time.sleep(2)
    raise AssertionError(f"timeout waiting for IF-MIB SNMP walk data; last output:\n{last}")


def _extract_index_for_value(walk: str, oid_prefix: str, value: str) -> int:
    pattern = re.compile(rf"^{re.escape(oid_prefix)}\.(\d+)\s+=\s+.*\b{re.escape(value)}\b", re.MULTILINE)
    match = pattern.search(walk)
    if not match:
        raise AssertionError(f"missing {oid_prefix} row for {value}:\n{walk}")
    return int(match.group(1))


def _require_integer(walk: str, oid: str, value: int) -> None:
    pattern = re.compile(rf"^{re.escape(oid)}\s+=\s+.*\b{value}\b", re.MULTILINE)
    if not pattern.search(walk):
        raise AssertionError(f"missing integer value {oid}={value}:\n{walk}")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_container = rt.container_name("r1")
    collector: subprocess.Popen[str] | None = None

    try:
        _cleanup(rt)

        step("Start LLDP syslog collector and configure reporting on r1")
        collector = _start_udp_collector(r1_container)
        run_cmds(rt, "r1", commands=["config", f"syslog server 127.0.0.1 port {SYSLOG_PORT}", "end"])

        step("Configure LLDP topology and start SNMP on r1")
        _configure(rt)
        run_cmds(rt, "r1", commands=["config", f"snmp trap server 127.0.0.1 port {SNMP_TRAP_PORT}", "end"])

        step("Wait for LLDP neighbor discovery")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show lldp neighbors detail",
                    "contains": [f"Interface: {GE_IF}", f"System name : {R2_SYSNAME}", PORT_DESC_R2],
                    "label": "r1 learns r2 through lldp",
                }
            ],
            timeout=45,
            interval=3,
        )
        _wait_syslog(r1_container, ("lldp/neighbor-up", f"interface={GE_IF}", f"neighbor={R2_SYSNAME}"))

        step("Verify LLDP-MIB walk contains topology data")
        walk = _wait_snmpwalk_lldp(r1_container)
        if LLDP_LOC_PORT_ID_PREFIX not in walk:
            raise AssertionError(f"LLDP local port table missing from walk:\n{walk}")

        step("Verify IF-MIB and LLDP-MIB indexes are aligned")
        if_walk = _wait_snmpwalk_if(r1_container)
        if_index = _extract_index_for_value(if_walk, IF_NAME_PREFIX, GE_IF)
        if_descr_index = _extract_index_for_value(if_walk, IF_DESCR_PREFIX, GE_IF)
        lldp_port_num = _extract_index_for_value(walk, LLDP_LOC_PORT_ID_PREFIX, GE_IF)
        if if_index != if_descr_index or if_index != lldp_port_num:
            raise AssertionError(
                f"IF/LLDP index mismatch: ifName={if_index}, ifDescr={if_descr_index}, "
                f"lldpLocPortId={lldp_port_num}\nIF walk:\n{if_walk}\nLLDP walk:\n{walk}"
            )
        _require_integer(if_walk, f"{IF_ADMIN_STATUS_PREFIX}.{if_index}", 1)
        _require_integer(if_walk, f"{IF_OPER_STATUS_PREFIX}.{if_index}", 1)

        current = cmd(rt, "r1", "show current-configuration")
        if f"snmp trap server 127.0.0.1 port {SNMP_TRAP_PORT}" not in current:
            raise AssertionError(f"SNMP config missing from current configuration:\n{current}")

    finally:
        _cleanup(rt, restore_topology_sysname=True)
        _stop_collector(r1_container, collector)

    print("LLDP SNMP walk topology check passed.")
