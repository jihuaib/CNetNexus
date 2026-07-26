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
import fcntl
import ipaddress
import json
import os
import re
import shlex
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


PROMPT_RE = re.compile(br"^<[A-Za-z0-9_.-]+(?:\([^>\r\n]*\))?>", re.MULTILINE)
IF_RE = re.compile(r"^GE-(\d+)$")
PAGER_DISABLE_CMD = "terminal length 0"
CORE_DIR_ENV = "NN_CORE_DIR"
DEVICE_KIND_NETNEXUS = "netnexus"
DEVICE_KIND_FRR = "frr"
SUPPORTED_DEVICE_KINDS = {DEVICE_KIND_NETNEXUS, DEVICE_KIND_FRR}
DEFAULT_FRR_IMAGE = "netnexus-frr-ci:localtest"
FRR_IMAGE_ENV = "CNETNEXUS_FRR_IMAGE"
LLDP_GROUP_FWD_MASK = 1 << 14


def get_core_dump_dir() -> str | None:
    core_dir = os.environ.get(CORE_DIR_ENV, "").strip()
    return core_dir or None


def _strip_command_echo(text: str, command: str) -> str:
    """从 telnet 响应中剥离首个出现的命令回显行，保留实际输出。"""
    if not command:
        return text
    idx = text.find(command)
    if idx < 0:
        return text
    end = idx + len(command)
    if end < len(text) and text[end] == "\n":
        end += 1
    return text[:idx] + text[end:]

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


def docker_network_bridge_name(network_name: str) -> str | None:
    out = run_cmd(
        [
            "docker",
            "network",
            "inspect",
            "-f",
            "{{.Id}} {{index .Options \"com.docker.network.bridge.name\"}}",
            network_name,
        ],
        check=False,
    ).strip()
    if not out:
        return None

    parts = out.split()
    if len(parts) >= 2 and parts[1] and not parts[1].startswith("<"):
        return parts[1]
    if parts and len(parts[0]) >= 12:
        return f"br-{parts[0][:12]}"
    return None


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


def device_kind(cfg: dict[str, Any]) -> str:
    kind = str(cfg.get("kind", cfg.get("type", DEVICE_KIND_NETNEXUS))).strip().lower()
    return kind or DEVICE_KIND_NETNEXUS


def if_linux_name(if_name: str) -> str:
    return f"eth{parse_if_index(if_name)}"


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


class _ConsolePipe:
    """串口/console 传输：driving ``docker exec -i <ctr> netnexus-console`` 的管道。

    暴露与 telnetlib.Telnet 相同的 write/read_very_eager/close 子集，使 NetNexusCli
    其余逻辑无需改动。console 是容器内的 AF_UNIX socket（telnet 默认关闭后唯一可用入口），
    宿主无法直连，故借 docker exec 跑内置客户端经其 stdio 中转。
    """

    NETNEXUS_CONSOLE_BIN = "/opt/netnexus/bin/netnexus-console"

    def __init__(self, container: str) -> None:
        self.container = container
        self.proc = subprocess.Popen(
            ["docker", "exec", "-i", "-e", "NN_CONSOLE_SOCK=/opt/netnexus/run/console.sock", container, self.NETNEXUS_CONSOLE_BIN],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        # stdout 置非阻塞，使 read_very_eager 不阻塞（与 telnetlib 语义一致）
        fd = self.proc.stdout.fileno()
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    def write(self, data: bytes) -> None:
        try:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise EOFError(f"console pipe write failed: {exc}") from exc

    def read_very_eager(self) -> bytes:
        if self.proc.poll() is not None:
            # 客户端已退出（如 console.sock 尚未就绪）：视作连接关闭，触发上层重连/超时
            raise EOFError("netnexus-console process exited")
        try:
            # 直接 os.read 裸 fd（非阻塞）：无数据抛 BlockingIOError，EOF 返回 b""
            data = os.read(self.proc.stdout.fileno(), 65536)
        except BlockingIOError:
            return b""
        except OSError as exc:
            raise EOFError(f"console pipe read failed: {exc}") from exc
        if data == b"":
            raise EOFError("console stdout closed")
        return data

    def close(self) -> None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


class NetNexusCli:
    def __init__(
        self,
        host: str,
        port: int,
        name: str,
        cmd_timeout: int = 20,
        verbose: bool = False,
        log_commands: bool = True,
        container: str | None = None,
    ) -> None:
        self.host = host
        self.port = port
        self.name = name
        self.cmd_timeout = cmd_timeout
        self.verbose = verbose
        self.log_commands = log_commands
        # container 非空 → 走 console（串口）传输（docker exec netnexus-console）；
        # 否则回退 telnet（仅在显式使能 telnet 的场景）。
        self.container = container
        self.tn: telnetlib.Telnet | _ConsolePipe | None = None
        self._rx_buf = bytearray()
        self._last_prompt = f"<{self.name}>"

    def _open_transport(self) -> telnetlib.Telnet | _ConsolePipe:
        if self.container:
            return _ConsolePipe(self.container)
        return telnetlib.Telnet(self.host, self.port, timeout=5)

    def connect(self, timeout: int = 25) -> None:
        deadline = time.time() + timeout
        last_err: Exception | None = None
        while time.time() < deadline:
            try:
                self.tn = self._open_transport()
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

    def is_alive(self, probe_timeout: float = 2.0) -> bool:
        """非破坏性探活:发一个空行,等 prompt 回来。

        覆盖两种死法:
        - 全关:write 抛 BrokenPipeError;
        - 半关(对端发了 FIN 还没回 RST):read_very_eager 抛 EOFError 或拿不到 prompt 超时。
        """
        if not self.tn:
            return False
        try:
            self.tn.write(b"\n")
        except (BrokenPipeError, OSError, EOFError):
            return False
        try:
            self._read_until_prompt(timeout=max(1, int(probe_timeout)))
            return True
        except Exception:
            return False

    def cmd(self, command: str, timeout: int | None = None, strict: bool = True) -> str:
        if not self.tn:
            raise RuntimeError(f"{self.name}: CLI not connected")
        eff_timeout = timeout if timeout is not None else self.cmd_timeout
        if self.log_commands or self.verbose:
            print(f"{self._last_prompt} {command}")
        self.tn.write(command.encode("ascii") + b"\n")
        out = self._read_with_prompt_recovery(command=command, timeout=eff_timeout, expect_echo=command)
        text = out.replace("\r", "")
        display = _strip_command_echo(text, command)
        if self.log_commands or self.verbose:
            stripped = display.strip()
            if stripped:
                print(stripped)
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
            print(f"{self._last_prompt} {partial}? (help)")
        self.tn.write(partial.encode("ascii") + b"?")
        help_raw = self._read_with_prompt_recovery(
            command=f"{partial}?", timeout=eff_timeout, expect_echo=f"{partial}?"
        )
        help_text = help_raw.replace("\r", "")
        display = _strip_command_echo(help_text, f"{partial}?")
        if self.log_commands or self.verbose:
            stripped = display.strip()
            if stripped:
                print(stripped)
        cleanup = (b"\x7f" * len(partial)) + b"\n"
        self.tn.write(cleanup)
        try:
            self._read_until_prompt(timeout=eff_timeout)
        except Exception:
            pass
        return help_text

    def _read_with_prompt_recovery(
        self, command: str, timeout: int, expect_echo: str | None = None
    ) -> str:
        try:
            return self._read_until_prompt(timeout=timeout, expect_echo=expect_echo)
        except RuntimeError as first_exc:
            # In noisy console scenarios, the command may have succeeded but prompt
            # framing can be disrupted. Try a single newline to force a fresh prompt.
            if "timeout waiting prompt" not in str(first_exc):
                raise
            if self.verbose:
                print(f"!!! prompt timeout after '{command}', retry with newline")

            assert self.tn is not None
            self.tn.write(b"\n")
            recovery_timeout = min(4, max(2, timeout // 3))
            try:
                # 恢复路径不再要求 echo:此时已经无法保证后续 prompt 来自原命令
                recovered = self._read_until_prompt(timeout=recovery_timeout)
            except Exception:
                raise first_exc
            if self.verbose:
                print(f"!!! prompt recovered after newline")
            return recovered

    def _read_until_prompt(self, timeout: int, expect_echo: str | None = None) -> str:
        """读到 prompt 为止。

        若 ``expect_echo`` 非空,则要求先在 buffer 中找到本命令的回显,只有
        回显之后的字节才允许匹配 prompt。这样可以避免上一条命令滞留在
        buffer 里的 prompt(或 banner/MOTD 残留)被误认为本次回包,从而引
        发 off-by-one 的应答错位。
        """
        assert self.tn is not None
        deadline = time.time() + timeout
        echo_bytes = expect_echo.encode("ascii", errors="ignore") if expect_echo else b""
        echo_pos = -1  # echo 在当前 _rx_buf 中的起始下标(随 buffer 头截断而调整)

        while time.time() < deadline:
            if echo_bytes:
                if echo_pos < 0:
                    idx = self._rx_buf.find(echo_bytes)
                    if idx >= 0:
                        echo_pos = idx
                search_from = echo_pos + len(echo_bytes) if echo_pos >= 0 else None
            else:
                search_from = 0

            if search_from is not None:
                m = PROMPT_RE.search(self._rx_buf, search_from)
                if m is not None:
                    end = m.end()
                    data = bytes(self._rx_buf[:end])
                    self._last_prompt = m.group(0).decode("utf-8", errors="ignore")
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
                    print(f"... rx {len(chunk)} bytes {preview}{suffix}")
                self._rx_buf.extend(chunk)
                if len(self._rx_buf) > 262144:
                    drop = len(self._rx_buf) - 262144
                    if echo_pos >= 0:
                        # 已锁定 echo,可安全丢弃 echo 之前的字节
                        actual_drop = min(drop, echo_pos)
                        if actual_drop > 0:
                            del self._rx_buf[:actual_drop]
                            echo_pos -= actual_drop
                    elif not echo_bytes:
                        # 不需要 echo,旧逻辑:从头丢
                        del self._rx_buf[:drop]
                    # 否则 echo 还没出现,保留头部以便后续仍能匹配回显
                continue

            time.sleep(0.02)

        tail_raw = bytes(self._rx_buf[-512:])
        tail_txt = tail_raw.decode("utf-8", errors="ignore").replace("\r", "")
        tail_hex = tail_raw.hex(" ")
        dump_thread_stacks(None, f"cli_prompt_timeout-{self.name}")
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
    if not isinstance(links, list):
        raise ValueError("top.links must be a list")

    for dev, cfg in devices.items():
        if not isinstance(cfg, dict):
            raise ValueError(f"device '{dev}' config must be a mapping")
        kind = device_kind(cfg)
        if kind not in SUPPORTED_DEVICE_KINDS:
            supported = ", ".join(sorted(SUPPORTED_DEVICE_KINDS))
            raise ValueError(f"device '{dev}' has unsupported kind '{kind}', supported: {supported}")

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


_STACK_DUMP_IN_PROGRESS = False
_ACTIVE_RUNTIME: "TopologyRuntime | None" = None  # 由 TopologyRuntime.start 注册，供低层 raise 点回退


def dump_thread_stacks(rt: "TopologyRuntime | None", label: str) -> list[Path]:
    """超时点抓全部 netnexus 进程的多线程 backtrace 到 NN_STACKS_DIR。

    在 wait_modules_ready / wait_check{,s} 等等待逻辑发现要 raise 前调用一次，
    把每个容器里 netnexus-* 进程的 `thread apply all bt` 落到文件，便于排查死锁。
    best-effort：失败不抛、二级递归（dump 内部又触发等待超时）只做一次。
    """
    global _STACK_DUMP_IN_PROGRESS
    if _STACK_DUMP_IN_PROGRESS:
        return []
    if os.environ.get("NN_NO_STACK_DUMP", "").strip() in ("1", "true", "True"):
        return []
    dest_raw = os.environ.get("NN_STACKS_DIR", "").strip()
    if not dest_raw:
        return []
    if rt is None:
        rt = _ACTIVE_RUNTIME
    containers = list(getattr(rt, "container_names", []) or []) if rt is not None else []
    if not containers:
        return []
    _STACK_DUMP_IN_PROGRESS = True
    try:
        dest = Path(dest_raw)
        dest.mkdir(parents=True, exist_ok=True)
        safe_label = re.sub(r"[^0-9A-Za-z_.-]+", "_", label).strip("_")[:80] or "timeout"
        ts = time.strftime("%H%M%S")
        gdb_script = (
            'PIDS=$(pgrep -x -f "netnexus(-[a-z]+)?$" 2>/dev/null || true); '
            'if [ -z "$PIDS" ]; then echo "(no netnexus processes found)"; exit 0; fi; '
            'for pid in $PIDS; do '
            '  cmdline=$(tr -d "\\0" </proc/$pid/cmdline 2>/dev/null | head -c 200); '
            '  echo "============ pid=$pid cmdline=[$cmdline] ============"; '
            '  timeout 8 gdb -p $pid -batch '
            '    -ex "set pagination off" '
            '    -ex "set print thread-events off" '
            '    -ex "info threads" '
            '    -ex "thread apply all bt 30" '
            '    -ex "detach" 2>&1 || echo "(gdb failed for pid=$pid rc=$?)"; '
            'done'
        )
        written: list[Path] = []
        for cname in containers:
            out_path = dest / f"stacks-{cname}-{safe_label}-{ts}.txt"
            try:
                proc = subprocess.run(
                    ["docker", "exec", cname, "bash", "-c", gdb_script],
                    capture_output=True, text=True, timeout=90,
                )
                body = proc.stdout
                if proc.stderr:
                    body += "\n----- stderr -----\n" + proc.stderr
                if proc.returncode != 0:
                    body += f"\n----- docker exec rc={proc.returncode} -----\n"
                out_path.write_text(body)
                written.append(out_path)
            except Exception as e:
                try:
                    out_path.write_text(f"(stack dump failed: {e})")
                    written.append(out_path)
                except Exception:
                    pass
        if written:
            print(f"[stack-dump] {label}: saved {len(written)} file(s) to {dest}", flush=True)
        return written
    except Exception as e:
        print(f"[stack-dump] unexpected error: {e}", flush=True)
        return []
    finally:
        _STACK_DUMP_IN_PROGRESS = False


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
        self.device_kinds: dict[str, str] = {dev: device_kind(cfg) for dev, cfg in self.devices.items()}
        self.device_images: dict[str, str] = {
            dev: self._resolve_device_image(dev, cfg, image) for dev, cfg in self.devices.items()
        }
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
        self.container_to_device: dict[str, str] = {}
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

    def _resolve_device_image(self, device: str, cfg: dict[str, Any], default_image: str) -> str:
        explicit = str(cfg.get("image", "")).strip()
        if explicit:
            return explicit

        images = self.top.get("images", {})
        if not isinstance(images, dict):
            images = {}

        kind = device_kind(cfg)
        if kind == DEVICE_KIND_FRR:
            env_image = os.environ.get(FRR_IMAGE_ENV, "").strip()
            return (
                env_image
                or str(images.get(DEVICE_KIND_FRR, "")).strip()
                or str(self.top.get("frr_image", "")).strip()
                or DEFAULT_FRR_IMAGE
            )

        return str(images.get(DEVICE_KIND_NETNEXUS, "")).strip() or default_image

    def _device_kind(self, device: str) -> str:
        if device not in self.devices:
            raise ValueError(f"unknown device '{device}'")
        return self.device_kinds[device]

    def get_device_kind(self, device: str) -> str:
        return self._device_kind(device)

    def _is_netnexus(self, device: str) -> bool:
        return self._device_kind(device) == DEVICE_KIND_NETNEXUS

    def _is_frr(self, device: str) -> bool:
        return self._device_kind(device) == DEVICE_KIND_FRR

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
        core_dir = get_core_dump_dir()
        core_setup = ""
        if core_dir:
            quoted_core_dir = shlex.quote(core_dir)
            core_setup = f"mkdir -p {quoted_core_dir} && chmod 1777 {quoted_core_dir} 2>/dev/null || true; "

        run_cmd(
            [
                "docker",
                "exec",
                "-d",
                container_name,
                "/bin/bash",
                "-lc",
                (
                    "ulimit -c unlimited || true; "
                    f"{core_setup}"
                    "mkdir -p /opt/netnexus/log /opt/netnexus/log/asan /opt/netnexus/data /opt/netnexus/run && "
                    "export NN_WORK_DIR=/opt/netnexus && "
                    "export NN_CONSOLE_SOCK=/opt/netnexus/run/console.sock && "
                    "export LD_LIBRARY_PATH=/opt/netnexus/lib:${LD_LIBRARY_PATH} && "
                    "export ASAN_OPTIONS="
                    "\"${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=0:abort_on_error=0:"
                    "log_path=/opt/netnexus/log/asan/asan}\" && "
                    # 走 supervise.sh：DEV 异常退出后自动按 backoff 重起（≤10次/120s）。
                    # CI 用更紧的窗口让短时 crash 也能很快恢复，但仍保留上限以暴露持续性 bug。
                    "export NN_SUPERVISE_WINDOW_SEC=60 && "
                    "export NN_SUPERVISE_MAX_RETRIES=5 && "
                    "export NN_SUPERVISE_BACKOFF_SEC=1 && "
                    "exec /opt/netnexus/scripts/supervise.sh > /tmp/netnexus.log 2>&1"
                ),
            ]
        )

    def _start_frr_process(self, container_name: str) -> None:
        run_cmd(
            [
                "docker",
                "exec",
                "-d",
                container_name,
                "/bin/bash",
                "-lc",
                "exec /usr/local/bin/ci-start-frr.sh > /tmp/frr-ci-start.log 2>&1",
            ]
        )

    def _wait_frr_ready(self, device: str, *, timeout: int) -> None:
        cname = self._container_name(device)
        deadline = time.time() + timeout
        last_err = ""
        while time.time() < deadline:
            proc = subprocess.run(
                ["docker", "exec", cname, "vtysh", "-c", "show version"],
                text=True,
                capture_output=True,
            )
            if proc.returncode == 0:
                return
            last_err = (proc.stderr or proc.stdout or "").strip()
            time.sleep(1)
        raise RuntimeError(f"{device}: FRR vtysh not ready within {timeout}s: {last_err}")

    def _exec_frr_shell(
        self,
        device: str,
        command: str,
        *,
        timeout: int | None = None,
        strict: bool = True,
    ) -> str:
        cname = self._container_name(device)
        eff_timeout = timeout if timeout is not None else self.cmd_timeout
        print(f"<{device}:shell> {command}")
        proc = subprocess.run(
            ["docker", "exec", cname, "/bin/bash", "-lc", command],
            text=True,
            capture_output=True,
            timeout=eff_timeout,
        )
        text = proc.stdout or ""
        if proc.stderr:
            if text and not text.endswith("\n"):
                text += "\n"
            text += proc.stderr
        if text.strip():
            print(text.strip())
        if strict and proc.returncode != 0:
            raise RuntimeError(f"{device}: command failed ({proc.returncode}): {command}\n{text}")
        return text

    def _connect_cli(self, device: str, host: str, *, timeout: int) -> None:
        # 经串口/console 连接：telnet 默认关闭，console（容器内 unix socket）是唯一稳定入口。
        cli = NetNexusCli(
            host,
            0,
            device,
            cmd_timeout=self.cmd_timeout,
            verbose=self.verbose,
            container=self._container_name(device),
        )
        cli.connect(timeout=timeout)
        self.cli_map[device] = cli

    # `show dev modules` 输出行：id name phase port ipc pid（id 为十进制；pid 可能是 '-'）
    _MODULES_ROW_RE = re.compile(
        r"^\s*\d+\s+(?P<name>[A-Za-z0-9_-]+)\s+(?P<phase>[A-Za-z0-9_-]+)\s+\d+\s+(?P<ipc>[A-Za-z0-9_-]+)\s+\S+\s*$"
    )

    def wait_modules_ready(self, device: str, *, timeout: int = 60, interval: float = 1.0) -> None:
        """轮询 ``show dev modules`` 直到所有模块 Phase=READY/ON-DEMAND，IPC=up/down 匹配。

        必须在下发任何配置命令之前调用——sysname、接口配置等需要 DEV 自身和被涉及模块都已 READY。
        """
        deadline = time.monotonic() + timeout
        last_bad: list[str] = []
        last_out = ""
        while time.monotonic() < deadline:
            try:
                last_out = self.exec_cmd(device, "show dev modules", strict=False)
            except Exception as e:
                last_out = f"(exec_cmd failed: {e})"
                time.sleep(interval)
                continue
            rows = []
            for line in last_out.splitlines():
                m = TopologyRuntime._MODULES_ROW_RE.match(line)
                if m:
                    rows.append({"name": m.group("name"), "phase": m.group("phase"), "ipc": m.group("ipc")})
            if rows:
                bad = []
                for r in rows:
                    phase = r["phase"].upper()
                    ipc = r["ipc"].lower()
                    if phase == "READY" and ipc == "up":
                        continue
                    if phase == "ON-DEMAND" and ipc == "down":
                        continue
                    bad.append(f"{r['name']}(phase={r['phase']},ipc={r['ipc']})")
                if not bad:
                    return
                last_bad = bad
            time.sleep(interval)
        dump_thread_stacks(self, f"wait_modules_ready-{device}")
        raise RuntimeError(
            f"{device}: modules not ready within {timeout}s; pending: {', '.join(last_bad) or '(parse failed)'}\n{last_out}"
        )

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

    def _allow_lldp_on_bridge(self, network_name: str) -> None:
        """Allow LLDP link-local frames through the Docker bridge used by a CI link network."""
        bridge = docker_network_bridge_name(network_name)
        if not bridge:
            print(f"warn: failed to resolve docker bridge for network {network_name}; LLDP frames may be filtered", flush=True)
            return

        mask_path = Path("/sys/class/net") / bridge / "bridge" / "group_fwd_mask"
        if not mask_path.exists():
            print(f"warn: bridge group_fwd_mask not found for {bridge}; LLDP frames may be filtered", flush=True)
            return

        try:
            current = int(mask_path.read_text(encoding="ascii").strip() or "0", 0)
            desired = current | LLDP_GROUP_FWD_MASK
            if desired != current:
                mask_path.write_text(f"{desired}\n", encoding="ascii")
            return
        except OSError:
            pass
        except ValueError:
            pass

        script = (
            f"path=/sys/class/net/{shlex.quote(bridge)}/bridge/group_fwd_mask; "
            "cur=$(cat \"$path\" 2>/dev/null || echo 0); "
            f"printf '%d\\n' $((cur | {LLDP_GROUP_FWD_MASK})) > \"$path\""
        )
        try:
            run_cmd(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--privileged",
                    "--network",
                    "host",
                    self.image,
                    "sh",
                    "-c",
                    script,
                ]
            )
        except RuntimeError as exc:
            print(
                f"warn: failed to enable LLDP forwarding on bridge {bridge} ({network_name}): {exc}",
                flush=True,
            )

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
        global _ACTIVE_RUNTIME
        _ACTIVE_RUNTIME = self
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
                "--privileged",
            ]
            if self._is_netnexus(dev):
                docker_run_cmd.extend(
                    [
                        "-e",
                        "NN_WORK_DIR=/opt/netnexus",
                        "-e",
                        "LD_LIBRARY_PATH=/opt/netnexus/lib",
                        "-v",
                        "/var/run/docker.sock:/var/run/docker.sock",
                    ]
                )
                core_dir = get_core_dump_dir()
                if core_dir:
                    docker_run_cmd.extend(["--ulimit", "core=-1", "-v", f"{core_dir}:{core_dir}"])
                if dev in self.cli_publish_ports:
                    docker_run_cmd.extend(["-p", f"{self.cli_publish_ports[dev]}:23"])
            docker_run_cmd.extend([self.device_images[dev], "sleep", "infinity"])
            run_cmd(docker_run_cmd)
            self.container_names.append(cname)
            self.container_to_device[cname] = dev

        # 2) Create link networks.
        self.link_to_net.clear()
        for link in self.top["links"]:
            lname = str(link["name"])
            net_name = f"{self.prefix}-lnk-{sanitize_name(lname)}"
            run_cmd(["docker", "network", "create", "--ipv6", net_name])
            self._allow_lldp_on_bridge(net_name)
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
                self._docker_network_connect(net, cname, strict=True)
                self.slot_net[dev][ge_idx] = net
        self.link_connected = set(self.link_to_net.keys())

        # 4) Start device control-plane processes inside each container.
        for dev in self.devices:
            if self._is_netnexus(dev):
                self._start_netnexus_process(self._container_name(dev))
            elif self._is_frr(dev):
                self._start_frr_process(self._container_name(dev))

        # 5) Connect NetNexus CLI via management network IP; wait for FRR vtysh.
        for dev in self.devices:
            if self._is_netnexus(dev):
                cname = self._container_name(dev)
                mgmt_ip = get_container_network_ip(cname, self.mgmt_net)
                self._connect_cli(dev, mgmt_ip, timeout=self.connect_timeout)
            elif self._is_frr(dev):
                self._wait_frr_ready(dev, timeout=self.connect_timeout)

        # 5a) 在下发任何配置（sysname / 接口）前等所有模块 READY，否则配置会丢
        for dev in self.devices:
            if self._is_netnexus(dev):
                print(f"===== STEP: Wait modules ready on {dev} =====")
                self.wait_modules_ready(dev)
                # CLI may have gated connect()-time session setup until its
                # startup replay completed. Re-apply it after readiness.
                ready_cli = self.cli_map.get(dev)
                if ready_cli is None:
                    raise RuntimeError(f"{dev}: CLI disappeared during topology startup")
                ready_cli.cmd("end", strict=False, timeout=min(10, self.cmd_timeout))
                ready_cli.cmd(PAGER_DISABLE_CMD, strict=False, timeout=min(10, self.cmd_timeout))

        # 5b) Apply per-device sysname == top.yaml device key (so prompt 显示 r1 / r2 ...)
        for dev in self.devices:
            if not self._is_netnexus(dev):
                continue
            cli = self.cli_map.get(dev)
            if cli is None:
                continue
            try:
                cli.cmd("config", strict=False)
                cli.cmd(f"sysname {dev}", strict=False)
                cli.cmd("end", strict=False)
            except Exception as exc:
                print(f"warn: failed to set sysname on {dev}: {exc}", flush=True)

        # 6) Optional interface base config from top.
        if configure_interfaces:
            for dev, eps in self.endpoints.items():
                if self._is_netnexus(dev):
                    cli_configure_interfaces(self.cli_map[dev], eps)
                elif self._is_frr(dev):
                    frr_configure_interfaces(self, dev, eps)

    def reboot_device(self, device: str, *, reconnect_timeout: int = 90) -> None:
        if device not in self.devices:
            raise ValueError(f"unknown device '{device}'")
        if not self._is_netnexus(device):
            raise ValueError(f"reboot_device is only supported for NetNexus nodes, got {device}")

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
                new_cli = NetNexusCli(
                    host, port, device, cmd_timeout=self.cmd_timeout, verbose=self.verbose,
                    container=self._container_name(device),
                )
                new_cli.connect(timeout=min(10, max(2, int(deadline - time.time()))))

                # Two probes reduce false-positive reconnect against pre-reboot/unstable state.
                new_cli.cmd("show version", strict=False, timeout=min(8, self.cmd_timeout))
                time.sleep(1)
                new_cli.cmd("show version", strict=False, timeout=min(8, self.cmd_timeout))

                self.cli_map[device] = new_cli

                # 等待所有模块 Phase=READY 且 IPC=up，再返回给调用方
                remaining = max(10, int(deadline - time.time()))
                self.wait_modules_ready(device, timeout=remaining)
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

    def cold_reboot_device(self, device: str, *, reconnect_timeout: int = 90) -> None:
        """Restart the complete NetNexus control plane with a fresh supervisor.

        Unlike the in-process ``reboot`` command, this ends the supervisor and
        starts a new root ``netnexus`` process without ``NN_WARM_RESTART``.
        DB therefore executes its real boot path and consumes the selected
        startup/db or startup/cfg snapshot.  The Docker container and its
        attached topology networks remain intact.
        """
        if device not in self.devices:
            raise ValueError(f"unknown device '{device}'")
        if not self._is_netnexus(device):
            raise ValueError(f"cold_reboot_device is only supported for NetNexus nodes, got {device}")

        cli = self.cli_map.get(device)
        if cli is None:
            raise RuntimeError(f"device '{device}' has no active CLI session to cold reboot")

        cname = self._container_name(device)
        print(f"\n===== STEP: Cold reboot control plane on {device} =====", flush=True)
        cli.close()

        stopped = subprocess.run(
            ["docker", "exec", cname, "pkill", "-TERM", "-x", "supervise.sh"],
            text=True,
            capture_output=True,
        )
        if stopped.returncode not in (0, 1):
            raise RuntimeError(
                f"{device}: failed to stop NetNexus supervisor ({stopped.returncode}): "
                f"{stopped.stderr or stopped.stdout}"
            )

        stop_deadline = time.time() + min(30, reconnect_timeout)
        last_processes = ""
        while time.time() < stop_deadline:
            probe = subprocess.run(
                ["docker", "exec", cname, "ps", "-C", "supervise.sh", "-C", "netnexus", "-o", "comm="],
                text=True,
                capture_output=True,
            )
            if probe.returncode not in (0, 1):
                raise RuntimeError(
                    f"{device}: failed to inspect old control-plane processes "
                    f"({probe.returncode}): {probe.stderr or probe.stdout}"
                )
            last_processes = (probe.stdout or "").strip()
            if not last_processes:
                break
            time.sleep(0.2)
        else:
            raise RuntimeError(f"{device}: old control plane did not stop: {last_processes}")

        self._start_netnexus_process(cname)
        self.ensure_cli_alive(device, reconnect_timeout=reconnect_timeout)
        # connect() may have sent pager setup while startup/cfg replay was still
        # gated. wait_modules_ready() above only returns after the gate opens;
        # apply the session setup once more before handing control to a case.
        ready_cli = self.cli_map.get(device)
        if ready_cli is None:
            raise RuntimeError(f"{device}: CLI disappeared after cold reboot")
        ready_cli.cmd("end", strict=False, timeout=min(10, self.cmd_timeout))
        ready_cli.cmd(PAGER_DISABLE_CMD, strict=False, timeout=min(10, self.cmd_timeout))

    def ensure_cli_alive(self, device: str, *, reconnect_timeout: int = 30) -> bool:
        """探活 + 必要时重连 device 的 CLI 会话。

        case 之间 ACCESS 端可能因用户态 ``exit`` 关掉 telnet socket
        (新架构 access_session_t.close_requested 路径),老 cli 复用就会
        碰到 BrokenPipe。本方法在每个 case 起手前调用,确保会话可用。

        - 返回 True  : 会话原本就活着,未做任何动作;
        - 返回 False : 会话已死,完成了一次重连(含 wait_modules_ready)。

        ``reboot_device`` 走的是另一条专用路径,带 reboot 探针 + 较长超时,
        与此处的 lightweight reconnect 互不重叠。
        """
        if not self._is_netnexus(device):
            return True

        cli = self.cli_map.get(device)
        if cli is not None and cli.is_alive():
            return True

        host = cli.host if cli is not None else None
        port = cli.port if cli is not None else None
        if cli is not None:
            try:
                cli.close()
            except Exception:
                pass
        self.cli_map.pop(device, None)
        if host is None or port is None:
            raise RuntimeError(
                f"{device}: CLI session is dead and runtime has no host/port to reconnect"
            )

        print(f"===== STEP: Reconnect CLI to {device} (previous session closed) =====", flush=True)
        deadline = time.time() + reconnect_timeout
        last_err: Exception | None = None
        while time.time() < deadline:
            new_cli: NetNexusCli | None = None
            try:
                new_cli = NetNexusCli(
                    host, port, device, cmd_timeout=self.cmd_timeout, verbose=self.verbose,
                    container=self._container_name(device),
                )
                new_cli.connect(timeout=min(10, max(2, int(deadline - time.time()))))
                self.cli_map[device] = new_cli
                remaining = max(10, int(deadline - time.time()))
                self.wait_modules_ready(device, timeout=remaining)
                return False
            except Exception as exc:
                last_err = exc
                if new_cli is not None:
                    try:
                        new_cli.close()
                    except Exception:
                        pass
                    self.cli_map.pop(device, None)
                time.sleep(1)

        raise RuntimeError(
            f"{device}: failed to reconnect CLI within {reconnect_timeout}s: {last_err}"
        )

    def exec_cmd(self, device: str, command: str, *, timeout: int | None = None, strict: bool = True) -> str:
        if self._is_frr(device):
            return self._exec_frr_shell(device, command, timeout=timeout, strict=strict)

        cli = self.cli_map.get(device)
        if cli is None:
            raise ValueError(f"unknown or disconnected device '{device}'")
        return cli.cmd(command, timeout=timeout, strict=strict)

    def disable_pager_for_all_sessions(self) -> None:
        for dev in sorted(self.devices.keys()):
            if not self._is_netnexus(dev):
                continue
            cli = self.cli_map.get(dev)
            if cli is None:
                raise RuntimeError(f"device '{dev}' has no active CLI session")
            cli.disable_pager(timeout=min(self.cmd_timeout, 8))

    def on(self, device: str) -> DeviceExec:
        if device not in self.devices:
            raise ValueError(f"unknown device '{device}'")
        return DeviceExec(self, device)

    def get_container_log_specs(self, container_name: str) -> list[tuple[str, str]]:
        device = self.container_to_device.get(container_name)
        if not device:
            return [("/opt/netnexus/log", "modules")]
        if self._is_frr(device):
            return [("/var/log/frr", "frr")]
        return [("/opt/netnexus/log", "modules")]

    def clear_container_logs(self, container_name: str) -> None:
        device = self.container_to_device.get(container_name)
        if device and self._is_frr(device):
            clear_cmd = "if [ -d /var/log/frr ]; then find /var/log/frr -type f -exec truncate -s 0 {} +; fi"
        else:
            clear_cmd = "if [ -d /opt/netnexus/log ]; then find /opt/netnexus/log -type f -exec truncate -s 0 {} +; fi"
        proc = subprocess.run(
            ["docker", "exec", container_name, "/bin/bash", "-lc", clear_cmd],
            text=True,
            capture_output=True,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"failed to clear logs in container {container_name}: "
                f"rc={proc.returncode}, stdout={proc.stdout or ''}, stderr={proc.stderr or ''}"
            )

    def close(self, *, failed: bool = False) -> None:
        global _ACTIVE_RUNTIME
        if _ACTIVE_RUNTIME is self:
            _ACTIVE_RUNTIME = None
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
        print(f"apply interface {ep.if_name}: {plan}, no shutdown", flush=True)
        cli.cmd(f"if {ep.if_name}")
        cli.cmd(f"ip address {ep.ip} {ep.prefix}")
        if ep.ip6:
            cli.cmd(f"ipv6 address {ep.ip6} {ep.prefix6}")
        cli.cmd("no shutdown")
        cli.cmd("exit")
    cli.cmd("end")


def frr_configure_interfaces(rt: TopologyRuntime, device: str, endpoints: list[Endpoint]) -> None:
    print(f"\n===== STEP: Auto configure FRR/Linux interface IPs on {device} =====", flush=True)
    rt.exec_cmd(device, "sysctl -w net.ipv4.ip_forward=1 net.ipv6.conf.all.forwarding=1 >/dev/null", strict=False)
    for ep in endpoints:
        linux_if = if_linux_name(ep.if_name)
        plan = f"{linux_if}: {ep.ip}/{ep.prefix}"
        if ep.ip6:
            plan = f"{plan}; {ep.ip6}/{ep.prefix6}"
        print(f"apply interface {ep.if_name} ({linux_if}): {plan}, up", flush=True)
        rt.exec_cmd(device, f"ip link set dev {shlex.quote(linux_if)} up")
        rt.exec_cmd(device, f"ip -4 addr flush dev {shlex.quote(linux_if)} scope global", strict=False)
        rt.exec_cmd(device, f"ip -6 addr flush dev {shlex.quote(linux_if)} scope global", strict=False)
        rt.exec_cmd(device, f"ip addr replace {shlex.quote(ep.ip)}/{ep.prefix} dev {shlex.quote(linux_if)}")
        if ep.ip6:
            rt.exec_cmd(device, f"ip -6 addr replace {shlex.quote(ep.ip6)}/{ep.prefix6} dev {shlex.quote(linux_if)}")


def dump_logs(container_names: list[str]) -> None:
    for name in container_names:
        print(f"\n===== docker logs: {name} =====")
        proc = subprocess.run(["docker", "logs", name], text=True, capture_output=True)
        if proc.stdout:
            print(proc.stdout)
        if proc.stderr:
            print(proc.stderr, file=sys.stderr)
        inner = subprocess.run(
            [
                "docker",
                "exec",
                name,
                "/bin/bash",
                "-lc",
                "test -f /tmp/netnexus.log && cat /tmp/netnexus.log; "
                "test -f /tmp/frr-ci-start.log && cat /tmp/frr-ci-start.log; "
                "test -d /var/log/frr && find /var/log/frr -maxdepth 1 -type f -print -exec cat {} \\;",
            ],
            text=True,
            capture_output=True,
        )
        if inner.stdout.strip():
            print(f"----- device logs ({name}) -----")
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
            if rt._is_frr(dev):
                out = rt.exec_cmd(dev, "vtysh -c 'show version'", strict=False)
            else:
                out = rt.exec_cmd(dev, "show version", strict=False)
            print(f"show version on {dev}: ok ({len(out.strip())} chars)")

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
