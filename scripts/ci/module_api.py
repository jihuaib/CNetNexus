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
                local_cidr6 = str(local.get("cidr6", ""))
                peer_cidr6 = str(peer.get("cidr6", ""))

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

                local_ip6 = ""
                prefix6 = 0
                peer_ip6 = ""
                if local_cidr6 and peer_cidr6:
                    try:
                        local_iface6 = ipaddress.ip_interface(local_cidr6)
                        peer_iface6 = ipaddress.ip_interface(peer_cidr6)
                        if local_iface6.version == 6 and peer_iface6.version == 6:
                            local_ip6 = str(local_iface6.ip)
                            prefix6 = int(local_iface6.network.prefixlen)
                            peer_ip6 = str(peer_iface6.ip)
                    except Exception:
                        local_ip6 = ""
                        prefix6 = 0
                        peer_ip6 = ""

                device_eps[local_dev].append(
                    {
                        "link_name": link_name,
                        "device": local_dev,
                        "if_name": local_if_name,
                        "ip": local_ip,
                        "mask": mask,
                        "prefix": prefix,
                        "cidr": local_cidr,
                        "ip6": local_ip6,
                        "prefix6": prefix6,
                        "cidr6": local_cidr6,
                        "peer": str(peer.get("device", "")),
                        "peer_if": peer_if_name,
                        "peer_ip": peer_ip,
                        "peer_ip6": peer_ip6,
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
                    "ip6": str(first.get("ip6", "")),
                    "prefix6": int(first.get("prefix6", 0)),
                    "cidr6": str(first.get("cidr6", "")),
                    "peer": str(first.get("peer", "")),
                    "peer_if": str(first.get("peer_if", "")),
                    "peer_ip": str(first.get("peer_ip", "")),
                    "peer_ip6": str(first.get("peer_ip6", "")),
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
                    "ip6": "",
                    "prefix6": 0,
                    "cidr6": "",
                    "peer": "",
                    "peer_if": "",
                    "peer_ip": "",
                    "peer_ip6": "",
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
        root.set("ip6", str(first_dev.ip6))
        root.set("prefix6", int(first_dev.prefix6))
        root.set("cidr6", str(first_dev.cidr6))
    else:
        root.set("device_name", "")
        root.set("if_name", "")
        root.set("ip", "")
        root.set("mask", "")
        root.set("prefix", 0)
        root.set("cidr", "")
        root.set("ip6", "")
        root.set("prefix6", 0)
        root.set("cidr6", "")

    return root


g_top = _GlobalTop()
_last_step_title: str | None = None
_failed_step_title: str | None = None


def load_global_top(top: dict[str, Any]) -> None:
    g_top.load(top)


def reset_last_step() -> None:
    global _last_step_title, _failed_step_title
    _last_step_title = None
    _failed_step_title = None


def get_last_step() -> str | None:
    return _last_step_title


def mark_step_failed(step_title: str | None = None) -> None:
    global _failed_step_title
    if _failed_step_title:
        return
    candidate = step_title or _last_step_title
    if candidate:
        _failed_step_title = candidate


def get_failed_step() -> str | None:
    return _failed_step_title


def step(title: str) -> None:
    global _last_step_title
    normalized = str(title).strip() or "Step"
    _last_step_title = normalized
    print(f"\n===== STEP: {normalized} =====", flush=True)


def require_devices(top: dict, required: Iterable[str]) -> None:
    devices = top.get("devices")
    if not isinstance(devices, dict) or not devices:
        raise ValueError("top.devices must be a non-empty mapping")

    have = set(devices.keys())
    need = set(required)
    missing = sorted(need - have)
    if missing:
        raise ValueError(f"topology missing required devices: {', '.join(missing)}")


def count_occurrences(output: str, token: str) -> int:
    return output.count(token)


def _normalize_ws_text(text: str) -> str:
    """
    Normalize horizontal whitespace in each line:
    - collapse runs of spaces/tabs to one space
    - trim line edges
    Newline boundaries are preserved.
    """
    if not text:
        return ""
    return "\n".join(re.sub(r"[ \t]+", " ", line).strip() for line in text.splitlines())


def _normalize_count_map(raw: object) -> dict[str, int]:
    if not isinstance(raw, dict):
        return {}

    out: dict[str, int] = {}
    for key, value in raw.items():
        out[str(key)] = int(value)
    return out


def check_output(
    output: str,
    *,
    contains: Iterable[str] = (),
    not_contains: Iterable[str] = (),
    regex: Iterable[str] = (),
    not_regex: Iterable[str] = (),
    count: dict[str, int] | None = None,
    count_ge: dict[str, int] | None = None,
    count_le: dict[str, int] | None = None,
    normalize_whitespace: bool = True,
) -> list[str]:
    violations: list[str] = []
    plain_output = _normalize_ws_text(output) if normalize_whitespace else output

    for token in contains:
        t = str(token)
        t_match = _normalize_ws_text(t) if normalize_whitespace else t
        if t_match not in plain_output:
            violations.append(f"missing '{t}'")

    for token in not_contains:
        t = str(token)
        t_match = _normalize_ws_text(t) if normalize_whitespace else t
        if t_match in plain_output:
            violations.append(f"unexpected '{t}'")

    for pattern in regex:
        p = str(pattern)
        if re.search(p, output, flags=re.MULTILINE) is None:
            violations.append(f"regex not matched /{p}/")

    for pattern in not_regex:
        p = str(pattern)
        if re.search(p, output, flags=re.MULTILINE) is not None:
            violations.append(f"regex unexpectedly matched /{p}/")

    for token, expected in (count or {}).items():
        token_match = _normalize_ws_text(token) if normalize_whitespace else token
        got = count_occurrences(plain_output, token_match)
        if got != expected:
            violations.append(f"count('{token}') expect={expected} got={got}")

    for token, expected in (count_ge or {}).items():
        token_match = _normalize_ws_text(token) if normalize_whitespace else token
        got = count_occurrences(plain_output, token_match)
        if got < expected:
            violations.append(f"count('{token}') expect>={expected} got={got}")

    for token, expected in (count_le or {}).items():
        token_match = _normalize_ws_text(token) if normalize_whitespace else token
        got = count_occurrences(plain_output, token_match)
        if got > expected:
            violations.append(f"count('{token}') expect<={expected} got={got}")

    return violations


def cmd(
    rt: TopologyRuntime,
    device: str,
    command: str,
    *,
    strict: bool = True,
    timeout: int | None = None,
) -> str:
    try:
        return execCmd(rt, device).exec(command, strict=strict, timeout=timeout)
    except Exception:
        mark_step_failed()
        raise


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


def cmd_query_help(
    rt: TopologyRuntime,
    device: str,
    partial: str,
    *,
    timeout: int | None = None,
) -> str:
    """Send ``<partial>?`` to the device CLI and return the help text."""
    try:
        return execCmd(rt, device).query_help(partial, timeout=timeout)
    except Exception:
        mark_step_failed()
        raise


def reboot_device(rt: TopologyRuntime, device: str, *, timeout: int = 90) -> None:
    rt.reboot_device(device, reconnect_timeout=timeout)


def remove_link(rt: TopologyRuntime, link_name: str, *, strict: bool = True) -> None:
    try:
        rt.remove_link(link_name, strict=strict)
    except Exception:
        mark_step_failed()
        raise


def add_link(rt: TopologyRuntime, link_name: str, *, strict: bool = True) -> None:
    try:
        rt.add_link(link_name, strict=strict)
    except Exception:
        mark_step_failed()
        raise


def delete_link(rt: TopologyRuntime, link_name: str, *, strict: bool = True) -> None:
    remove_link(rt, link_name, strict=strict)


def disconnect_link(rt: TopologyRuntime, link_name: str, *, strict: bool = True) -> None:
    remove_link(rt, link_name, strict=strict)


def connect_link(rt: TopologyRuntime, link_name: str, *, strict: bool = True) -> None:
    add_link(rt, link_name, strict=strict)


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
      - contains: list[str]       (all substrings must appear)
      - not_contains: list[str]   (none of substrings should appear)
      - regex: list[str]          (all regex patterns must match)
      - not_regex: list[str]      (none of regex patterns should match)
      - count: dict[str, int]     (exact substring occurrence count)
      - count_ge: dict[str, int]  (minimum substring occurrence count)
      - count_le: dict[str, int]  (maximum substring occurrence count)
      - normalize_whitespace: bool (default True, contains/not_contains/count ignore space/tab run length)
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
            not_tokens = [str(x) for x in chk.get("not_contains", [])]
            regex_patterns = [str(x) for x in chk.get("regex", [])]
            not_regex_patterns = [str(x) for x in chk.get("not_regex", [])]
            count = _normalize_count_map(chk.get("count", {}))
            count_ge = _normalize_count_map(chk.get("count_ge", {}))
            count_le = _normalize_count_map(chk.get("count_le", {}))
            normalize_whitespace = bool(chk.get("normalize_whitespace", True))
            label = str(chk.get("label", f"{device}: {command}"))

            out = cmd(rt, device, command, strict=False)
            last_out[label] = out

            violations = check_output(
                out,
                contains=tokens,
                not_contains=not_tokens,
                regex=regex_patterns,
                not_regex=not_regex_patterns,
                count=count,
                count_ge=count_ge,
                count_le=count_le,
                normalize_whitespace=normalize_whitespace,
            )
            if violations:
                pending += 1
                missing_detail.append(f"{label}: {'; '.join(violations)}")

        if pending == 0:
            return

        last_missing = missing_detail
        time.sleep(interval)

    detail = "\n".join(last_missing)
    output_dump = "\n\n".join([f"[{label}]\n{out}" for label, out in last_out.items()])
    mark_step_failed()
    step_title = _last_step_title or "Step"
    print(f"ERROR: step '{step_title}' check timeout ({timeout}s)")
    if detail:
        print(f"ERROR: unsatisfied checks:\n{detail}")
    if output_dump:
        print(f"ERROR: last outputs:\n{output_dump}")
    raise RuntimeError(
        f"checks not satisfied within {timeout}s\n{detail}\n\nlast outputs:\n{output_dump}"
    )


def wait_check(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    timeout: int,
    interval: int = 2,
    contains: Iterable[str] = (),
    not_contains: Iterable[str] = (),
    regex: Iterable[str] = (),
    not_regex: Iterable[str] = (),
    count: dict[str, int] | None = None,
    count_ge: dict[str, int] | None = None,
    count_le: dict[str, int] | None = None,
    normalize_whitespace: bool = True,
    label: str | None = None,
) -> None:
    check: dict[str, object] = {
        "device": device,
        "command": command,
        "contains": list(contains),
        "not_contains": list(not_contains),
        "regex": list(regex),
        "not_regex": list(not_regex),
        "count": dict(count or {}),
        "count_ge": dict(count_ge or {}),
        "count_le": dict(count_le or {}),
        "normalize_whitespace": normalize_whitespace,
    }
    if label:
        check["label"] = label
    wait_checks(rt, [check], timeout=timeout, interval=interval)


def wait_fib_route(
    rt: TopologyRuntime,
    *,
    device: str,
    afi: str,
    prefix_addr: str,
    prefix_len: int | str,
    expect_present: bool,
    nexthop: str | None = None,
    nh_type: str | None = None,
    installed: bool | None = None,
    skip_os: bool | None = None,
    timeout: int,
    interval: int = 2,
    label: str | None = None,
) -> None:
    prefix_len_int = int(prefix_len)
    network = ipaddress.ip_network(f"{prefix_addr}/{prefix_len_int}", strict=False)
    prefix = f"{network.network_address}/{prefix_len_int}"
    afi_value = str(afi).lower()
    if afi_value not in ("ipv4", "ipv6"):
        raise ValueError(f"unsupported FIB afi: {afi}")

    if not expect_present:
        nexthop = None
        nh_type = None
        installed = None
        skip_os = None

    nexthop_pat = r"\S+" if nexthop is None else re.escape(nexthop)
    nh_type_pat = r"\S+" if nh_type is None else re.escape(nh_type)
    installed_pat = r"\S+" if installed is None else ("yes" if installed else "no")
    skip_os_pat = r"\S+" if skip_os is None else ("yes" if skip_os else "no")
    row_regex = (
        rf"(?im)^\s*{afi_value}\s+{re.escape(prefix)}\s+{nexthop_pat}\s+"
        rf"{nh_type_pat}\s+\S+\s+-?\d+\s+-?\d+\s+{installed_pat}\s+{skip_os_pat}\s*$"
    )
    wait_check(
        rt,
        device=device,
        command=f"show fib {afi_value} {network.network_address} {prefix_len_int}",
        timeout=timeout,
        interval=interval,
        regex=[row_regex] if expect_present else (),
        not_regex=[row_regex] if not expect_present else (),
        label=label
        or f"{device} fib {afi_value} route {prefix} {'present' if expect_present else 'absent'}",
    )


def wait_fib_ipv4_route(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix_addr: str,
    prefix_len: int | str,
    expect_present: bool,
    nexthop: str | None = None,
    nh_type: str | None = None,
    installed: bool | None = None,
    skip_os: bool | None = None,
    timeout: int,
    interval: int = 2,
    label: str | None = None,
) -> None:
    wait_fib_route(
        rt,
        device=device,
        afi="ipv4",
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=expect_present,
        nexthop=nexthop,
        nh_type=nh_type,
        installed=installed,
        skip_os=skip_os,
        timeout=timeout,
        interval=interval,
        label=label,
    )


def wait_fib_ipv6_route(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix_addr: str,
    prefix_len: int | str,
    expect_present: bool,
    nexthop: str | None = None,
    nh_type: str | None = None,
    installed: bool | None = None,
    skip_os: bool | None = None,
    timeout: int,
    interval: int = 2,
    label: str | None = None,
) -> None:
    wait_fib_route(
        rt,
        device=device,
        afi="ipv6",
        prefix_addr=prefix_addr,
        prefix_len=prefix_len,
        expect_present=expect_present,
        nexthop=nexthop,
        nh_type=nh_type,
        installed=installed,
        skip_os=skip_os,
        timeout=timeout,
        interval=interval,
        label=label,
    )


def hold_check(
    rt: TopologyRuntime,
    *,
    device: str,
    command: str,
    duration: int,
    interval: int = 2,
    contains: Iterable[str] = (),
    not_contains: Iterable[str] = (),
    regex: Iterable[str] = (),
    not_regex: Iterable[str] = (),
    count: dict[str, int] | None = None,
    count_ge: dict[str, int] | None = None,
    count_le: dict[str, int] | None = None,
    normalize_whitespace: bool = True,
    label: str | None = None,
) -> None:
    """
    Assert output constraints keep holding for the full duration window.
    Any violation during the window fails immediately.
    """
    deadline = time.time() + max(duration, 0)
    target = label or f"{device}: {command}"
    last_out = ""

    while True:
        out = cmd(rt, device, command, strict=False)
        last_out = out
        violations = check_output(
            out,
            contains=contains,
            not_contains=not_contains,
            regex=regex,
            not_regex=not_regex,
            count=count,
            count_ge=count_ge,
            count_le=count_le,
            normalize_whitespace=normalize_whitespace,
        )
        if violations:
            mark_step_failed()
            raise RuntimeError(
                f"{target} violated during hold window {duration}s: {'; '.join(violations)}\n"
                f"last output:\n{out}"
            )

        if time.time() >= deadline:
            return

        time.sleep(interval)
