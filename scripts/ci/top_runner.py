#!/usr/bin/env python3
"""
Topology-driven NetNexus BGP smoke runner.

Example:
  python3 scripts/ci/top_runner.py \
    --top scripts/ci/modules/bgp/two-node-r1ge1-r2ge1/top.yaml \
    --image netnexus-ci:latest
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import telnetlib
except ImportError as exc:  # pragma: no cover
    raise SystemExit(f"python stdlib telnetlib not available: {exc}")

try:
    import yaml
except ImportError:
    yaml = None


PROMPT_RE = re.compile(br"<NetNexus[^>]*>")
IF_RE = re.compile(r"^GE-(\d+)$")


def run_cmd(cmd: list[str], check: bool = True) -> str:
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )
    return (proc.stdout or "").strip()


def sanitize_name(name: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_.-]+", "-", name)


def parse_if_index(if_name: str) -> int:
    m = IF_RE.match(if_name or "")
    if not m:
        raise ValueError(f"invalid interface name '{if_name}', expected GE-<n>")
    idx = int(m.group(1))
    if idx < 1:
        raise ValueError(f"invalid interface index in '{if_name}'")
    return idx


def parse_cidr(cidr: str) -> tuple[str, int]:
    iface = ipaddress.ip_interface(cidr)
    if iface.version != 4:
        raise ValueError(f"only IPv4 is supported for now, got '{cidr}'")
    return str(iface.ip), int(iface.network.prefixlen)


@dataclass
class Endpoint:
    link_name: str
    device: str
    if_name: str
    ip: str
    prefix: int


class NetNexusCli:
    def __init__(self, host: str, port: int, name: str, cmd_timeout: int = 20, verbose: bool = False) -> None:
        self.host = host
        self.port = port
        self.name = name
        self.cmd_timeout = cmd_timeout
        self.verbose = verbose
        self.tn: telnetlib.Telnet | None = None
        self._rx_buf = bytearray()

    def connect(self, timeout: int = 25) -> None:
        deadline = time.time() + timeout
        last_err: Exception | None = None
        while time.time() < deadline:
            try:
                self.tn = telnetlib.Telnet(self.host, self.port, timeout=5)
                self._read_until_prompt(timeout=6)
                return
            except Exception as exc:
                last_err = exc
                self.close()
                time.sleep(1)
        raise RuntimeError(f"{self.name}: failed to connect CLI within {timeout}s: {last_err}")

    def close(self) -> None:
        if self.tn:
            try:
                self.tn.close()
            except Exception:
                pass
            self.tn = None
        self._rx_buf.clear()

    def cmd(self, command: str, timeout: int | None = None, strict: bool = True) -> str:
        if not self.tn:
            raise RuntimeError(f"{self.name}: CLI not connected")
        eff_timeout = timeout if timeout is not None else self.cmd_timeout
        if self.verbose:
            print(f"[{self.name}] >>> {command}")
        self.tn.write(command.encode("ascii") + b"\n")
        out = self._read_with_prompt_recovery(command=command, timeout=eff_timeout)
        text = out.replace("\r", "")
        if self.verbose:
            print(f"[{self.name}] <<< {text.strip()}")
        if strict and ("BGP Error:" in text or "Error:" in text):
            raise RuntimeError(f"{self.name}: command failed: {command}\n{text}")
        return text

    def _read_with_prompt_recovery(self, command: str, timeout: int) -> str:
        try:
            return self._read_until_prompt(timeout=timeout)
        except RuntimeError as first_exc:
            # In noisy console scenarios, the command may have succeeded but prompt
            # framing can be disrupted. Try a single newline to force a fresh prompt.
            if "timeout waiting prompt" not in str(first_exc):
                raise
            if self.verbose:
                print(f"[{self.name}] !!! prompt timeout after '{command}', retry with newline")

            assert self.tn is not None
            self.tn.write(b"\n")
            recovery_timeout = min(4, max(2, timeout // 3))
            try:
                recovered = self._read_until_prompt(timeout=recovery_timeout)
            except Exception:
                raise first_exc
            if self.verbose:
                print(f"[{self.name}] !!! prompt recovered after newline")
            return recovered

    def _read_until_prompt(self, timeout: int) -> str:
        assert self.tn is not None
        deadline = time.time() + timeout

        while time.time() < deadline:
            m = PROMPT_RE.search(self._rx_buf)
            if m is not None:
                end = m.end()
                data = bytes(self._rx_buf[:end])
                del self._rx_buf[:end]
                return data.decode("utf-8", errors="ignore")

            try:
                chunk = self.tn.read_very_eager()
            except EOFError as exc:
                tail = bytes(self._rx_buf[-512:]).decode("utf-8", errors="ignore").replace("\r", "")
                raise RuntimeError(f"{self.name}: CLI connection closed while waiting prompt, tail:\n{tail}") from exc

            if chunk:
                if self.verbose:
                    preview = repr(chunk[:120])
                    suffix = "..." if len(chunk) > 120 else ""
                    print(f"[{self.name}] ... rx {len(chunk)} bytes {preview}{suffix}")
                self._rx_buf.extend(chunk)
                if len(self._rx_buf) > 262144:
                    del self._rx_buf[:-262144]
                continue

            time.sleep(0.02)

        tail_raw = bytes(self._rx_buf[-512:])
        tail_txt = tail_raw.decode("utf-8", errors="ignore").replace("\r", "")
        tail_hex = tail_raw.hex(" ")
        raise RuntimeError(f"{self.name}: timeout waiting prompt ({timeout}s), tail:\n{tail_txt}\nhex:\n{tail_hex}")


def load_topology(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise ValueError(f"topology file not found: {path}")

    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() == ".json":
        data = json.loads(text)
    else:
        if yaml is None:
            raise ValueError("PyYAML is required for YAML top files (pip install pyyaml)")
        data = yaml.safe_load(text)

    if not isinstance(data, dict):
        raise ValueError("topology root must be a mapping")
    return data


def validate_top(top: dict[str, Any]) -> None:
    devices = top.get("devices")
    links = top.get("links")

    if not isinstance(devices, dict) or not devices:
        raise ValueError("top.devices must be a non-empty mapping")
    if not isinstance(links, list) or not links:
        raise ValueError("top.links must be a non-empty list")

    for dev, cfg in devices.items():
        if not isinstance(cfg, dict):
            raise ValueError(f"device '{dev}' config must be a mapping")

    used_if: dict[str, set[str]] = {d: set() for d in devices}
    for link in links:
        if not isinstance(link, dict):
            raise ValueError("each link must be a mapping")
        lname = str(link.get("name", "")).strip()
        if not lname:
            raise ValueError("link.name is required")
        eps = link.get("endpoints")
        if not isinstance(eps, list) or len(eps) != 2:
            raise ValueError(f"link '{lname}' must have exactly 2 endpoints")
        for ep in eps:
            dev = ep.get("device")
            if_name = ep.get("if")
            cidr = ep.get("cidr")
            if dev not in devices:
                raise ValueError(f"link '{lname}': unknown device '{dev}'")
            parse_if_index(str(if_name))
            parse_cidr(str(cidr))
            if if_name in used_if[dev]:
                raise ValueError(f"device '{dev}' interface '{if_name}' used by multiple links")
            used_if[dev].add(str(if_name))
        if eps[0].get("device") == eps[1].get("device"):
            raise ValueError(f"link '{lname}' endpoints cannot be same device")

    for dev, ifs in used_if.items():
        if len(ifs) > 4:
            raise ValueError(f"device '{dev}' uses {len(ifs)} links; max 4 is supported")


def build_endpoints(top: dict[str, Any]) -> dict[str, list[Endpoint]]:
    per_dev: dict[str, list[Endpoint]] = {dev: [] for dev in top["devices"]}
    for link in top["links"]:
        lname = str(link["name"])
        for ep in link["endpoints"]:
            ip, pfx = parse_cidr(str(ep["cidr"]))
            per_dev[ep["device"]].append(
                Endpoint(
                    link_name=lname,
                    device=ep["device"],
                    if_name=str(ep["if"]),
                    ip=ip,
                    prefix=pfx,
                )
            )
    for dev in per_dev:
        per_dev[dev].sort(key=lambda x: parse_if_index(x.if_name))
    return per_dev


def find_peer_ip(top: dict[str, Any], local: str, peer: str, local_if: str | None = None) -> str:
    candidates: list[tuple[str, str, str]] = []
    for link in top["links"]:
        eps = link["endpoints"]
        a = eps[0]
        b = eps[1]
        if a["device"] == local and b["device"] == peer:
            candidates.append((str(a["if"]), str(a["cidr"]), str(b["cidr"])))
        elif a["device"] == peer and b["device"] == local:
            candidates.append((str(b["if"]), str(b["cidr"]), str(a["cidr"])))

    if not candidates:
        raise ValueError(f"no link found between {local} and {peer}")

    chosen: tuple[str, str, str] | None = None
    if local_if:
        for c in candidates:
            if c[0] == local_if:
                chosen = c
                break
        if chosen is None:
            raise ValueError(f"session {local}->{peer}: local_if '{local_if}' not matched in links")
    else:
        if len(candidates) != 1:
            raise ValueError(
                f"session {local}->{peer}: multiple links found, set local_if to disambiguate"
            )
        chosen = candidates[0]

    peer_cidr = chosen[2]
    return parse_cidr(peer_cidr)[0]


def build_if_map_file(path: Path, endpoints: list[Endpoint]) -> None:
    lines = ["# Auto-generated for topology run\n"]
    for idx, ep in enumerate(endpoints, start=1):
        lines.append(f"{ep.if_name} = eth{idx}\n")
    path.write_text("".join(lines), encoding="utf-8")


def get_container_network_ip(container_name: str, network_name: str) -> str:
    raw = run_cmd(["docker", "inspect", container_name])
    payload = json.loads(raw)
    if not isinstance(payload, list) or not payload:
        raise RuntimeError(f"docker inspect returned unexpected payload for {container_name}")
    ip = (
        payload[0]
        .get("NetworkSettings", {})
        .get("Networks", {})
        .get(network_name, {})
        .get("IPAddress", "")
        .strip()
    )
    if not ip:
        raise RuntimeError(f"failed to get IP for container={container_name} network={network_name}")
    return ip


class DeviceExec:
    """
    Device-bound command sender.
    """

    def __init__(self, runtime: "TopologyRuntime", device: str) -> None:
        self.runtime = runtime
        self.device = device

    def exec(self, command: str, *, timeout: int | None = None, strict: bool = True) -> str:
        return self.runtime.exec_cmd(self.device, command, timeout=timeout, strict=strict)

    def cmd(self, command: str, *, timeout: int | None = None, strict: bool = True) -> str:
        return self.exec(command, timeout=timeout, strict=strict)

    def run(self, commands: list[str], *, timeout: int | None = None, strict: bool = True) -> list[str]:
        outputs: list[str] = []
        for command in commands:
            outputs.append(self.exec(command, timeout=timeout, strict=strict))
        return outputs

    def __call__(self, command: str, *, timeout: int | None = None, strict: bool = True) -> str:
        return self.exec(command, timeout=timeout, strict=strict)


class TopologyRuntime:
    """
    Shared runtime for module scripts.

    Responsibilities:
    - Bring up containers/networks from a shared top file.
    - Start NetNexus processes and establish CLI sessions.
    - Provide common `exec_cmd` for module scripts.
    - Handle teardown/keep behavior.
    """

    def __init__(
        self,
        top: dict[str, Any],
        image: str,
        prefix: str,
        *,
        keep: bool = False,
        cmd_timeout: int = 20,
        connect_timeout: int = 60,
        verbose: bool = False,
    ) -> None:
        validate_top(top)
        self.top = top
        self.image = image
        self.prefix = sanitize_name(prefix)
        self.keep = keep
        self.cmd_timeout = cmd_timeout
        self.connect_timeout = connect_timeout
        self.verbose = verbose

        self.mgmt_net = f"{self.prefix}-mgmt"
        self.devices: dict[str, dict[str, Any]] = top["devices"]
        self.endpoints = build_endpoints(top)
        self.tmpdir = Path(tempfile.mkdtemp(prefix=f"{self.prefix}-"))

        self.container_names: list[str] = []
        self.link_networks: list[str] = []
        self.cli_map: dict[str, NetNexusCli] = {}

    def start(self, *, configure_interfaces: bool = True) -> None:
        run_cmd(["docker", "network", "create", self.mgmt_net])

        # 1) Create paused containers and mount per-device if_map override.
        for dev in self.devices:
            cname = f"{self.prefix}-{sanitize_name(dev)}"

            map_file = self.tmpdir / f"{sanitize_name(dev)}-if_map.conf.gns3"
            build_if_map_file(map_file, self.endpoints[dev])

            run_cmd(
                [
                    "docker",
                    "run",
                    "-d",
                    "--rm",
                    "--name",
                    cname,
                    "--hostname",
                    dev,
                    "--network",
                    self.mgmt_net,
                    "--cap-add",
                    "NET_ADMIN",
                    "--cap-add",
                    "NET_RAW",
                    "-e",
                    "NN_WORK_DIR=/opt/netnexus",
                    "-e",
                    "LD_LIBRARY_PATH=/opt/netnexus/lib",
                    "-v",
                    f"{map_file}:/opt/netnexus/resources/if/if_map.conf.gns3:ro",
                    self.image,
                    "sleep",
                    "infinity",
                ]
            )
            self.container_names.append(cname)

        # 2) Create link networks.
        link_to_net: dict[str, str] = {}
        for link in self.top["links"]:
            lname = str(link["name"])
            net_name = f"{self.prefix}-lnk-{sanitize_name(lname)}"
            run_cmd(["docker", "network", "create", net_name])
            link_to_net[lname] = net_name
            self.link_networks.append(net_name)

        # 3) Connect each device to link networks in GE index order.
        for dev, eps in self.endpoints.items():
            cname = f"{self.prefix}-{sanitize_name(dev)}"
            for ep in eps:
                run_cmd(["docker", "network", "connect", link_to_net[ep.link_name], cname])

        # 4) Start netnexus process inside each container.
        for dev in self.devices:
            cname = f"{self.prefix}-{sanitize_name(dev)}"
            run_cmd(
                [
                    "docker",
                    "exec",
                    "-d",
                    cname,
                    "/bin/bash",
                    "-lc",
                    (
                        "mkdir -p /opt/netnexus/log /opt/netnexus/data && "
                        "export NN_WORK_DIR=/opt/netnexus && "
                        "export LD_LIBRARY_PATH=/opt/netnexus/lib:${LD_LIBRARY_PATH} && "
                        "exec /opt/netnexus/bin/netnexus > /tmp/netnexus.log 2>&1"
                    ),
                ]
            )

        # 5) Connect CLI via management network IP.
        for dev in self.devices:
            cname = f"{self.prefix}-{sanitize_name(dev)}"
            mgmt_ip = get_container_network_ip(cname, self.mgmt_net)
            cli = NetNexusCli(mgmt_ip, 3788, dev, cmd_timeout=self.cmd_timeout, verbose=self.verbose)
            cli.connect(timeout=self.connect_timeout)
            self.cli_map[dev] = cli

        # 6) Optional interface base config from top.
        if configure_interfaces:
            for dev, eps in self.endpoints.items():
                cli_configure_interfaces(self.cli_map[dev], eps)

    def exec_cmd(self, device: str, command: str, *, timeout: int | None = None, strict: bool = True) -> str:
        cli = self.cli_map.get(device)
        if cli is None:
            raise ValueError(f"unknown or disconnected device '{device}'")
        return cli.cmd(command, timeout=timeout, strict=strict)

    def on(self, device: str) -> DeviceExec:
        if device not in self.devices:
            raise ValueError(f"unknown device '{device}'")
        return DeviceExec(self, device)

    def close(self, *, failed: bool = False) -> None:
        for cli in self.cli_map.values():
            cli.close()
        self.cli_map.clear()

        if self.keep:
            print("Resources kept (--keep enabled).")
            print(f"Containers: {', '.join(self.container_names)}")
            print(f"Networks: {', '.join([self.mgmt_net] + self.link_networks)}")
            print(f"Temp dir: {self.tmpdir}")
            return

        for name in self.container_names:
            run_cmd(["docker", "rm", "-f", name], check=False)
        for net in self.link_networks:
            run_cmd(["docker", "network", "rm", net], check=False)
        run_cmd(["docker", "network", "rm", self.mgmt_net], check=False)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

        if failed:
            print("Cleanup complete after failure.", file=sys.stderr)


def execCmd(runtime: TopologyRuntime, device: str) -> DeviceExec:
    """
    Return a device-bound command executor for module scripts.
    """
    return runtime.on(device)


def cli_configure_interfaces(cli: NetNexusCli, endpoints: list[Endpoint]) -> None:
    print(f"\n===== STEP: Auto configure interface IPs on {cli.name} =====", flush=True)
    cli.cmd("config")
    for ep in endpoints:
        print(
            f"[{cli.name}] apply interface {ep.if_name}: ip address {ep.ip} {ep.prefix}, no shutdown",
            flush=True,
        )
        cli.cmd(f"if {ep.if_name}")
        cli.cmd(f"ip address {ep.ip} {ep.prefix}")
        cli.cmd("no shutdown")
        cli.cmd("exit")
    cli.cmd("end")


def cli_configure_bgp_base(cli: NetNexusCli, asn: int, router_id: str) -> None:
    cli.cmd("config")
    cli.cmd(f"bgp {asn}")
    cli.cmd(f"router-id {router_id}")
    cli.cmd("end")


def cli_configure_bgp_session(
    cli: NetNexusCli,
    local_as: int,
    peer_ip: str,
    peer_as: int,
    afs: list[str],
    import_static: bool,
) -> None:
    cli.cmd("config")
    cli.cmd(f"bgp {local_as}")
    cli.cmd(f"neighbor {peer_ip} as {peer_as}")
    for af in afs:
        cli.cmd(f"af {af}")
        cli.cmd(f"neighbor {peer_ip} enable")
        if import_static:
            cli.cmd("import-route static")
        cli.cmd("exit")
    cli.cmd("end")


def cli_add_static_route(cli: NetNexusCli, prefix: str, mask: str, nexthop: str) -> None:
    cli.cmd("config")
    cli.cmd(f"route ipv4 {prefix} {mask} {nexthop}")
    cli.cmd("end")


def wait_sessions(cli_map: dict[str, NetNexusCli], checks: list[dict[str, str]], timeout: int) -> None:
    deadline = time.time() + timeout
    last_out: dict[str, str] = {}

    while time.time() < deadline:
        pending = 0
        for chk in checks:
            dev = chk["local"]
            peer_ip = chk["peer_ip"]
            af = chk["af"]
            out = cli_map[dev].cmd(f"show bgp neighbor af {af}", strict=False)
            last_out[dev] = out
            if peer_ip not in out or "Established" not in out:
                pending += 1
        if pending == 0:
            return
        time.sleep(2)

    detail = "\n\n".join([f"[{d}]\n{v}" for d, v in last_out.items()])
    raise RuntimeError(f"BGP sessions not established within {timeout}s\n{detail}")


def wait_routes(cli_map: dict[str, NetNexusCli], checks: list[dict[str, str]], timeout: int) -> None:
    if not checks:
        return

    deadline = time.time() + timeout
    last_out: dict[str, str] = {}

    while time.time() < deadline:
        pending = 0
        for chk in checks:
            dev = chk["device"]
            af = chk["af"]
            prefix = chk["prefix"]
            out = cli_map[dev].cmd(f"show bgp route af {af}", strict=False)
            last_out[dev] = out
            if prefix not in out:
                pending += 1
        if pending == 0:
            return
        time.sleep(2)

    detail = "\n\n".join([f"[{d}]\n{v}" for d, v in last_out.items()])
    raise RuntimeError(f"BGP route checks failed within {timeout}s\n{detail}")


def dump_logs(container_names: list[str]) -> None:
    for name in container_names:
        print(f"\n===== docker logs: {name} =====")
        proc = subprocess.run(["docker", "logs", name], text=True, capture_output=True)
        if proc.stdout:
            print(proc.stdout)
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
        inner = subprocess.run(
            ["docker", "exec", name, "/bin/bash", "-lc", "test -f /tmp/netnexus.log && cat /tmp/netnexus.log"],
            text=True,
            capture_output=True,
        )
        if inner.stdout.strip():
            print(f"----- /tmp/netnexus.log ({name}) -----")
            print(inner.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run NetNexus topology smoke test from top file")
    parser.add_argument("--top", required=True, help="topology file path (.yaml/.yml/.json)")
    parser.add_argument("--image", required=False, help="docker image tag")
    parser.add_argument("--prefix", default=f"nn-top-{os.getpid()}", help="resource name prefix")
    parser.add_argument("--keep", action="store_true", help="keep containers/networks for debugging")
    parser.add_argument("--cmd-timeout", type=int, default=20, help="CLI command timeout seconds")
    parser.add_argument("--verbose", action="store_true", help="print command-level debug output")
    args = parser.parse_args()

    top = load_topology(Path(args.top))
    validate_top(top)

    image = args.image or str(top.get("image", "")).strip()
    if not image:
        raise SystemExit("image is required (use --image or top.image)")

    prefix = sanitize_name(args.prefix)
    mgmt_net = f"{prefix}-mgmt"

    devices: dict[str, dict[str, Any]] = top["devices"]
    endpoints = build_endpoints(top)
    sessions = top["bgp"]["sessions"]
    traffic = top.get("traffic", {})
    checks_cfg = top.get("checks", {})
    session_timeout = int(checks_cfg.get("session_timeout_sec", 60))
    route_timeout = int(checks_cfg.get("route_timeout_sec", 60))
    route_checks = checks_cfg.get("bgp_routes", [])

    tmpdir = Path(tempfile.mkdtemp(prefix=f"{prefix}-"))
    container_names: list[str] = []
    link_networks: list[str] = []
    cli_map: dict[str, NetNexusCli] = {}
    failed = False
    exit_code = 0

    try:
        run_cmd(["docker", "network", "create", mgmt_net])

        # 1) Create paused containers (sleep), mount per-device if_map override.
        for dev, cfg in devices.items():
            cname = f"{prefix}-{sanitize_name(dev)}"

            map_file = tmpdir / f"{sanitize_name(dev)}-if_map.conf.gns3"
            build_if_map_file(map_file, endpoints[dev])

            run_cmd(
                [
                    "docker",
                    "run",
                    "-d",
                    "--rm",
                    "--name",
                    cname,
                    "--hostname",
                    dev,
                    "--network",
                    mgmt_net,
                    "--cap-add",
                    "NET_ADMIN",
                    "--cap-add",
                    "NET_RAW",
                    "-e",
                    "NN_WORK_DIR=/opt/netnexus",
                    "-e",
                    "LD_LIBRARY_PATH=/opt/netnexus/lib",
                    "-v",
                    f"{map_file}:/opt/netnexus/resources/if/if_map.conf.gns3:ro",
                    image,
                    "sleep",
                    "infinity",
                ]
            )
            container_names.append(cname)

        # 2) Create link networks.
        link_to_net: dict[str, str] = {}
        for link in top["links"]:
            lname = str(link["name"])
            net_name = f"{prefix}-lnk-{sanitize_name(lname)}"
            run_cmd(["docker", "network", "create", net_name])
            link_to_net[lname] = net_name
            link_networks.append(net_name)

        # 3) Connect each device to link networks in GE index order.
        for dev, eps in endpoints.items():
            cname = f"{prefix}-{sanitize_name(dev)}"
            for ep in eps:
                run_cmd(["docker", "network", "connect", link_to_net[ep.link_name], cname])

        # 4) Start netnexus process inside each container.
        for dev in devices:
            cname = f"{prefix}-{sanitize_name(dev)}"
            run_cmd(
                [
                    "docker",
                    "exec",
                    "-d",
                    cname,
                    "/bin/bash",
                    "-lc",
                    (
                        "mkdir -p /opt/netnexus/log /opt/netnexus/data && "
                        "export NN_WORK_DIR=/opt/netnexus && "
                        "export LD_LIBRARY_PATH=/opt/netnexus/lib:${LD_LIBRARY_PATH} && "
                        "exec /opt/netnexus/bin/netnexus > /tmp/netnexus.log 2>&1"
                    ),
                ]
            )

        # 5) Wait CLI and connect.
        for dev in devices:
            cname = f"{prefix}-{sanitize_name(dev)}"
            mgmt_ip = get_container_network_ip(cname, mgmt_net)
            cli = NetNexusCli(mgmt_ip, 3788, dev, cmd_timeout=args.cmd_timeout, verbose=args.verbose)
            cli.connect(timeout=30)
            cli_map[dev] = cli

        # 6) Configure interface IPs.
        for dev, eps in endpoints.items():
            cli_configure_interfaces(cli_map[dev], eps)

        # 7) Configure per-device BGP base.
        for dev, cfg in devices.items():
            cli_configure_bgp_base(cli_map[dev], int(cfg["asn"]), str(cfg["router_id"]))

        # 8) Configure BGP sessions.
        session_checks: list[dict[str, str]] = []
        for sess in sessions:
            local = str(sess["local"])
            peer = str(sess["peer"])
            afs = list(sess.get("afs", ["ipv4-unicast"]))
            local_if = sess.get("local_if")
            peer_ip = find_peer_ip(top, local, peer, local_if=local_if)

            cli_configure_bgp_session(
                cli=cli_map[local],
                local_as=int(devices[local]["asn"]),
                peer_ip=peer_ip,
                peer_as=int(devices[peer]["asn"]),
                afs=afs,
                import_static=bool(sess.get("import_static", False)),
            )

            for af in afs:
                session_checks.append({"local": local, "peer_ip": peer_ip, "af": af})

        # 9) Optional business traffic/static route injection.
        for route in traffic.get("static_routes", []) or []:
            dev = str(route["device"])
            cli_add_static_route(
                cli_map[dev],
                prefix=str(route["prefix"]),
                mask=str(route["mask"]),
                nexthop=str(route["nexthop"]),
            )

        # 10) Wait checks.
        wait_sessions(cli_map, session_checks, timeout=session_timeout)

        parsed_route_checks: list[dict[str, str]] = []
        for chk in route_checks:
            parsed_route_checks.append(
                {
                    "device": str(chk["device"]),
                    "af": str(chk.get("af", "ipv4-unicast")),
                    "prefix": str(chk["prefix"]),
                }
            )
        wait_routes(cli_map, parsed_route_checks, timeout=route_timeout)

        print("Topology smoke test passed.")
        exit_code = 0
    except Exception as exc:
        failed = True
        exit_code = 1
        print(f"ERROR: {exc}", file=sys.stderr)
        dump_logs(container_names)
    finally:
        for cli in cli_map.values():
            cli.close()

        if args.keep:
            print("Resources kept (--keep enabled).")
            print(f"Containers: {', '.join(container_names)}")
            print(f"Networks: {', '.join([mgmt_net] + link_networks)}")
            print(f"Temp dir: {tmpdir}")
        else:
            for name in container_names:
                run_cmd(["docker", "rm", "-f", name], check=False)
            for net in link_networks:
                run_cmd(["docker", "network", "rm", net], check=False)
            run_cmd(["docker", "network", "rm", mgmt_net], check=False)
            shutil.rmtree(tmpdir, ignore_errors=True)

            if failed:
                print("Cleanup complete after failure.", file=sys.stderr)

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
