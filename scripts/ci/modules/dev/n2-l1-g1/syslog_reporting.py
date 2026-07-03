#!/usr/bin/env python3
"""
Syslog reporting end-to-end check.

Coverage:
1. CLI configures `syslog server <server> [port <port>]` and BDR exports it.
2. A UDP syslog collector inside r1 receives CLI command reports.
3. IF link events, BGP neighbor state, and ISIS neighbor state reports are emitted.
4. `no syslog server` removes the configuration.
"""

from __future__ import annotations

import subprocess
import time
from pathlib import PurePosixPath

from module_api import cmd, g_top, mark_step_failed, require_devices, run_cmds, step  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


SYSLOG_PORT = 5514
SYSLOG_FILE = "/tmp/netnexus-syslog-ci.log"
SYSLOG_MAX_MESSAGES = 80

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


def _stop_collector(container: str, proc: subprocess.Popen[str]) -> None:
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


def _wait_syslog(container: str, needle: str, *, timeout: float = 25.0) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        last = _read_syslog(container)
        if needle in last:
            return last
        time.sleep(0.5)
    mark_step_failed()
    raise AssertionError(f"timeout waiting syslog needle {needle!r}; current syslog:\n{last}")


def _wait_syslog_any(container: str, needles: tuple[str, ...], *, timeout: float = 25.0) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        last = _read_syslog(container)
        if any(needle in last for needle in needles):
            return last
        time.sleep(0.5)
    mark_step_failed()
    raise AssertionError(f"timeout waiting syslog needles {needles!r}; current syslog:\n{last}")


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
                "no syslog server",
                "end",
            ],
        )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_container = rt.container_name("r1")
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    collector: subprocess.Popen[str] | None = None
    try:
        _cleanup(rt)
        collector = _start_udp_collector(r1_container)

        step("Configure remote syslog on r1")
        run_cmds(
            rt,
            "r1",
            commands=[
                "config",
                f"syslog server 127.0.0.1 port {SYSLOG_PORT}",
                "end",
            ],
        )
        current = cmd(rt, "r1", "show current-configuration")
        if f"syslog server 127.0.0.1 port {SYSLOG_PORT}" not in current:
            raise AssertionError(f"syslog config missing from current configuration:\n{current}")

        step("Verify CLI command report")
        cmd(rt, "r1", "show version")
        _wait_syslog(r1_container, "cli/command")
        _wait_syslog(r1_container, 'cmd="show version"')

        step("Verify IF link event report")
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
        _wait_syslog_any(r1_container, ("if/link-down", "if/proto-down"))

        step("Verify BGP neighbor state report")
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
        _wait_syslog(r1_container, "bgp/neighbor-state", timeout=45)
        _wait_syslog(r1_container, "new=Established", timeout=45)

        step("Verify ISIS neighbor state report")
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
        _wait_syslog(r1_container, "isis/neighbor-state", timeout=45)
        _wait_syslog(r1_container, "new=Up", timeout=45)

        step("Disable remote syslog and verify current configuration")
        run_cmds(rt, "r1", commands=["config", "no syslog server", "end"])
        current = cmd(rt, "r1", "show current-configuration")
        if "syslog server" in current:
            raise AssertionError(f"syslog config should be removed:\n{current}")

        print("Syslog reporting check passed.")
    finally:
        _cleanup(rt)
        if collector is not None:
            _stop_collector(r1_container, collector)
