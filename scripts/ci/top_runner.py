#!/usr/bin/env python3
"""
Topology-driven NetNexus BGP smoke runner.

Example:
  python3 scripts/ci/top_runner.py \
    --top scripts/ci/modules/bgp/n2-l1-g1/top.yaml \
    --image netnexus-ci:latest
"""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import re
import subprocess
import sys
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
PAGER_DISABLE_CMD = "terminal length 0"

# 每台设备固定预占 4 个 GE 口，没挂 link 的槽位用 stub 网络补位，
# 保证容器内 eth1..eth4 始终存在且与 if_map.conf.gns3 对齐。
# remove_link/add_link 通过 link<->stub 原位互换来完成热插拔，避免
# ethN 变成 eth5+ 导致 GE 映射错位。
GE_PORT_COUNT = 4


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
        raise ValueError(f"expected IPv4 CIDR, got '{cidr}'")
    return str(iface.ip), int(iface.network.prefixlen)


def parse_cidr6(cidr: str) -> tuple[str, int]:
    iface = ipaddress.ip_interface(cidr)
    if iface.version != 6:
        raise ValueError(f"expected IPv6 CIDR, got '{cidr}'")
    return str(iface.ip), int(iface.network.prefixlen)


@dataclass
class Endpoint:
    link_name: str
    device: str
    if_name: str
    ip: str
    prefix: int
    ip6: str = ""
    prefix6: int = 0


class NetNexusCli:
    def __init__(
        self,
        host: str,
        port: int,
        name: str,
        cmd_timeout: int = 20,
        verbose: bool = False,
        log_commands: bool = True,
    ) -> None:
        self.host = host
        self.port = port
        self.name = name
        self.cmd_timeout = cmd_timeout
        self.verbose = verbose
        self.log_commands = log_commands
        self.tn: telnetlib.Telnet | None = None
        self._rx_buf = bytearray()

    def connect(self, timeout: int = 25) -> None:
        deadline = time.time() + timeout
        last_err: Exception | None = None
        while time.time() < deadline:
            try:
                self.tn = telnetlib.Telnet(self.host, self.port, timeout=5)
                self._read_until_prompt(timeout=6)
                self.disable_pager(timeout=min(self.cmd_timeout, 6))
                return
            except Exception as exc:
                last_err = exc
                self.close()
                time.sleep(1)
        raise RuntimeError(f"{self.name}: failed to connect CLI within {timeout}s: {last_err}")

    def disable_pager(self, timeout: int | None = None) -> None:
        eff_timeout = timeout if timeout is not None else min(self.cmd_timeout, 8)
        # terminal-length command is user-view scoped; ensure we are not left in sub-views.
        self.cmd("end", timeout=eff_timeout, strict=False)
        self.cmd(PAGER_DISABLE_CMD, timeout=eff_timeout, strict=True)

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
        if self.log_commands or self.verbose:
            print(f"[{self.name}] >>> {command}")
        self.tn.write(command.encode("ascii") + b"\n")
        out = self._read_with_prompt_recovery(command=command, timeout=eff_timeout)
        text = out.replace("\r", "")
        if self.log_commands or self.verbose:
            print(f"[{self.name}] <<< {text.strip()}")
        if strict and ("BGP Error:" in text or "Error:" in text):
            raise RuntimeError(f"{self.name}: command failed: {command}\n{text}")
        return text

    def query_help(self, partial: str, timeout: int | None = None) -> str:
        """Send ``<partial>?`` (no newline) to trigger CLI help and return captured text.

        After capturing the help output, clears the server's line buffer using
        backspaces and issues a blank newline to restore a clean prompt so
        that subsequent :meth:`cmd` calls are not polluted by the residual
        input.
        """
        if not self.tn:
            raise RuntimeError(f"{self.name}: CLI not connected")
        eff_timeout = timeout if timeout is not None else self.cmd_timeout
        if self.log_commands or self.verbose:
            print(f"[{self.name}] >>> {partial}? (help)")
        self.tn.write(partial.encode("ascii") + b"?")
        help_raw = self._read_with_prompt_recovery(command=f"{partial}?", timeout=eff_timeout)
        help_text = help_raw.replace("\r", "")
        if self.log_commands or self.verbose:
            print(f"[{self.name}] <<< {help_text.strip()}")
        cleanup = (b"\x7f" * len(partial)) + b"\n"
        self.tn.write(cleanup)
        try:
            self._read_until_prompt(timeout=eff_timeout)
        except Exception:
            pass
        return help_text

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
    seen_links: set[str] = set()
    for link in links:
        if not isinstance(link, dict):
            raise ValueError("each link must be a mapping")
        lname = str(link.get("name", "")).strip()
        if not lname:
            raise ValueError("link.name is required")
        if lname in seen_links:
            raise ValueError(f"duplicate link.name '{lname}' is not allowed")
        seen_links.add(lname)
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
            cidr6 = str(ep.get("cidr6", "")).strip()
            if cidr6:
                parse_cidr6(cidr6)
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
            ip6 = ""
            pfx6 = 0
            cidr6 = str(ep.get("cidr6", "")).strip()
            if cidr6:
                ip6, pfx6 = parse_cidr6(cidr6)
            per_dev[ep["device"]].append(
                Endpoint(
                    link_name=lname,
                    device=ep["device"],
                    if_name=str(ep["if"]),
                    ip=ip,
                    prefix=pfx,
                    ip6=ip6,
                    prefix6=pfx6,
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

    def query_help(self, partial: str, *, timeout: int | None = None) -> str:
        cli = self.runtime.cli_map.get(self.device)
        if cli is None:
            raise ValueError(f"unknown or disconnected device '{self.device}'")
        return cli.query_help(partial, timeout=timeout)


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
        connect_timeout: int = 90,
        verbose: bool = False,
        publish_cli_base: int | None = None,
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
        self.device_order: list[str] = sorted(self.devices.keys())
        self.endpoints = build_endpoints(top)
        self.link_endpoints: dict[str, tuple[tuple[str, str], tuple[str, str]]] = {}
        for link in self.top["links"]:
            lname = str(link["name"])
            eps = link["endpoints"]
            self.link_endpoints[lname] = (
                (str(eps[0]["device"]), str(eps[0]["if"])),
                (str(eps[1]["device"]), str(eps[1]["if"])),
            )
        self.link_to_net: dict[str, str] = {}
        self.link_connected: set[str] = set()

        self.container_names: list[str] = []
        self.link_networks: list[str] = []
        # 所有已创建的 stub 网络（key 由 _stub_network_name 生成），close() 时统一清理
        self.stub_networks: set[str] = set()
        # 每台设备每个 GE 槽位当前挂的 docker 网络名（link 或 stub），用于热插拔时知道该 disconnect 哪个
        # 结构：{device: {ge_idx: net_name}}
        self.slot_net: dict[str, dict[int, str]] = {dev: {} for dev in self.devices}
        self.cli_map: dict[str, NetNexusCli] = {}
        self.cli_publish_ports: dict[str, int] = {}
        if publish_cli_base is not None:
            if publish_cli_base < 1 or publish_cli_base > 65535:
                raise ValueError(f"publish_cli_base out of range: {publish_cli_base}")
            end_port = publish_cli_base + len(self.device_order) - 1
            if end_port > 65535:
                raise ValueError(
                    f"publish_cli_base range overflow: base={publish_cli_base}, devices={len(self.device_order)}"
                )
            for idx, dev in enumerate(self.device_order):
                self.cli_publish_ports[dev] = publish_cli_base + idx

    def _container_name(self, device: str) -> str:
        return f"{self.prefix}-{sanitize_name(device)}"

    def container_name(self, device: str) -> str:
        if device not in self.devices:
            raise ValueError(f"unknown device '{device}'")
        return self._container_name(device)

    def get_mgmt_ip(self, device: str) -> str:
        return get_container_network_ip(self.container_name(device), self.mgmt_net)

    def get_published_cli_port(self, device: str) -> int | None:
        if device not in self.devices:
            raise ValueError(f"unknown device '{device}'")
        return self.cli_publish_ports.get(device)

    def _start_netnexus_process(self, container_name: str) -> None:
        run_cmd(
            [
                "docker",
                "exec",
                "-d",
                container_name,
                "/bin/bash",
                "-lc",
                (
                    "mkdir -p /opt/netnexus/log /opt/netnexus/log/asan /opt/netnexus/data && "
                    "export NN_WORK_DIR=/opt/netnexus && "
                    "export LD_LIBRARY_PATH=/opt/netnexus/lib:${LD_LIBRARY_PATH} && "
                    "export ASAN_OPTIONS="
                    "\"${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=0:abort_on_error=0:"
                    "log_path=/opt/netnexus/log/asan/asan}\" && "
                    "exec /opt/netnexus/bin/netnexus > /tmp/netnexus.log 2>&1"
                ),
            ]
        )

    def _connect_cli(self, device: str, host: str, *, timeout: int) -> None:
        cli = NetNexusCli(host, 3788, device, cmd_timeout=self.cmd_timeout, verbose=self.verbose)
        cli.connect(timeout=timeout)
        self.cli_map[device] = cli

    def _get_link_endpoints(self, link_name: str) -> tuple[tuple[str, str], tuple[str, str]]:
        name = str(link_name).strip()
        if not name:
            raise ValueError("link_name is required")
        endpoints = self.link_endpoints.get(name)
        if endpoints is None:
            available = ", ".join(sorted(self.link_endpoints.keys()))
            raise ValueError(f"unknown link '{name}', available: {available}")
        return endpoints

    def _get_link_network(self, link_name: str) -> str:
        name = str(link_name).strip()
        self._get_link_endpoints(name)
        net_name = self.link_to_net.get(name, "").strip()
        if not net_name:
            raise RuntimeError(
                f"link '{name}' network is not initialized yet, call TopologyRuntime.start() first"
            )
        return net_name

    @staticmethod
    def _is_benign_link_error(action: str, err_text: str) -> bool:
        normalized = err_text.lower()
        if action == "connect":
            return (
                "already exists in network" in normalized
                or "already connected" in normalized
                or "endpoint with name" in normalized
            )
        if action == "disconnect":
            return "is not connected" in normalized or "endpoint not found" in normalized
        return False

    def _docker_network_connect(self, network_name: str, container_name: str, *, strict: bool) -> None:
        try:
            run_cmd(["docker", "network", "connect", network_name, container_name])
        except RuntimeError as exc:
            if (not strict) and self._is_benign_link_error("connect", str(exc)):
                return
            raise

    def _docker_network_disconnect(self, network_name: str, container_name: str, *, strict: bool) -> None:
        try:
            run_cmd(["docker", "network", "disconnect", network_name, container_name])
        except RuntimeError as exc:
            if (not strict) and self._is_benign_link_error("disconnect", str(exc)):
                return
            raise

    def _stub_network_name(self, device: str, ge_idx: int) -> str:
        return f"{self.prefix}-stub-{sanitize_name(device)}-{ge_idx}"

    def _ensure_stub_network(self, device: str, ge_idx: int) -> str:
        """创建（或复用）某台设备某 GE 槽位专用的 stub 桥网络，仅用于占位保持 ethN 对齐。"""
        name = self._stub_network_name(device, ge_idx)
        if name not in self.stub_networks:
            existing = run_cmd(
                ["docker", "network", "ls", "--filter", f"name=^{name}$", "--format", "{{.Name}}"],
                check=False,
            ).strip()
            if existing != name:
                # keep stub slots dual-stack capable so IPv6 address restore does not fail after link<->stub swap
                run_cmd(["docker", "network", "create", "--ipv6", "--driver", "bridge", "--internal", name])
            self.stub_networks.add(name)
        return name

    def _remove_stub_network(self, device: str, ge_idx: int) -> None:
        name = self._stub_network_name(device, ge_idx)
        run_cmd(["docker", "network", "rm", name], check=False)
        self.stub_networks.discard(name)

    def remove_link(self, link_name: str, *, strict: bool = True) -> None:
        """删除 link：原位换成 stub 占位，让容器内 ethN 槽位保留 carrier，ping 不通但接口保持 UP。"""
        net_name = self._get_link_network(link_name)
        endpoints = self._get_link_endpoints(link_name)
        for device, if_name in endpoints:
            cname = self._container_name(device)
            ge_idx = parse_if_index(if_name)
            # 先 disconnect link，再 connect stub —— 此时 ethN 释放出来，docker 把新 stub 接口挑到同一个最小空位
            self._docker_network_disconnect(net_name, cname, strict=strict)
            stub_name = self._ensure_stub_network(device, ge_idx)
            self._docker_network_connect(stub_name, cname, strict=strict)
            self.slot_net.setdefault(device, {})[ge_idx] = stub_name
        self.link_connected.discard(str(link_name).strip())

    def add_link(self, link_name: str, *, strict: bool = True) -> None:
        """加 link：把占位的 stub 换成 link 网络，ethN 槽位保持不变。"""
        net_name = self._get_link_network(link_name)
        endpoints = self._get_link_endpoints(link_name)
        for device, if_name in endpoints:
            cname = self._container_name(device)
            ge_idx = parse_if_index(if_name)
            current = self.slot_net.setdefault(device, {}).get(ge_idx)
            stub_name = self._stub_network_name(device, ge_idx)
            # 如果当前槽位是 stub，先解绑并销毁这个私有 stub；随后 connect link 会填到同一个 ethN
            if current == stub_name:
                self._docker_network_disconnect(stub_name, cname, strict=strict)
                self._remove_stub_network(device, ge_idx)
            self._docker_network_connect(net_name, cname, strict=strict)
            self.slot_net[device][ge_idx] = net_name
        self.link_connected.add(str(link_name).strip())

    def disconnect_link(self, link_name: str, *, strict: bool = True) -> None:
        self.remove_link(link_name, strict=strict)

    def connect_link(self, link_name: str, *, strict: bool = True) -> None:
        self.add_link(link_name, strict=strict)

    def start(self, *, configure_interfaces: bool = True) -> None:
        run_cmd(["docker", "network", "create", "--ipv6", self.mgmt_net])

        # 1) Create paused containers. if_map.conf.gns3 由 image 自带的
        # GE-1..GE-8=eth1..eth8 提供,不再 bind mount(避免阻碍 dev swap-image)。
        for dev in self.devices:
            cname = self._container_name(dev)

            docker_run_cmd = [
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
                "/var/run/docker.sock:/var/run/docker.sock",
            ]
            if dev in self.cli_publish_ports:
                docker_run_cmd.extend(["-p", f"{self.cli_publish_ports[dev]}:3788"])
            docker_run_cmd.extend([self.image, "sleep", "infinity"])
            run_cmd(docker_run_cmd)
            self.container_names.append(cname)

        # 2) Create link networks.
        self.link_to_net.clear()
        for link in self.top["links"]:
            lname = str(link["name"])
            net_name = f"{self.prefix}-lnk-{sanitize_name(lname)}"
            run_cmd(["docker", "network", "create", "--ipv6", net_name])
            self.link_to_net[lname] = net_name
            self.link_networks.append(net_name)

        # 3) 按 GE-1..GE-4 顺序给每台设备挂网络：有 link 的挂 link 网络，没 link 的挂 stub。
        # 顺序必须严格按 ge_idx 升序，docker 才会把 ethN 的 N 分配成 1,2,3,4 与 if_map 对齐。
        for dev, eps in self.endpoints.items():
            cname = self._container_name(dev)
            link_by_ge = {parse_if_index(ep.if_name): ep for ep in eps}
            for ge_idx in range(1, GE_PORT_COUNT + 1):
                if ge_idx in link_by_ge:
                    net = self.link_to_net[link_by_ge[ge_idx].link_name]
                else:
                    net = self._ensure_stub_network(dev, ge_idx)
                run_cmd(["docker", "network", "connect", net, cname])
                self.slot_net[dev][ge_idx] = net
        self.link_connected = set(self.link_to_net.keys())

        # 4) Start netnexus process inside each container.
        for dev in self.devices:
            self._start_netnexus_process(self._container_name(dev))

        # 5) Connect CLI via management network IP.
        for dev in self.devices:
            cname = self._container_name(dev)
            mgmt_ip = get_container_network_ip(cname, self.mgmt_net)
            self._connect_cli(dev, mgmt_ip, timeout=self.connect_timeout)

        # 6) Optional interface base config from top.
        if configure_interfaces:
            for dev, eps in self.endpoints.items():
                cli_configure_interfaces(self.cli_map[dev], eps)

    def reboot_device(self, device: str, *, reconnect_timeout: int = 90) -> None:
        if device not in self.devices:
            raise ValueError(f"unknown device '{device}'")

        # Unified software reboot path (container/non-container):
        # DEV handles MODULE_SHUTDOWN notification before restarting modules.
        cli = self.cli_map.get(device)
        if cli is None:
            raise RuntimeError(f"device '{device}' has no active CLI session to trigger reboot")

        host = cli.host
        port = cli.port
        print(f"\n===== STEP: Reboot software on {device} =====", flush=True)
        reboot_triggered_at = time.time()
        try:
            cli.cmd("reboot", strict=False, timeout=min(5, self.cmd_timeout))
        except Exception:
            # Reboot usually closes CLI quickly; reconnect below decides final success.
            pass
        finally:
            cli.close()
            self.cli_map.pop(device, None)

        deadline = time.time() + reconnect_timeout
        last_err: Exception | None = None
        while time.time() < deadline:
            # Avoid reconnecting too early while reboot worker is still about to restart modules.
            if time.time() < reboot_triggered_at + 2.0:
                time.sleep(0.2)
                continue
            new_cli: NetNexusCli | None = None
            try:
                new_cli = NetNexusCli(host, port, device, cmd_timeout=self.cmd_timeout, verbose=self.verbose)
                new_cli.connect(timeout=min(10, max(2, int(deadline - time.time()))))

                # Two probes reduce false-positive reconnect against pre-reboot/unstable state.
                new_cli.cmd("show version", strict=False, timeout=min(8, self.cmd_timeout))
                time.sleep(1)
                new_cli.cmd("show version", strict=False, timeout=min(8, self.cmd_timeout))

                self.cli_map[device] = new_cli

                # 等待所有模块 Phase=READY 且 IPC=up，再返回给调用方
                from module_runner import wait_device_modules_ready
                remaining = max(10, int(deadline - time.time()))
                wait_device_modules_ready(self, device, timeout=remaining)
                return
            except Exception as exc:
                last_err = exc
                try:
                    if new_cli is not None:
                        new_cli.close()
                        self.cli_map.pop(device, None)
                except Exception:
                    pass
                time.sleep(1)

        raise RuntimeError(
            f"{device}: failed to reconnect after reboot within {reconnect_timeout}s: {last_err}"
        )

    def exec_cmd(self, device: str, command: str, *, timeout: int | None = None, strict: bool = True) -> str:
        cli = self.cli_map.get(device)
        if cli is None:
            raise ValueError(f"unknown or disconnected device '{device}'")
        return cli.cmd(command, timeout=timeout, strict=strict)

    def disable_pager_for_all_sessions(self) -> None:
        for dev in sorted(self.devices.keys()):
            cli = self.cli_map.get(dev)
            if cli is None:
                raise RuntimeError(f"device '{dev}' has no active CLI session")
            cli.disable_pager(timeout=min(self.cmd_timeout, 8))

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
            stub_list = sorted(self.stub_networks)
            print(f"Networks: {', '.join([self.mgmt_net] + self.link_networks + stub_list)}")
            return

        for name in self.container_names:
            run_cmd(["docker", "rm", "-f", name], check=False)
        for net in self.link_networks:
            run_cmd(["docker", "network", "rm", net], check=False)
        for net in list(self.stub_networks):
            run_cmd(["docker", "network", "rm", net], check=False)
        self.stub_networks.clear()
        run_cmd(["docker", "network", "rm", self.mgmt_net], check=False)

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
        plan = f"ip address {ep.ip} {ep.prefix}"
        if ep.ip6:
            plan = f"{plan}; ipv6 address {ep.ip6} {ep.prefix6}"
        print(f"[{cli.name}] apply interface {ep.if_name}: {plan}, no shutdown", flush=True)
        cli.cmd(f"if {ep.if_name}")
        cli.cmd(f"ip address {ep.ip} {ep.prefix}")
        if ep.ip6:
            cli.cmd(f"ipv6 address {ep.ip6} {ep.prefix6}")
        cli.cmd("no shutdown")
        cli.cmd("exit")
    cli.cmd("end")


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
    parser = argparse.ArgumentParser(description="Run generic NetNexus topology runtime smoke test")
    parser.add_argument("--top", required=True, help="topology file path (.yaml/.yml/.json)")
    parser.add_argument("--image", required=False, help="docker image tag")
    parser.add_argument("--prefix", default=f"nn-top-{os.getpid()}", help="resource name prefix")
    parser.add_argument("--keep", action="store_true", help="keep containers/networks for debugging")
    parser.add_argument("--cmd-timeout", type=int, default=20, help="CLI command timeout seconds")
    parser.add_argument("--connect-timeout", type=int, default=90, help="CLI initial connect timeout seconds")
    parser.add_argument("--verbose", action="store_true", help="print command-level debug output")
    args = parser.parse_args()

    top = load_topology(Path(args.top))
    validate_top(top)

    image = args.image or str(top.get("image", "")).strip()
    if not image:
        raise SystemExit("image is required (use --image or top.image)")

    rt = TopologyRuntime(
        top=top,
        image=image,
        prefix=args.prefix,
        keep=args.keep,
        cmd_timeout=args.cmd_timeout,
        connect_timeout=args.connect_timeout,
        verbose=args.verbose,
    )
    failed = False

    try:
        print("\n===== STEP: Runtime startup =====", flush=True)
        rt.start(configure_interfaces=True)

        print("\n===== STEP: CLI probe =====", flush=True)
        for dev in sorted(rt.devices.keys()):
            out = rt.exec_cmd(dev, "show version", strict=False)
            print(f"[{dev}] show version ok ({len(out.strip())} chars)")

        print("Topology runtime smoke test passed.")
        return 0
    except Exception as exc:
        failed = True
        print(f"ERROR: {exc}", file=sys.stderr)
        dump_logs(rt.container_names)
        return 1
    finally:
        rt.close(failed=failed)


if __name__ == "__main__":
    sys.exit(main())
