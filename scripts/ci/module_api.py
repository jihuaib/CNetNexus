#!/usr/bin/env python3
"""
Common helpers for CI module scripts.

Modules should focus on scenario commands and assertions, while shared
execution/wait boilerplate lives here.
"""

from __future__ import annotations

import ipaddress
import re
import time
from typing import Any, Iterable

from top_runner import TopologyRuntime, execCmd, parse_if_index


SAFE_ATTR_RE = re.compile(r"[^0-9A-Za-z_]+")


def _safe_attr(name: str) -> str:
    value = SAFE_ATTR_RE.sub("_", str(name))
    if not value:
        value = "_"
    if value[0].isdigit():
        value = f"_{value}"
    return value


def _if_sort_key(if_name: str) -> tuple[int, int | str]:
    try:
        return (0, parse_if_index(if_name))
    except Exception:
        return (1, if_name)


class _TopNode:
    def __init__(self, values: dict[str, Any] | None = None) -> None:
        self._values: dict[str, Any] = {}
        if values:
            for key, value in values.items():
                self.set(key, value)

    def _convert(self, value: Any) -> Any:
        if isinstance(value, _TopNode):
            return value
        if isinstance(value, dict):
            return _TopNode(value)
        if isinstance(value, list):
            return [self._convert(x) for x in value]
        return value

    def set(self, key: str, value: Any) -> None:
        converted = self._convert(value)
        self._values[key] = converted
        safe = _safe_attr(key)
        if safe not in self._values:
            self._values[safe] = converted

    def __getitem__(self, key: str) -> Any:
        if key in self._values:
            return self._values[key]
        safe = _safe_attr(key)
        if safe in self._values:
            return self._values[safe]
        raise KeyError(key)

    def __getattr__(self, name: str) -> Any:
        if name in self._values:
            return self._values[name]
        raise AttributeError(name)

    def get(self, key: str, default: Any = None) -> Any:
        try:
            return self[key]
        except KeyError:
            return default


class _GlobalTop:
    def __init__(self) -> None:
        self._root = _TopNode()

    def load(self, top: dict[str, Any]) -> None:
        self._root = _build_top_view(top)

    def __getitem__(self, key: str) -> Any:
        return self._root[key]

    def __getattr__(self, name: str) -> Any:
        return getattr(self._root, name)

    def get(self, key: str, default: Any = None) -> Any:
        return self._root.get(key, default)


def _build_top_view(top: dict[str, Any]) -> _TopNode:
    devices_raw = top.get("devices", {})
    links_raw = top.get("links", [])

    device_eps: dict[str, list[dict[str, Any]]] = {
        str(dev): [] for dev in (devices_raw.keys() if isinstance(devices_raw, dict) else [])
    }

    if isinstance(links_raw, list):
        for link in links_raw:
            if not isinstance(link, dict):
                continue
            link_name = str(link.get("name", ""))
            eps = link.get("endpoints", [])
            if not isinstance(eps, list) or len(eps) != 2:
                continue

            left = eps[0]
            right = eps[1]
            if not isinstance(left, dict) or not isinstance(right, dict):
                continue

            for local, peer in ((left, right), (right, left)):
                local_dev = str(local.get("device", ""))
                if not local_dev:
                    continue
                if local_dev not in device_eps:
                    device_eps[local_dev] = []

                local_if_name = str(local.get("if", ""))
                peer_if_name = str(peer.get("if", ""))
                local_cidr = str(local.get("cidr", ""))
                peer_cidr = str(peer.get("cidr", ""))

                try:
                    local_iface = ipaddress.ip_interface(local_cidr)
                    peer_iface = ipaddress.ip_interface(peer_cidr)
                    if local_iface.version != 4 or peer_iface.version != 4:
                        continue
                    local_ip = str(local_iface.ip)
                    prefix = int(local_iface.network.prefixlen)
                    mask = str(ipaddress.IPv4Network(f"0.0.0.0/{prefix}").netmask)
                    peer_ip = str(peer_iface.ip)
                except Exception:
                    continue

                device_eps[local_dev].append(
                    {
                        "link_name": link_name,
                        "device": local_dev,
                        "if_name": local_if_name,
                        "ip": local_ip,
                        "mask": mask,
                        "prefix": prefix,
                        "cidr": local_cidr,
                        "peer": str(peer.get("device", "")),
                        "peer_if": peer_if_name,
                        "peer_ip": peer_ip,
                    }
                )

    device_nodes: dict[str, _TopNode] = {}
    for dev, eps in device_eps.items():
        eps = sorted(eps, key=lambda x: _if_sort_key(str(x.get("if_name", ""))))
        if_map: dict[str, _TopNode] = {}
        for ep in eps:
            if_node = _TopNode(ep)
            if_name = str(ep.get("if_name", ""))
            if if_name:
                if_map[if_name] = if_node

        dev_fields: dict[str, Any] = {"name": dev, "interfaces": if_map, "ifs": if_map}
        if eps:
            first = eps[0]
            dev_fields.update(
                {
                    "if_name": str(first.get("if_name", "")),
                    "ip": str(first.get("ip", "")),
                    "mask": str(first.get("mask", "")),
                    "prefix": int(first.get("prefix", 0)),
                    "cidr": str(first.get("cidr", "")),
                    "peer": str(first.get("peer", "")),
                    "peer_if": str(first.get("peer_if", "")),
                    "peer_ip": str(first.get("peer_ip", "")),
                    "link_name": str(first.get("link_name", "")),
                }
            )
        else:
            dev_fields.update(
                {
                    "if_name": "",
                    "ip": "",
                    "mask": "",
                    "prefix": 0,
                    "cidr": "",
                    "peer": "",
                    "peer_if": "",
                    "peer_ip": "",
                    "link_name": "",
                }
            )

        for if_name, if_node in if_map.items():
            dev_fields[_safe_attr(if_name)] = if_node

        device_nodes[dev] = _TopNode(dev_fields)

    root = _TopNode({"devices": device_nodes})
    for dev, dev_node in device_nodes.items():
        root.set(dev, dev_node)

    first_dev_name = sorted(device_nodes.keys())[0] if device_nodes else ""
    if first_dev_name:
        first_dev = device_nodes[first_dev_name]
        root.set("device_name", first_dev_name)
        root.set("if_name", str(first_dev.if_name))
        root.set("ip", str(first_dev.ip))
        root.set("mask", str(first_dev.mask))
        root.set("prefix", int(first_dev.prefix))
        root.set("cidr", str(first_dev.cidr))
    else:
        root.set("device_name", "")
        root.set("if_name", "")
        root.set("ip", "")
        root.set("mask", "")
        root.set("prefix", 0)
        root.set("cidr", "")

    return root


g_top = _GlobalTop()


def load_global_top(top: dict[str, Any]) -> None:
    g_top.load(top)


def step(title: str) -> None:
    print(f"\n===== STEP: {title} =====", flush=True)


def require_devices(top: dict, required: Iterable[str]) -> None:
    devices = top.get("devices")
    if not isinstance(devices, dict) or not devices:
        raise ValueError("top.devices must be a non-empty mapping")

    have = set(devices.keys())
    need = set(required)
    missing = sorted(need - have)
    if missing:
        raise ValueError(f"topology missing required devices: {', '.join(missing)}")


def cmd(
    rt: TopologyRuntime,
    device: str,
    command: str,
    *,
    strict: bool = True,
    timeout: int | None = None,
) -> str:
    return execCmd(rt, device).exec(command, strict=strict, timeout=timeout)


def run_cmds(
    rt: TopologyRuntime,
    device: str,
    commands: Iterable[str],
    *,
    strict: bool = True,
    timeout: int | None = None,
) -> list[str]:
    outputs: list[str] = []
    for command in commands:
        outputs.append(cmd(rt, device, command, strict=strict, timeout=timeout))
    return outputs


def reboot_device(rt: TopologyRuntime, device: str, *, timeout: int = 90) -> None:
    rt.reboot_device(device, reconnect_timeout=timeout)


def wait_checks(
    rt: TopologyRuntime,
    checks: list[dict[str, object]],
    *,
    timeout: int,
    interval: int = 2,
) -> None:
    """
    Generic polling checker.

    Each check item:
      - device: str
      - command: str
      - contains: list[str]  (all substrings must appear)
      - label: str (optional, used in error text)
    """
    if not checks:
        return

    deadline = time.time() + timeout
    last_out: dict[str, str] = {}
    last_missing: list[str] = []

    while time.time() < deadline:
        pending = 0
        missing_detail: list[str] = []

        for chk in checks:
            device = str(chk["device"])
            command = str(chk["command"])
            tokens = [str(x) for x in chk.get("contains", [])]
            label = str(chk.get("label", f"{device}: {command}"))

            out = cmd(rt, device, command, strict=False)
            last_out[device] = out

            miss = [t for t in tokens if t not in out]
            if miss:
                pending += 1
                missing_detail.append(f"{label} missing: {', '.join(miss)}")

        if pending == 0:
            return

        last_missing = missing_detail
        time.sleep(interval)

    detail = "\n".join(last_missing)
    output_dump = "\n\n".join([f"[{dev}]\n{out}" for dev, out in last_out.items()])
    raise RuntimeError(
        f"checks not satisfied within {timeout}s\n{detail}\n\nlast outputs:\n{output_dump}"
    )
