#!/usr/bin/env python3
"""
Discover case directories under scripts/ci/modules, start topology runtime once per
case directory, run all check scripts in that case, then cleanup.
"""

from __future__ import annotations

import argparse
import contextlib
import difflib
import html
import importlib.util
import io
import json
import os
import re
import shutil
import shlex
import subprocess
import sys
import tempfile
import time
import traceback
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


CI_DIR = Path(__file__).resolve().parent
if str(CI_DIR) not in sys.path:
    sys.path.insert(0, str(CI_DIR))

from module_api import get_failed_step, get_last_step, load_global_top, reset_last_step  # noqa: E402
from top_runner import (  # noqa: E402
    DEVICE_KIND_FRR,
    DEVICE_KIND_NETNEXUS,
    PAGER_DISABLE_CMD,
    TopologyRuntime,
    load_topology,
    sanitize_name,
)


MAX_HTML_OUTPUT_CHARS = 200000
TOP_CANDIDATES = ("top.yaml", "top.yml", "top.json")
SHOW_CURRENT_CONFIG_CMD = "show current-configuration"
SHOW_VERSION_CMD = "show version"
FRR_SHOW_CURRENT_CONFIG_CMD = "vtysh -c 'show running-config'"
FRR_SHOW_VERSION_CMD = "vtysh -c 'show version'"
PROMPT_LINE_RE = re.compile(r"^\s*<NetNexus[^>]*>.*$")
MAX_CONFIG_DIFF_LINES = 300
STEP_MARKER_RE = re.compile(r"^(?:\[[^\]]+\]\s*)?\s*=+\s*STEP:\s*(.*?)\s*=+\s*$")
LOG_CMD_LINE_RE = re.compile(r"^(?:\[[^\]]+\]\s*)?\[[^\]]+\]\s+<[A-Za-z0-9_.-]+>\s+\S.*$")
LOG_ECHO_LINE_RE = re.compile(r"(?!.*)")
LOG_PROMPT_LINE_RE = re.compile(r"^(?:\[[^\]]+\]\s*)?<NetNexus[^>]*>\s*$")
LOG_STEP_LINE_RE = re.compile(r"^(?:\[[^\]]+\]\s*)?=+\s*STEP:.*$")
FAIL_STEP_HINTS = (
    "===== CHECK FAIL:",
    "Traceback (most recent call last):",
    "RuntimeError:",
)
WARN_STEP_HINTS = (
    "WARNING:",
    "疑似未清理本次脚本配置",
    "config drift",
)
CORE_DIR_ENV = "NN_CORE_DIR"


def _device_kind(rt: TopologyRuntime, dev: str) -> str:
    return rt.get_device_kind(dev)
TIMESTAMP_FMT = "%Y-%m-%dT%H:%M:%S.%fZ"
MODULE_ROW_RE = re.compile(
    r"^\s*(?P<id>\d+)\s+(?P<name>[A-Za-z0-9_-]+)\s+(?P<phase>[A-Za-z0-9_-]+)\s+(?P<port>\d+)\s+(?P<ipc>[A-Za-z0-9_-]+)(\s+(?P<pid>\S+))?\s*$"
)
MODULE_HEALTH_WAIT_TIMEOUT_SEC = 60
MODULE_HEALTH_WAIT_INTERVAL_SEC = 2


@dataclass
class CheckResult:
    case_dir: Path
    script: Path
    command: list[str]
    started_at: float
    ended_at: float
    returncode: int
    stdout: str
    stderr: str
    previous_script: str | None = None
    previous_status: str | None = None
    failed_step: str | None = None

    @property
    def duration_sec(self) -> float:
        return self.ended_at - self.started_at

    @property
    def status(self) -> str:
        return "PASS" if self.returncode == 0 else "FAIL"


class Tee(io.TextIOBase):
    def __init__(self, *targets: io.TextIOBase) -> None:
        self.targets = targets

    def write(self, s: str) -> int:
        for target in self.targets:
            target.write(s)
            target.flush()
        return len(s)

    def flush(self) -> None:
        for target in self.targets:
            target.flush()


class TimestampedBuffer(io.TextIOBase):
    """
    Capture text with per-line UTC timestamp prefix for report/log rendering.
    """

    def __init__(self) -> None:
        self._buf = io.StringIO()
        self._pending = ""

    def _now(self) -> str:
        return datetime.now(timezone.utc).strftime(TIMESTAMP_FMT)

    def _write_line(self, line: str) -> None:
        self._buf.write(f"[{self._now()}] {line}")

    def write(self, s: str) -> int:
        if not s:
            return 0

        text = self._pending + s
        self._pending = ""

        parts = text.splitlines(keepends=True)
        if parts and not (parts[-1].endswith("\n") or parts[-1].endswith("\r")):
            self._pending = parts.pop()

        for part in parts:
            self._write_line(part)
        return len(s)

    def flush(self) -> None:
        # Keep pending fragment until newline/finalize, no-op here.
        pass

    def getvalue(self) -> str:
        if self._pending:
            self._write_line(self._pending)
            self._pending = ""
        return self._buf.getvalue()


def format_timestamp(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()


def make_case_artifact_token(case_dir: Path) -> str:
    base_modules = (CI_DIR / "modules").resolve()
    case_abs = case_dir.resolve()
    try:
        rel = case_abs.relative_to(base_modules)
        return sanitize_name(str(rel).replace(os.sep, "-"))
    except ValueError:
        return sanitize_name(str(case_abs))


def make_script_log_token(index: int, script: Path) -> str:
    return f"{index:02d}-{sanitize_name(script.stem)}"


def check_script_label(script: Path, case_dir: Path | None = None) -> str:
    if case_dir is not None:
        try:
            return str(script.resolve().relative_to(case_dir.resolve()))
        except ValueError:
            pass
    return str(script)


def get_core_dump_dir() -> Path | None:
    raw = os.environ.get(CORE_DIR_ENV, "").strip()
    return Path(raw) if raw else None


def unique_dest_path(dest_dir: Path, name: str) -> Path:
    dest = dest_dir / name
    if not dest.exists():
        return dest

    stem = dest.stem
    suffix = dest.suffix
    for idx in range(1, 1000):
        candidate = dest_dir / f"{stem}.{idx}{suffix}"
        if not candidate.exists():
            return candidate
    raise RuntimeError(f"too many duplicate core dump names under {dest_dir}")


def copy_and_remove_core_file(src: Path, dest: Path) -> None:
    try:
        shutil.copy2(src, dest)
        src.unlink()
        return
    except PermissionError:
        pass

    subprocess.run(["sudo", "cp", "-a", "--", str(src), str(dest)], check=True, text=True, capture_output=True)
    subprocess.run(
        ["sudo", "chown", f"{os.getuid()}:{os.getgid()}", "--", str(dest)],
        check=True,
        text=True,
        capture_output=True,
    )
    subprocess.run(["sudo", "rm", "-f", "--", str(src)], check=True, text=True, capture_output=True)


def collect_core_dumps(dest_dir: Path) -> list[Path]:
    core_dir = get_core_dump_dir()
    if core_dir is None or not core_dir.is_dir():
        return []

    core_files = sorted(path for path in core_dir.iterdir() if path.is_file() and path.name.startswith("core."))
    if not core_files:
        return []

    dest_dir.mkdir(parents=True, exist_ok=True)
    collected: list[Path] = []
    errors: list[str] = []
    for src in core_files:
        dest = unique_dest_path(dest_dir, src.name)
        try:
            copy_and_remove_core_file(src, dest)
            collected.append(dest)
        except Exception as exc:
            errors.append(f"{src}: {exc}")

    if errors:
        err_path = unique_dest_path(dest_dir, "core-collector.err")
        err_path.write_text("\n".join(errors) + "\n", encoding="utf-8")
        collected.append(err_path)

    return collected


def find_top_file(case_dir: Path) -> Path | None:
    for name in TOP_CANDIDATES:
        path = case_dir / name
        if path.is_file():
            return path
    return None


def discover_case_dirs(modules_dir: Path) -> list[Path]:
    if not modules_dir.exists():
        return []

    case_dirs: list[Path] = []
    if find_top_file(modules_dir) is not None:
        case_dirs.append(modules_dir)

    for path in modules_dir.rglob("*"):
        if not path.is_dir():
            continue
        if find_top_file(path) is not None:
            case_dirs.append(path)
    return sorted(case_dirs)


def discover_case_scripts(case_dir: Path) -> list[Path]:
    return sorted(
        p
        for p in case_dir.glob("*.py")
        if p.is_file() and p.name != "__init__.py" and not p.name.startswith("_")
    )


def resolve_script_selector(selector: str, modules_dir: Path) -> Path:
    raw = Path(selector)
    candidates: list[Path] = []

    if raw.is_absolute():
        candidates.append(raw)
    else:
        candidates.append((modules_dir / raw).resolve())
        candidates.append((Path.cwd() / raw).resolve())

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    if (not raw.is_absolute()) and len(raw.parts) == 1:
        names = [raw.name]
        if raw.suffix != ".py":
            names.append(f"{raw.name}.py")

        matches: list[Path] = []
        for name in names:
            matches.extend(p.resolve() for p in modules_dir.rglob(name) if p.is_file())

        uniq = sorted(set(matches))
        if len(uniq) == 1:
            return uniq[0]
        if len(uniq) > 1:
            joined = ", ".join(str(p) for p in uniq)
            raise RuntimeError(f"ambiguous selector '{selector}', matched: {joined}")

    raise RuntimeError(f"script not found: {selector}")


def load_run_callable(script: Path):
    mod_name = "ci_case_" + sanitize_name(str(script.relative_to(CI_DIR))).replace("-", "_").replace(".", "_")
    spec = importlib.util.spec_from_file_location(mod_name, script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to import script: {script}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    run_fn = getattr(module, "run", None)
    if not callable(run_fn):
        raise RuntimeError(f"{script}: missing callable `run(rt, top)`")
    return run_fn


def wait_device_modules_ready(
    rt: TopologyRuntime,
    dev: str,
    *,
    timeout: int = MODULE_HEALTH_WAIT_TIMEOUT_SEC,
) -> None:
    """等待单台设备所有模块 Phase=READY 且 IPC=up。"""
    deadline = time.time() + timeout
    last_out = ""
    last_bad: list[str] = []
    last_parse_ok = False

    while time.time() < deadline:
        out = rt.exec_cmd(dev, "show dev modules", strict=False)
        last_out = out

        rows: list[dict[str, str]] = []
        for line in out.splitlines():
            m = MODULE_ROW_RE.match(line)
            if m:
                rows.append(
                    {
                        "name": m.group("name"),
                        "phase": m.group("phase"),
                        "ipc": m.group("ipc"),
                    }
                )

        if not rows:
            last_parse_ok = False
            time.sleep(MODULE_HEALTH_WAIT_INTERVAL_SEC)
            continue

        last_parse_ok = True
        # on-demand 模块未启动时 phase=ON-DEMAND + ipc=down 视为正常待命状态。
        # 框架 precheck 接受 READY/up 或 ON-DEMAND/down 这两种健康态。
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
            print(f"modules on {dev}: READY/up OK ({len(rows)} modules)")
            return

        last_bad = bad
        time.sleep(MODULE_HEALTH_WAIT_INTERVAL_SEC)

    if not last_parse_ok:
        raise RuntimeError(f"{dev}: failed to parse module table from 'show dev modules'\n{last_out}")
    raise RuntimeError(
        f"{dev}: modules not healthy (require Phase=READY and IPC=up): {', '.join(last_bad)}\n{last_out}"
    )


def ensure_device_modules_ready(rt: TopologyRuntime, top: dict[str, Any]) -> None:
    devices = top.get("devices", {})
    if not isinstance(devices, dict) or not devices:
        raise RuntimeError("top.devices must be a non-empty mapping for module precheck")

    print("===== STEP: Precheck device modules =====")
    for dev in sorted(devices.keys()):
        if _device_kind(rt, dev) != DEVICE_KIND_NETNEXUS:
            print(f"skip NetNexus module precheck on {dev} ({_device_kind(rt, dev)})")
            continue
        wait_device_modules_ready(rt, dev)


def ensure_cli_pager_disabled(rt: TopologyRuntime, top: dict[str, Any]) -> None:
    devices = top.get("devices", {})
    if not isinstance(devices, dict) or not devices:
        raise RuntimeError("top.devices must be a non-empty mapping for pager precheck")

    print("===== STEP: Disable CLI pager =====")
    rt.disable_pager_for_all_sessions()
    for dev in sorted(devices.keys()):
        if _device_kind(rt, dev) != DEVICE_KIND_NETNEXUS:
            continue
        print(f"pager disabled on {dev} via '{PAGER_DISABLE_CMD}'")


def normalize_cli_command_output(raw: str, command: str) -> str:
    lines: list[str] = []
    cmd = command.strip()
    for raw_line in raw.replace("\r", "").splitlines():
        stripped = raw_line.strip()
        if stripped == cmd:
            continue
        if PROMPT_LINE_RE.match(stripped):
            continue
        lines.append(raw_line.rstrip())
    return "\n".join(lines).strip()


def print_device_versions(rt: TopologyRuntime, top: dict[str, Any]) -> None:
    devices = top.get("devices", {})
    if not isinstance(devices, dict) or not devices:
        print("WARNING: skip version probe because top.devices is empty")
        return

    print("===== STEP: Print device versions =====")
    timeout = max(20, rt.cmd_timeout * 2)
    for dev in sorted(devices.keys()):
        show_cmd = FRR_SHOW_VERSION_CMD if _device_kind(rt, dev) == DEVICE_KIND_FRR else SHOW_VERSION_CMD
        try:
            out = rt.exec_cmd(dev, show_cmd, strict=False, timeout=timeout)
        except Exception as exc:
            print(f"WARNING: '{show_cmd}' on {dev} failed: {exc}")
            continue

        normalized = normalize_cli_command_output(out, show_cmd)
        print(f"{show_cmd} on {dev}:")
        print(normalized if normalized else "(empty output)")


def normalize_show_current_config(raw: str) -> str:
    lines: list[str] = []
    for raw_line in raw.replace("\r", "").splitlines():
        stripped = raw_line.strip()
        if not stripped:
            continue
        if stripped == "--More--":
            continue
        if stripped == SHOW_CURRENT_CONFIG_CMD:
            continue
        if PROMPT_LINE_RE.match(stripped):
            continue
        lines.append(raw_line.rstrip())
    return "\n".join(lines).strip()


def collect_show_current_config(rt: TopologyRuntime, top: dict[str, Any], *, stage: str) -> dict[str, str]:
    devices = top.get("devices", {})
    if not isinstance(devices, dict) or not devices:
        raise RuntimeError(f"top.devices must be a non-empty mapping for '{stage}' config snapshot")

    print(f"===== STEP: Snapshot running config ({stage}) =====")
    timeout = max(30, rt.cmd_timeout * 3)
    snapshots: dict[str, str] = {}
    for dev in sorted(devices.keys()):
        if _device_kind(rt, dev) == DEVICE_KIND_FRR:
            show_cmd = FRR_SHOW_CURRENT_CONFIG_CMD
        else:
            show_cmd = SHOW_CURRENT_CONFIG_CMD
        out = rt.exec_cmd(dev, show_cmd, strict=False, timeout=timeout)
        normalized = normalize_show_current_config(out)
        snapshots[dev] = normalized
        print(f"collected '{show_cmd}' on {dev} ({len(normalized.splitlines())} lines)")
    return snapshots


def diff_show_current_config(before: dict[str, str], after: dict[str, str]) -> dict[str, str]:
    drifts: dict[str, str] = {}
    for dev in sorted(set(before.keys()) | set(after.keys())):
        old = before.get(dev, "")
        new = after.get(dev, "")
        if old == new:
            continue

        diff_lines = list(
            difflib.unified_diff(
                old.splitlines(),
                new.splitlines(),
                fromfile=f"{dev}:before",
                tofile=f"{dev}:after",
                lineterm="",
            )
        )
        if len(diff_lines) > MAX_CONFIG_DIFF_LINES:
            omitted = len(diff_lines) - MAX_CONFIG_DIFF_LINES
            diff_lines = diff_lines[:MAX_CONFIG_DIFF_LINES] + [f"... ({omitted} more diff lines omitted)"]
        drifts[dev] = "\n".join(diff_lines) if diff_lines else "(configuration changed)"
    return drifts


def report_config_drift(drifts: dict[str, str]) -> None:
    print("===== STEP: Verify config cleanup =====")
    if not drifts:
        print("No configuration drift detected after check script.")
        return

    print("WARNING: Detected config drift after script; 疑似未清理本次脚本配置。")
    for dev in sorted(drifts.keys()):
        print(f"WARNING: ----- {dev} config diff -----")
        print(drifts[dev])


def run_check(
    script: Path,
    rt: TopologyRuntime,
    top: dict[str, Any],
    previous_result: CheckResult | None = None,
) -> CheckResult:
    command = ["run(rt, top)"]
    started = time.time()
    previous_script = check_script_label(previous_result.script, previous_result.case_dir) if previous_result else None
    previous_status = previous_result.status if previous_result else None

    out_buf = TimestampedBuffer()
    err_buf = TimestampedBuffer()
    tee_out = Tee(sys.stdout, out_buf)
    tee_err = Tee(sys.stderr, err_buf)

    rc = 0
    failed_step_title: str | None = None
    run_exc_logged = False
    try:
        load_global_top(top)
        run_fn = load_run_callable(script)
        with contextlib.redirect_stdout(tee_out), contextlib.redirect_stderr(tee_err):
            print(f"===== RUN CHECK: {script} =====")
            if previous_script is None:
                print("===== PREVIOUS CHECK: <none> =====")
            else:
                print(f"===== PREVIOUS CHECK: {previous_script} [{previous_status}] =====")
            load_global_top(top)
            ensure_device_modules_ready(rt, top)
            print_device_versions(rt, top)

            before_cfg = collect_show_current_config(rt, top, stage="before")
            run_failed = False
            reset_last_step()
            try:
                run_fn(rt, top)
            except Exception:
                run_failed = True
                run_exc_logged = True
                failed_step_title = get_failed_step() or get_last_step()
                if failed_step_title:
                    failed_at = f"module step '{failed_step_title}'"
                else:
                    failed_at = "before the first module step marker"
                print(f"ERROR: check script raised at {failed_at}")
                print("ERROR: runner will continue post-check cleanup/diff before final FAIL.")
                traceback.print_exc(file=sys.stderr)
                raise
            finally:
                try:
                    after_cfg = collect_show_current_config(rt, top, stage="after")
                    report_config_drift(diff_show_current_config(before_cfg, after_cfg))
                except Exception as cfg_exc:
                    print(f"WARNING: failed to run config cleanup check: {cfg_exc}")
                    if not run_failed:
                        raise
            print(f"===== CHECK PASS: {script} =====")
    except Exception:
        rc = 1
        with contextlib.redirect_stdout(tee_out), contextlib.redirect_stderr(tee_err):
            if failed_step_title and not run_exc_logged:
                print(f"ERROR: failing module step: {failed_step_title}")
            print("===== STEP: Final check status =====")
            print(f"===== CHECK FAIL: {script} =====")
            if not run_exc_logged:
                traceback.print_exc(file=sys.stderr)

    ended = time.time()
    return CheckResult(
        case_dir=script.parent,
        script=script,
        command=command,
        started_at=started,
        ended_at=ended,
        returncode=rc,
        stdout=out_buf.getvalue(),
        stderr=err_buf.getvalue(),
        previous_script=previous_script,
        previous_status=previous_status,
        failed_step=failed_step_title,
    )


def synth_failed_result(
    case_dir: Path,
    script: Path,
    err: str,
    previous_result: CheckResult | None = None,
) -> CheckResult:
    now = time.time()
    previous_script = check_script_label(previous_result.script, previous_result.case_dir) if previous_result else None
    previous_status = previous_result.status if previous_result else None
    return CheckResult(
        case_dir=case_dir,
        script=script,
        command=["run(rt, top)"],
        started_at=now,
        ended_at=now,
        returncode=1,
        stdout="",
        stderr=err,
        previous_script=previous_script,
        previous_status=previous_status,
    )


def collect_container_log_files(
    container: str,
    *,
    include_docker: bool,
    include_modules: bool,
    log_specs: list[tuple[str, str]] | None = None,
) -> dict[str, str]:
    files: dict[str, str] = {}

    if include_docker:
        docker_proc = subprocess.run(["docker", "logs", container], text=True, capture_output=True)
        docker_text = docker_proc.stdout or ""
        if docker_proc.stderr:
            if docker_text and not docker_text.endswith("\n"):
                docker_text += "\n"
            docker_text += "===== STDERR =====\n"
            docker_text += docker_proc.stderr
        if docker_proc.returncode != 0 and not docker_text.strip():
            docker_text = f"[collector] docker logs failed rc={docker_proc.returncode}\n"
        files["docker.log"] = docker_text

    if not include_modules:
        return files

    specs = log_specs or [("/opt/netnexus/log", "modules")]
    for src_path, out_prefix in specs:
        with tempfile.TemporaryDirectory(prefix="nn-ci-logs-") as tmpdir:
            tmp_path = Path(tmpdir)
            cp_proc = subprocess.run(
                ["docker", "cp", f"{container}:{src_path}/.", str(tmp_path)],
                text=True,
                capture_output=True,
            )
            if cp_proc.returncode != 0:
                files[f"{out_prefix}-copy.err"] = (
                    f"[collector] failed to copy {src_path} from container\n"
                    f"container={container}\n"
                    f"rc={cp_proc.returncode}\n"
                    f"stdout:\n{cp_proc.stdout or ''}\n"
                    f"stderr:\n{cp_proc.stderr or ''}\n"
                )
                continue

            for log_path in sorted(path for path in tmp_path.rglob("*") if path.is_file()):
                rel_path = log_path.relative_to(tmp_path).as_posix()
                files[f"{out_prefix}/{rel_path}"] = log_path.read_text(encoding="utf-8", errors="replace")

    return files


def write_container_log_files(
    container_out: Path,
    files: dict[str, str],
    *,
    script_token: str | None = None,
    skip_empty: bool = False,
) -> list[Path]:
    base_out = container_out if script_token is None else container_out / "scripts" / script_token
    exported: list[Path] = []

    for rel_path, text in sorted(files.items()):
        if skip_empty and not text.strip():
            continue
        out_path = base_out / Path(rel_path)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(text, encoding="utf-8")
        exported.append(out_path)

    return exported


def export_case_container_logs(
    rt: TopologyRuntime,
    case_dir: Path,
    out_root: Path,
    *,
    script_token: str | None = None,
    include_docker: bool = True,
    include_modules: bool = True,
    skip_empty: bool = False,
) -> list[Path]:
    """
    Export per-container logs for one case.

    Layout:
      <out_root>/<case-token>/<container>/docker.log
      <out_root>/<case-token>/<container>/modules/*.log
      <out_root>/<case-token>/<container>/scripts/<script-token>/modules/*.log
    """
    if not rt.container_names:
        return []

    case_out = out_root / make_case_artifact_token(case_dir)
    case_out.mkdir(parents=True, exist_ok=True)

    exported: list[Path] = []
    for container in rt.container_names:
        container_out = case_out / sanitize_name(container)
        log_specs = rt.get_container_log_specs(container)
        files = collect_container_log_files(
            container,
            include_docker=include_docker,
            include_modules=include_modules,
            log_specs=log_specs,
        )
        exported.extend(
            write_container_log_files(
                container_out,
                files,
                script_token=script_token,
                skip_empty=skip_empty,
            )
        )

    return exported


def format_artifact_paths(paths: list[Path], root: Path) -> str:
    lines: list[str] = []
    for path in sorted(paths):
        try:
            label = path.relative_to(root)
        except ValueError:
            label = path
        lines.append(f"  {label}")
    return "\n".join(lines)


def clear_case_container_module_logs(rt: TopologyRuntime) -> None:
    if not rt.container_names:
        return

    for container in rt.container_names:
        rt.clear_container_logs(container)


def run_case(
    case_dir: Path,
    image_arg: str | None,
    cmd_timeout: int,
    connect_timeout: int,
    verbose: bool,
    keep: bool,
    container_logs_dir: Path,
    core_dumps_dir: Path,
    scripts_override: list[Path] | None = None,
) -> list[CheckResult]:
    scripts = sorted(scripts_override) if scripts_override is not None else discover_case_scripts(case_dir)
    if not scripts:
        return [
            synth_failed_result(
                case_dir,
                case_dir / "<no-check-script>",
                f"no check scripts found in case directory: {case_dir}",
            )
        ]

    top_file = find_top_file(case_dir)
    if top_file is None:
        err = f"case has no top file ({', '.join(TOP_CANDIDATES)}): {case_dir}"
        return [synth_failed_result(case_dir, script, err) for script in scripts]

    top = load_topology(top_file)
    image = image_arg or str(top.get("image", "")).strip()
    if not image:
        return [
            synth_failed_result(
                case_dir,
                script,
                f"image is required (use --image or set top.image) in case {case_dir}",
            )
            for script in scripts
        ]

    prefix = sanitize_name(f"nn-case-{make_case_artifact_token(case_dir)}-{os.getpid()}")

    results: list[CheckResult] = []
    case_failed = False
    module_logs_cleared = False
    rt: TopologyRuntime | None = None
    startup_stdout = ""
    startup_stderr = ""
    startup_out_buf: TimestampedBuffer | None = None
    startup_err_buf: TimestampedBuffer | None = None

    try:
        print(f"\n===== START CASE: {case_dir} =====")
        rt = TopologyRuntime(
            top=top,
            image=image,
            prefix=prefix,
            keep=keep,
            cmd_timeout=cmd_timeout,
            connect_timeout=connect_timeout,
            verbose=verbose,
        )
        startup_out_buf = TimestampedBuffer()
        startup_err_buf = TimestampedBuffer()
        startup_tee_out = Tee(sys.stdout, startup_out_buf)
        startup_tee_err = Tee(sys.stderr, startup_err_buf)
        with contextlib.redirect_stdout(startup_tee_out), contextlib.redirect_stderr(startup_tee_err):
            print("===== STEP: Runtime startup =====")
            rt.start(configure_interfaces=True)
        startup_stdout = startup_out_buf.getvalue()
        startup_stderr = startup_err_buf.getvalue()
        startup_cores = collect_core_dumps(core_dumps_dir / make_case_artifact_token(case_dir) / "startup")
        if startup_cores:
            print(f"Collected startup core dumps for case '{case_dir.name}' -> {core_dumps_dir} ({len(startup_cores)} files)")

        previous_result: CheckResult | None = None
        for idx, script in enumerate(scripts, start=1):
            module_logs_cleared = False
            script_token = make_script_log_token(idx, script)
            result = run_check(script, rt, top, previous_result=previous_result)
            if idx == 1:
                prefix_parts: list[str] = []
                if startup_stdout:
                    prefix_parts.append(startup_stdout)
                if startup_stderr:
                    prefix_parts.append(f"===== STEP: Runtime startup stderr =====\n{startup_stderr}")
                if prefix_parts:
                    result.stdout = "\n\n".join(prefix_parts) + ("\n" if result.stdout else "") + result.stdout
            results.append(result)
            if result.returncode != 0:
                case_failed = True
            previous_result = result
            try:
                script_cores = collect_core_dumps(core_dumps_dir / make_case_artifact_token(case_dir) / script_token)
                if script_cores:
                    print(
                        f"Collected core dumps for '{script.name}' -> {core_dumps_dir} "
                        f"({len(script_cores)} files)"
                    )

                exported = export_case_container_logs(
                    rt,
                    case_dir,
                    container_logs_dir,
                    script_token=script_token,
                    include_docker=False,
                    include_modules=True,
                    skip_empty=True,
                )
                if exported:
                    print(
                        f"Collected per-script module logs for '{script.name}' -> {container_logs_dir} "
                        f"({len(exported)} files)"
                    )
                    asan_reports = [
                        p
                        for p in exported
                        if "/modules/asan/" in p.as_posix()
                        and p.name != ""
                        and p.stat().st_size > 0
                    ]
                    if asan_reports:
                        print(
                            f"Collected per-script ASAN reports for '{script.name}' -> {container_logs_dir} "
                            f"({len(asan_reports)} files)"
                        )
                        rel_reports = [str(p.relative_to(container_logs_dir)) for p in asan_reports]
                        result.stdout += (
                            "\n===== STEP: ASAN report check =====\n"
                            "RuntimeError: ASAN reports detected after check script:\n"
                            + "\n".join(f"  {item}" for item in rel_reports)
                            + "\n"
                        )
                        result.failed_step = result.failed_step or "ASAN report check"
                        result.returncode = 1
                        case_failed = True
                clear_case_container_module_logs(rt)
                module_logs_cleared = True
            except Exception as log_exc:
                print(
                    f"WARNING: failed to export/reset per-script module logs for '{script}': {log_exc}",
                    file=sys.stderr,
                )

    except Exception as exc:
        case_failed = True
        startup_cores = collect_core_dumps(core_dumps_dir / make_case_artifact_token(case_dir) / "startup")
        if startup_cores:
            print(f"Collected startup core dumps for case '{case_dir.name}' -> {core_dumps_dir} ({len(startup_cores)} files)")
        startup_log_note = ""
        if rt is not None:
            try:
                exported = export_case_container_logs(
                    rt,
                    case_dir,
                    container_logs_dir,
                    include_docker=True,
                    include_modules=True,
                )
                if exported:
                    startup_log_note = (
                        "\nCollected startup/runtime container logs:\n"
                        + format_artifact_paths(exported, container_logs_dir)
                    )
                    print(
                        f"Collected startup/runtime container logs for case {case_dir.name!r} -> "
                        f"{container_logs_dir} ({len(exported)} files)"
                    )
            except Exception as log_exc:
                startup_log_note = f"\nWARNING: failed to export startup/runtime container logs: {log_exc}"
                print(startup_log_note.strip(), file=sys.stderr)
        startup_capture_note = ""
        if startup_out_buf is not None:
            captured_out = startup_out_buf.getvalue()
            if captured_out:
                startup_capture_note += "\n===== Runtime startup stdout =====\n" + captured_out
        if startup_err_buf is not None:
            captured_err = startup_err_buf.getvalue()
            if captured_err:
                startup_capture_note += "\n===== Runtime startup stderr =====\n" + captured_err
        err = (
            f"case startup/runtime failed for {case_dir}: {exc}\n"
            f"{traceback.format_exc()}{startup_capture_note}{startup_log_note}\n"
        )
        print(err, file=sys.stderr)
        executed = {r.script for r in results}
        previous_result = results[-1] if results else None
        for script in scripts:
            if script in executed:
                continue
            result = synth_failed_result(case_dir, script, err, previous_result=previous_result)
            results.append(result)
            previous_result = result
    finally:
        if rt is not None:
            try:
                exported = export_case_container_logs(
                    rt,
                    case_dir,
                    container_logs_dir,
                    include_docker=True,
                    include_modules=not module_logs_cleared,
                )
                if exported:
                    print(
                        f"Collected container logs for case '{case_dir.name}' -> {container_logs_dir} "
                        f"({len(exported)} files)"
                    )
            except Exception as log_exc:
                print(f"WARNING: failed to export container logs for case '{case_dir}': {log_exc}", file=sys.stderr)
            rt.close(failed=case_failed)
            teardown_cores = collect_core_dumps(core_dumps_dir / make_case_artifact_token(case_dir) / "teardown")
            if teardown_cores:
                print(
                    f"Collected teardown core dumps for case '{case_dir.name}' -> {core_dumps_dir} "
                    f"({len(teardown_cores)} files)"
                )
        print(f"===== END CASE: {case_dir} =====")

    return results


def write_check_log(log_path: Path, result: CheckResult) -> None:
    lines = [
        f"case: {result.case_dir}",
        f"script: {result.script}",
        f"status: {result.status}",
        f"returncode: {result.returncode}",
        f"previous_script: {result.previous_script or '-'}",
        f"previous_status: {result.previous_status or '-'}",
        f"failed_step: {result.failed_step or '-'}",
        f"started_at_utc: {format_timestamp(result.started_at)}",
        f"ended_at_utc: {format_timestamp(result.ended_at)}",
        f"duration_sec: {result.duration_sec:.3f}",
        f"command: {' '.join(shlex.quote(part) for part in result.command)}",
        "",
        "===== STDOUT =====",
        result.stdout,
        "",
        "===== STDERR =====",
        result.stderr,
        "",
    ]
    log_path.write_text("\n".join(lines), encoding="utf-8")


def truncate_for_html(text: str) -> tuple[str, bool]:
    if len(text) <= MAX_HTML_OUTPUT_CHARS:
        return text, False
    return text[:MAX_HTML_OUTPUT_CHARS], True


def split_output_steps(text: str) -> list[tuple[str, str]]:
    """
    Parse output by lines like:
      ===== STEP: Configure BGP base =====
    """
    if not text:
        return [("Output", "")]

    steps: list[tuple[str, str]] = []
    current_title = "Output"
    current_lines: list[str] = []

    for line in text.splitlines(keepends=True):
        m = STEP_MARKER_RE.match(line.strip())
        if m:
            if current_lines or not steps:
                steps.append((current_title, "".join(current_lines)))
            current_title = m.group(1).strip() or "Step"
            current_lines = []
            continue
        current_lines.append(line)

    steps.append((current_title, "".join(current_lines)))

    # Drop empty leading "Output" section when real STEP blocks exist.
    if len(steps) > 1 and steps[0][0] == "Output" and not steps[0][1].strip():
        steps = steps[1:]
    return steps


def detect_step_status(title: str, content: str, *, failed_step_title: str | None = None) -> str:
    step_text = f"{title}\n{content}"
    if failed_step_title:
        if title == failed_step_title:
            return "fail"
        if any(token in step_text for token in FAIL_STEP_HINTS):
            return "warn"
        if any(token in step_text for token in WARN_STEP_HINTS):
            return "warn"
        return "pass"

    if any(token in step_text for token in FAIL_STEP_HINTS):
        return "fail"
    if any(token in step_text for token in WARN_STEP_HINTS):
        return "warn"
    return "pass"


def classify_step_line(line: str) -> str:
    if LOG_CMD_LINE_RE.match(line):
        return "cmd"
    if LOG_ECHO_LINE_RE.match(line):
        return "echo"
    if LOG_PROMPT_LINE_RE.match(line):
        return "prompt"
    if LOG_STEP_LINE_RE.match(line):
        return "marker"
    if any(token in line for token in FAIL_STEP_HINTS):
        return "fail"
    if any(token in line for token in WARN_STEP_HINTS):
        return "warn"
    return "text"


def render_step_content(content: str) -> str:
    if not content.strip():
        return "<div class='log-empty'>(no output)</div>"
    safe = html.escape(content.rstrip("\n")) or "(no output)"
    return f"<pre class='log-chunk log-text'>{safe}</pre>"


def build_step_views(
    steps: list[tuple[str, str]],
    *,
    failed_step_title: str | None = None,
) -> list[dict[str, object]]:
    views: list[dict[str, object]] = []
    for i, (title, content) in enumerate(steps, start=1):
        status = detect_step_status(title, content, failed_step_title=failed_step_title)
        views.append(
            {
                "index": i,
                "title": title,
                "content": content,
                "status": status,
            }
        )
    return views


def render_step_sidebar(step_views: list[dict[str, object]], *, active_index: int) -> str:
    items: list[str] = []
    for step in step_views:
        idx = int(step["index"])
        status = str(step["status"])
        status_upper = status.upper()
        safe_title = html.escape(str(step["title"]))
        active_cls = " active" if idx == active_index else ""
        items.append(
            "".join(
                [
                    f"<button class='step-nav-item step-nav-{status}{active_cls}' data-step='{idx}' type='button'>",
                    f"<span class='step-nav-num'>#{idx}</span>",
                    f"<span class='step-nav-title'>{safe_title}</span>",
                    f"<span class='step-nav-status'>{status_upper}</span>",
                    "</button>",
                ]
            )
        )
    return "".join(items)


def render_step_panels(step_views: list[dict[str, object]], *, active_index: int) -> str:
    panels: list[str] = []
    for step in step_views:
        idx = int(step["index"])
        status = str(step["status"])
        safe_title = html.escape(str(step["title"]))
        body_html = render_step_content(str(step["content"]))
        panels.append(
            "".join(
                [
                    f"<section class='step-panel step-{status}' id='step-{idx}' data-step-panel='{idx}' data-status='{status}'>",
                    "<header class='step-panel-header'>",
                    f"<h3><span class='step-num'>#{idx}</span><span class='step-title-text'>{safe_title}</span></h3>",
                    f"<span class='step-badge step-badge-{status}'>{status.upper()}</span>",
                    "</header>",
                    f"<div class='step-panel-body'>{body_html}</div>",
                    "</section>",
                ]
            )
        )
    return "".join(panels)


def write_check_html(path: Path, result: CheckResult, *, index: int) -> None:
    badge_cls = "status-badge-pass" if result.returncode == 0 else "status-badge-fail"
    case_name = html.escape(str(result.case_dir))
    script_name = html.escape(str(result.script))
    command = html.escape(" ".join(shlex.quote(part) for part in result.command))
    previous_label = result.previous_script or "<none>"
    previous_status = result.previous_status or "-"
    previous_meta = html.escape(f"{previous_label} [{previous_status}]" if result.previous_script else previous_label)

    combined = result.stdout
    if result.stderr:
        combined = f"{combined}\n\n===== STEP: STDERR =====\n{result.stderr}"
    clipped, truncated = truncate_for_html(combined)
    trunc_note = "<p class='trunc'>Output truncated in HTML. See logs/*.log for full content.</p>" if truncated else ""
    steps = split_output_steps(clipped)
    step_views = build_step_views(steps, failed_step_title=result.failed_step)
    active_index = 1
    if step_views:
        if result.failed_step:
            for step in step_views:
                if str(step["title"]) == result.failed_step:
                    active_index = int(step["index"])
                    break
        elif result.returncode != 0:
            for step in step_views:
                if str(step["status"]) == "fail":
                    active_index = int(step["index"])
                    break

    sidebar_html = render_step_sidebar(step_views, active_index=active_index)
    panel_html = render_step_panels(step_views, active_index=active_index)

    doc = f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
  <title>CI Check Detail #{index}</title>
  <style>
    :root {{
      --bg0: #f3f6fb;
      --panel: #ffffff;
      --panel-soft: #f8fbff;
      --line: #d7e2ea;
      --line-soft: #e7eef6;
      --text: #0f172a;
      --muted: #4b5563;
      --accent: #0f3d91;
      --ok: #12754b;
      --ok-bg: #e9f8f0;
      --ok-line: #a7dec3;
      --warn: #a16207;
      --warn-bg: #fff8e6;
      --warn-line: #f7d8a6;
      --bad: #b42318;
      --bad-bg: #fdeceb;
      --bad-line: #f2b3af;
      --ink: #0b1220;
      --shadow: 0 4px 14px rgba(15, 23, 42, 0.06);
      --header-h: 124px;
    }}
    * {{ box-sizing: border-box; }}
    html, body {{ height: 100%; }}
    body {{
      margin: 0;
      font-family: 'Segoe UI', 'Helvetica Neue', Arial, sans-serif;
      color: var(--text);
      background: var(--bg0);
    }}
    a {{ color: var(--accent); text-decoration: none; }}
    a:hover {{ text-decoration: underline; }}
    code {{
      background: #eef3fb;
      border: 1px solid #d6e1f0;
      padding: 1px 6px;
      border-radius: 6px;
      font-size: 12px;
      word-break: break-all;
    }}

    /* ========== Sticky compact header ========== */
    .topbar {{
      position: sticky;
      top: 0;
      z-index: 20;
      background: var(--panel);
      border-bottom: 1px solid var(--line);
      box-shadow: var(--shadow);
    }}
    .topbar-inner {{
      padding: 10px clamp(16px, 2vw, 28px);
      display: grid;
      grid-template-columns: minmax(0, 1fr) auto;
      align-items: center;
      gap: 14px;
    }}
    .top-link {{ font-size: 13px; }}
    .title-row {{
      display: flex;
      align-items: center;
      gap: 10px;
      min-width: 0;
      margin-top: 4px;
    }}
    h1 {{
      margin: 0;
      font-size: 17px;
      font-weight: 700;
      letter-spacing: 0.01em;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }}
    .idx-tag {{
      display: inline-flex;
      align-items: center;
      padding: 2px 8px;
      border-radius: 6px;
      background: var(--panel-soft);
      border: 1px solid var(--line);
      color: var(--muted);
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      font-size: 12px;
      font-weight: 700;
      flex: 0 0 auto;
    }}
    .status-badge-pass, .status-badge-fail {{
      padding: 3px 10px;
      border-radius: 999px;
      font-size: 11px;
      font-weight: 700;
      letter-spacing: 0.04em;
      flex: 0 0 auto;
    }}
    .status-badge-pass {{ color: var(--ok); background: var(--ok-bg); border: 1px solid var(--ok-line); }}
    .status-badge-fail {{ color: var(--bad); background: var(--bad-bg); border: 1px solid var(--bad-line); }}
    .meta-strip {{
      padding: 0 clamp(16px, 2vw, 28px) 10px;
      display: flex;
      flex-wrap: wrap;
      gap: 8px 18px;
      font-size: 12px;
      color: var(--muted);
    }}
    .meta-strip b {{ color: var(--text); font-weight: 600; }}
    .toolbar {{
      padding: 8px clamp(16px, 2vw, 28px) 10px;
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      align-items: center;
      border-top: 1px solid var(--line-soft);
      background: var(--panel-soft);
    }}
    .toolbar .steps-title {{ font-weight: 600; font-size: 13px; color: var(--muted); margin-right: auto; }}
    .toolbar .steps-title b {{ color: var(--text); }}
    .tool-btn {{
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--accent);
      border-radius: 8px;
      font-size: 13px;
      font-weight: 600;
      padding: 6px 12px;
      cursor: pointer;
    }}
    .tool-btn:hover {{ background: #f0f6ff; }}

    .trunc {{
      margin: 12px clamp(16px, 2vw, 28px) 0;
      color: #8a4b00;
      background: #fff3df;
      border: 1px solid #f7d8a6;
      padding: 10px 12px;
      border-radius: 10px;
      font-weight: 600;
    }}

    /* ========== Two-pane body ========== */
    .layout {{
      display: grid;
      grid-template-columns: minmax(260px, 320px) minmax(0, 1fr);
      gap: 0;
      align-items: start;
    }}
    .steps-sidebar {{
      position: sticky;
      top: var(--header-h);
      max-height: calc(100vh - var(--header-h));
      overflow-y: auto;
      padding: 14px clamp(8px, 1vw, 14px);
      border-right: 1px solid var(--line-soft);
      background: var(--panel-soft);
    }}
    .steps-sidebar h2 {{
      margin: 0 0 8px 0;
      font-size: 11px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: var(--muted);
    }}
    .steps-nav {{
      display: flex;
      flex-direction: column;
      gap: 5px;
    }}
    .step-nav-item {{
      width: 100%;
      display: grid;
      grid-template-columns: auto 1fr auto;
      gap: 8px;
      align-items: center;
      text-align: left;
      border: 1px solid transparent;
      background: transparent;
      border-radius: 8px;
      padding: 7px 10px;
      cursor: pointer;
      color: #1a2b45;
    }}
    .step-nav-item:hover {{ background: #eaf1fb; }}
    .step-nav-item.active {{
      background: #fff;
      border-color: #2d5fb8;
      box-shadow: 0 0 0 2px rgba(45, 95, 184, 0.12);
    }}
    .step-nav-num {{
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      font-size: 12px;
      color: var(--muted);
      font-weight: 700;
      font-variant-numeric: tabular-nums;
    }}
    .step-nav-title {{
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      font-size: 13px;
      font-weight: 500;
    }}
    .step-nav-status {{
      font-size: 10px;
      font-weight: 700;
      border-radius: 999px;
      padding: 2px 7px;
      border: 1px solid transparent;
      letter-spacing: 0.04em;
    }}
    .step-nav-pass .step-nav-status {{ color: var(--ok); background: var(--ok-bg); border-color: var(--ok-line); }}
    .step-nav-warn .step-nav-status {{ color: var(--warn); background: var(--warn-bg); border-color: var(--warn-line); }}
    .step-nav-fail .step-nav-status {{ color: var(--bad); background: var(--bad-bg); border-color: var(--bad-line); }}

    .main {{
      padding: 16px clamp(16px, 2vw, 28px) 32px;
      min-width: 0;
    }}
    .steps-content {{ min-width: 0; display: flex; flex-direction: column; gap: 12px; }}
    .step-panel {{
      border: 1px solid var(--line);
      border-radius: 12px;
      background: var(--panel);
      overflow: hidden;
      box-shadow: var(--shadow);
      scroll-margin-top: calc(var(--header-h) + 12px);
    }}
    .step-panel-header {{
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      padding: 10px 14px;
      border-bottom: 1px solid var(--line-soft);
      background: #f2f8ff;
      cursor: pointer;
      user-select: none;
    }}
    .step-panel-header h3 {{
      margin: 0;
      font-size: 14px;
      line-height: 1.3;
      color: #1a2b45;
      min-width: 0;
      flex: 1;
      display: flex;
      align-items: center;
      gap: 8px;
    }}
    .step-panel-header h3::before {{
      content: '\\25BE';
      color: #94a3b8;
      font-size: 10px;
      transition: transform 0.15s ease;
      flex: 0 0 auto;
    }}
    .step-panel.collapsed .step-panel-header h3::before {{ transform: rotate(-90deg); }}
    .step-panel-header .step-num {{
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      font-size: 12px;
      color: var(--muted);
      font-weight: 700;
      font-variant-numeric: tabular-nums;
    }}
    .step-badge {{
      font-size: 10px;
      font-weight: 700;
      border-radius: 999px;
      padding: 2px 8px;
      border: 1px solid transparent;
      letter-spacing: 0.04em;
      flex: 0 0 auto;
    }}
    .step-badge-pass {{ color: var(--ok); background: var(--ok-bg); border-color: var(--ok-line); }}
    .step-badge-warn {{ color: var(--warn); background: var(--warn-bg); border-color: var(--warn-line); }}
    .step-badge-fail {{ color: var(--bad); background: var(--bad-bg); border-color: var(--bad-line); }}
    .step-panel-body {{ padding: 12px; background: var(--panel); }}
    .step-panel.collapsed .step-panel-body {{ display: none; }}
    .log-empty {{ color: var(--muted); font-size: 13px; padding: 6px 2px; }}
    .log-chunk {{
      margin: 0;
      margin-bottom: 8px;
      padding: 10px 12px;
      overflow: auto;
      border-radius: 8px;
      border: 1px solid #2a3e5b;
      background: var(--ink);
      color: #dbe7ff;
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      font-size: 12px;
      line-height: 1.45;
      white-space: pre;
    }}
    .log-chunk:last-child {{ margin-bottom: 0; }}
    .log-cmd, .log-echo, .log-prompt, .log-marker, .log-fail, .log-warn, .log-text {{
      border-color: #2a3e5b;
      background: var(--ink);
      color: #dbe7ff;
    }}

    /* Responsive */
    @media (max-width: 1100px) {{
      :root {{ --header-h: 156px; }}
      .layout {{ grid-template-columns: 1fr; }}
      .steps-sidebar {{
        position: static;
        max-height: none;
        border-right: 0;
        border-bottom: 1px solid var(--line-soft);
        padding: 10px clamp(12px, 2vw, 20px);
      }}
      .steps-nav {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 4px; }}
    }}
    @media (max-width: 720px) {{
      .topbar-inner {{ grid-template-columns: 1fr; }}
      h1 {{ font-size: 15px; white-space: normal; }}
      .meta-strip {{ font-size: 11px; }}
      .step-nav-item {{ grid-template-columns: auto 1fr; }}
      .step-nav-status {{ grid-column: 1 / span 2; justify-self: start; }}
    }}
  </style>
</head>
<body>
  <header class=\"topbar\">
    <div class=\"topbar-inner\">
      <div>
        <p class=\"top-link\"><a href=\"../report.html\">← 返回 Summary</a></p>
        <div class=\"title-row\">
          <span class=\"idx-tag\">#{index}</span>
          <h1 title=\"{script_name}\">{script_name}</h1>
          <span class=\"{badge_cls}\">{result.status}</span>
        </div>
      </div>
    </div>
    <div class=\"meta-strip\">
      <span><b>Case</b> <code>{case_name}</code></span>
      <span><b>Prev</b> <code>{previous_meta}</code></span>
      <span><b>Cmd</b> <code>{command}</code></span>
      <span><b>Start</b> {html.escape(format_timestamp(result.started_at))}</span>
      <span><b>End</b> {html.escape(format_timestamp(result.ended_at))}</span>
      <span><b>Duration</b> {result.duration_sec:.2f}s</span>
    </div>
    <div class=\"toolbar\">
      <div class=\"steps-title\">执行输出 共 <b>{len(step_views)}</b> 个步骤</div>
      <button id=\"expand-all-btn\" class=\"tool-btn\" type=\"button\">全部展开</button>
      <button id=\"collapse-all-btn\" class=\"tool-btn\" type=\"button\">全部折叠</button>
      <button id=\"jump-fail-btn\" class=\"tool-btn\" type=\"button\">跳到首个失败</button>
    </div>
  </header>
  {trunc_note}
  <div class=\"layout\">
    <aside class=\"steps-sidebar\" aria-label=\"Step navigation\">
      <h2>步骤目录</h2>
      <div class=\"steps-nav\">
        {sidebar_html}
      </div>
    </aside>
    <main class=\"main\">
      <div class=\"steps-content\">
        {panel_html}
      </div>
    </main>
  </div>
  <script>
    (function () {{
      var navItems = Array.prototype.slice.call(document.querySelectorAll('.step-nav-item'));
      var panels = Array.prototype.slice.call(document.querySelectorAll('.step-panel'));

      function setActive(stepId) {{
        navItems.forEach(function (btn) {{
          btn.classList.toggle('active', btn.getAttribute('data-step') === stepId);
        }});
      }}

      function scrollToStep(stepId) {{
        var panel = document.getElementById('step-' + stepId);
        if (!panel) return;
        panel.classList.remove('collapsed');
        panel.scrollIntoView({{ behavior: 'smooth', block: 'start' }});
        setActive(stepId);
      }}

      navItems.forEach(function (btn) {{
        btn.addEventListener('click', function () {{
          scrollToStep(btn.getAttribute('data-step'));
        }});
      }});

      // Click panel header toggles collapse
      panels.forEach(function (panel) {{
        var hdr = panel.querySelector('.step-panel-header');
        if (hdr) {{
          hdr.addEventListener('click', function () {{
            panel.classList.toggle('collapsed');
          }});
        }}
      }});

      var expandBtn = document.getElementById('expand-all-btn');
      if (expandBtn) {{
        expandBtn.addEventListener('click', function () {{
          panels.forEach(function (p) {{ p.classList.remove('collapsed'); }});
        }});
      }}
      var collapseBtn = document.getElementById('collapse-all-btn');
      if (collapseBtn) {{
        collapseBtn.addEventListener('click', function () {{
          panels.forEach(function (p) {{ p.classList.add('collapsed'); }});
        }});
      }}
      var jumpFailBtn = document.getElementById('jump-fail-btn');
      if (jumpFailBtn) {{
        jumpFailBtn.addEventListener('click', function () {{
          var firstFail = document.querySelector(".step-panel[data-status='fail']");
          if (firstFail) scrollToStep(firstFail.getAttribute('data-step-panel'));
        }});
      }}

      // Highlight active step on scroll
      var io = ('IntersectionObserver' in window) ? new IntersectionObserver(function (entries) {{
        var visible = entries.filter(function (e) {{ return e.isIntersecting; }})
                             .sort(function (a, b) {{ return a.boundingClientRect.top - b.boundingClientRect.top; }});
        if (visible.length > 0) setActive(visible[0].target.getAttribute('data-step-panel'));
      }}, {{ rootMargin: '-20% 0px -60% 0px', threshold: 0 }}) : null;
      if (io) panels.forEach(function (p) {{ io.observe(p); }});

      // Initial: scroll to active step (failure or step 1)
      setActive('{active_index}');
      var initial = document.getElementById('step-{active_index}');
      if (initial && initial.getAttribute('data-status') === 'fail') {{
        // Defer to allow layout to settle
        setTimeout(function () {{ initial.scrollIntoView({{ behavior: 'auto', block: 'start' }}); }}, 0);
      }}
    }})();
  </script>
</body>
</html>
"""
    path.write_text(doc, encoding="utf-8")


def write_summary_json(path: Path, results: list[CheckResult], started_at: float, ended_at: float) -> None:
    payload = {
        "started_at_utc": format_timestamp(started_at),
        "ended_at_utc": format_timestamp(ended_at),
        "duration_sec": round(ended_at - started_at, 3),
        "total": len(results),
        "passed": sum(1 for r in results if r.returncode == 0),
        "failed": sum(1 for r in results if r.returncode != 0),
        "results": [
            {
                "case": str(r.case_dir),
                "script": str(r.script),
                "status": r.status,
                "returncode": r.returncode,
                "previous_script": r.previous_script,
                "previous_status": r.previous_status,
                "failed_step": r.failed_step,
                "duration_sec": round(r.duration_sec, 3),
                "started_at_utc": format_timestamp(r.started_at),
                "ended_at_utc": format_timestamp(r.ended_at),
                "command": r.command,
            }
            for r in results
        ],
    }
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=True), encoding="utf-8")


def split_case_for_report(case_dir: Path, modules_dir: Path | None) -> tuple[str, str]:
    case_abs = case_dir.resolve()
    candidate_roots: list[Path] = []

    default_modules_dir = (CI_DIR / "modules").resolve()
    candidate_roots.append(default_modules_dir)
    if modules_dir is not None:
        mod_abs = modules_dir.resolve()
        if mod_abs not in candidate_roots:
            candidate_roots.append(mod_abs)

    for root in candidate_roots:
        try:
            rel_case = case_abs.relative_to(root)
        except ValueError:
            continue
        parts = rel_case.parts
        if not parts:
            return "(root)", "(default)"
        if len(parts) == 1:
            return parts[0], "(default)"
        return parts[0], "/".join(parts[1:])

    parent_name = case_abs.parent.name if case_abs.parent.name else "(external)"
    return parent_name, case_abs.name


def write_html_report(
    path: Path,
    results: list[CheckResult],
    started_at: float,
    ended_at: float,
    check_html_relpaths: list[str],
    modules_dir: Path | None = None,
) -> None:
    passed = sum(1 for r in results if r.returncode == 0)
    failed = sum(1 for r in results if r.returncode != 0)
    total = len(results)
    duration_total = ended_at - started_at
    pass_rate = (passed / total * 100.0) if total > 0 else 0.0

    grouped: dict[str, dict[str, list[tuple[int, CheckResult, str]]]] = {}
    for idx, result in enumerate(results, start=1):
        check_link = check_html_relpaths[idx - 1] if idx - 1 < len(check_html_relpaths) else ""
        module_name, testbed_name = split_case_for_report(result.case_dir, modules_dir)
        grouped.setdefault(module_name, {}).setdefault(testbed_name, []).append((idx, result, check_link))

    def _slug(text: str) -> str:
        return re.sub(r"[^a-zA-Z0-9_-]+", "-", text).strip("-").lower() or "x"

    module_blocks: list[str] = []
    nav_items: list[str] = []
    for module_index, (module_name, testbeds) in enumerate(grouped.items()):
        module_rows = [row for testbed_rows in testbeds.values() for row in testbed_rows]
        module_passed = sum(1 for _, item, _ in module_rows if item.returncode == 0)
        module_failed = len(module_rows) - module_passed
        module_slug = f"mod-{module_index}-{_slug(module_name)}"
        module_state = "fail" if module_failed > 0 else "pass"

        testbed_blocks: list[str] = []
        nav_testbeds: list[str] = []
        for testbed_index, (testbed_name, rows) in enumerate(testbeds.items()):
            tb_passed = sum(1 for _, item, _ in rows if item.returncode == 0)
            tb_failed = len(rows) - tb_passed
            tb_slug = f"{module_slug}-tb-{testbed_index}-{_slug(testbed_name)}"
            tb_state = "fail" if tb_failed > 0 else "pass"

            table_rows: list[str] = []
            for row_index, result, check_link in rows:
                cls = "pass" if result.returncode == 0 else "fail"
                status_badge = f"<span class='status-badge status-{cls}'>{result.status}</span>"

                try:
                    rel_script = result.script.resolve().relative_to(result.case_dir.resolve())
                    script_label = str(rel_script)
                except ValueError:
                    script_label = result.script.name

                script_name = html.escape(script_label)
                if check_link:
                    link = html.escape(check_link)
                    script_view = f"<a href=\"{link}\">{script_name}</a>"
                else:
                    script_view = script_name
                previous_view = html.escape(
                    f"{result.previous_script} [{result.previous_status or '-'}]"
                    if result.previous_script
                    else "-"
                )

                table_rows.append(
                    "".join(
                        [
                            f"<tr data-status='{cls}' data-script='{html.escape(script_label.lower())}'>",
                            f"<td class='mono col-idx'>{row_index}</td>",
                            f"<td class='script'>{script_view}</td>",
                            f"<td class='mono col-prev'>{previous_view}</td>",
                            f"<td class='col-status'>{status_badge}</td>",
                            f"<td class='mono col-rc'>{result.returncode}</td>",
                            f"<td class='mono col-dur'>{result.duration_sec:.2f}</td>",
                            "</tr>",
                        ]
                    )
                )

            testbed_open = " open" if tb_failed > 0 else ""
            testbed_blocks.append(
                f"""
          <details class=\"group-node testbed-group\" id=\"{tb_slug}\" data-state=\"{tb_state}\" data-passed=\"{tb_passed}\" data-failed=\"{tb_failed}\"{testbed_open}>
            <summary>
              <div class=\"summary-title\">
                <span class=\"sum-dot dot-{tb_state}\"></span>
                <span class=\"sum-kind\">测试床</span>
                <span class=\"sum-name\">{html.escape(testbed_name)}</span>
              </div>
              <div class=\"summary-meta\">
                <span class=\"chip\">脚本 {len(rows)}</span>
                <span class=\"chip pass-chip\">通过 {tb_passed}</span>
                <span class=\"chip fail-chip\">失败 {tb_failed}</span>
              </div>
            </summary>
            <div class=\"table-scroll\">
              <table>
                <thead>
                  <tr><th class='col-idx'>#</th><th>Script</th><th class='col-prev'>Previous</th><th class='col-status'>Status</th><th class='col-rc'>RC</th><th class='col-dur'>Duration(s)</th></tr>
                </thead>
                <tbody>
                  {''.join(table_rows)}
                </tbody>
              </table>
            </div>
          </details>
                """.rstrip()
            )

            nav_testbeds.append(
                f"<li><a href=\"#{tb_slug}\" data-state=\"{tb_state}\">"
                f"<span class=\"nav-dot dot-{tb_state}\"></span>"
                f"<span class=\"nav-label\">{html.escape(testbed_name)}</span>"
                f"<span class=\"nav-counts\">{tb_passed}/{len(rows)}</span></a></li>"
            )

        module_open = " open" if (module_index == 0 or module_failed > 0) else ""
        module_blocks.append(
            f"""
      <details class=\"group-node module-group\" id=\"{module_slug}\" data-state=\"{module_state}\" data-passed=\"{module_passed}\" data-failed=\"{module_failed}\"{module_open}>
        <summary>
          <div class=\"summary-title\">
            <span class=\"sum-dot dot-{module_state}\"></span>
            <span class=\"sum-kind\">模块</span>
            <span class=\"sum-name\">{html.escape(module_name)}</span>
          </div>
          <div class=\"summary-meta\">
            <span class=\"chip\">测试床 {len(testbeds)}</span>
            <span class=\"chip\">脚本 {len(module_rows)}</span>
            <span class=\"chip pass-chip\">通过 {module_passed}</span>
            <span class=\"chip fail-chip\">失败 {module_failed}</span>
          </div>
        </summary>
        <div class=\"module-body\">
          {''.join(testbed_blocks)}
        </div>
      </details>
            """.rstrip()
        )

        nav_items.append(
            f"<li class=\"nav-module\" data-state=\"{module_state}\">"
            f"<a class=\"nav-mod-link\" href=\"#{module_slug}\" data-state=\"{module_state}\">"
            f"<span class=\"nav-dot dot-{module_state}\"></span>"
            f"<span class=\"nav-label\">{html.escape(module_name)}</span>"
            f"<span class=\"nav-counts\">{module_passed}/{len(module_rows)}</span></a>"
            f"<ul class=\"nav-tb-list\">{''.join(nav_testbeds)}</ul></li>"
        )

    doc = f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
  <title>CI Module Report</title>
  <style>
    :root {{
      --bg0: #f3f6fb;
      --bg1: #eef3f9;
      --panel: #ffffff;
      --panel-soft: #f8fbff;
      --line: #d8e1ea;
      --line-soft: #e7eef6;
      --text: #0f172a;
      --muted: #4b5563;
      --accent: #0f3d91;
      --ok: #12754b;
      --ok-bg: #e9f8f0;
      --ok-line: #a7dec3;
      --bad: #b42318;
      --bad-bg: #fdeceb;
      --bad-line: #f2b3af;
      --shadow: 0 4px 14px rgba(15, 23, 42, 0.06);
      --shadow-strong: 0 8px 22px rgba(15, 23, 42, 0.08);
      --radius: 12px;
      --header-h: 116px;
    }}
    * {{ box-sizing: border-box; }}
    html, body {{ height: 100%; }}
    body {{
      margin: 0;
      font-family: 'Segoe UI', 'Helvetica Neue', Arial, sans-serif;
      color: var(--text);
      background: var(--bg0);
    }}
    a {{ color: var(--accent); }}
    code {{
      background: #eef3fb;
      border: 1px solid #d6e1f0;
      padding: 1px 6px;
      border-radius: 6px;
      font-size: 12px;
    }}

    /* ========== Sticky header ========== */
    .topbar {{
      position: sticky;
      top: 0;
      z-index: 20;
      background: var(--panel);
      border-bottom: 1px solid var(--line);
      box-shadow: var(--shadow);
    }}
    .topbar-inner {{
      padding: 12px clamp(16px, 2vw, 28px);
      display: grid;
      grid-template-columns: minmax(280px, 1fr) auto;
      align-items: center;
      gap: 14px;
    }}
    .topbar h1 {{
      margin: 0;
      font-size: 18px;
      letter-spacing: 0.01em;
      display: flex;
      align-items: center;
      gap: 10px;
    }}
    .topbar h1 .ts {{
      color: var(--muted);
      font-weight: 500;
      font-size: 12px;
    }}
    .stats {{
      display: flex;
      gap: 6px;
      flex-wrap: wrap;
      justify-content: flex-end;
    }}
    .stat-pill {{
      display: inline-flex;
      align-items: baseline;
      gap: 6px;
      padding: 5px 11px;
      border-radius: 999px;
      background: var(--panel-soft);
      border: 1px solid var(--line);
      font-size: 13px;
      color: var(--muted);
    }}
    .stat-pill .v {{ font-weight: 700; color: var(--text); font-variant-numeric: tabular-nums; }}
    .stat-pill.pass {{ background: var(--ok-bg); border-color: var(--ok-line); color: var(--ok); }}
    .stat-pill.pass .v {{ color: var(--ok); }}
    .stat-pill.fail {{ background: var(--bad-bg); border-color: var(--bad-line); color: var(--bad); }}
    .stat-pill.fail .v {{ color: var(--bad); }}

    .toolbar {{
      padding: 10px clamp(16px, 2vw, 28px) 12px;
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      align-items: center;
      border-top: 1px solid var(--line-soft);
      background: var(--panel-soft);
    }}
    .filter-group {{ display: inline-flex; gap: 0; border: 1px solid var(--line); background: var(--panel); border-radius: 999px; overflow: hidden; }}
    .filter-btn {{
      border: 0;
      background: transparent;
      padding: 6px 14px;
      font-size: 13px;
      cursor: pointer;
      color: var(--muted);
      font-weight: 600;
    }}
    .filter-btn + .filter-btn {{ border-left: 1px solid var(--line); }}
    .filter-btn.active {{ background: var(--accent); color: #fff; }}
    .filter-btn.fail.active {{ background: var(--bad); }}
    .filter-btn.pass.active {{ background: var(--ok); }}
    .tool-btn {{
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--accent);
      font-weight: 600;
      border-radius: 8px;
      padding: 6px 12px;
      font-size: 13px;
      cursor: pointer;
    }}
    .tool-btn:hover {{ background: #f0f6ff; }}
    .search-box {{
      flex: 1 1 220px;
      min-width: 180px;
      max-width: 360px;
    }}
    .search-box input {{
      width: 100%;
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 8px;
      padding: 6px 10px;
      font-size: 13px;
      color: var(--text);
    }}
    .search-box input:focus {{ outline: 2px solid #c4d7f7; outline-offset: 0; }}

    /* ========== Two-pane body ========== */
    .layout {{
      display: grid;
      grid-template-columns: minmax(220px, 280px) minmax(0, 1fr);
      gap: 0;
      align-items: start;
    }}
    .sidenav {{
      position: sticky;
      top: var(--header-h);
      max-height: calc(100vh - var(--header-h));
      overflow-y: auto;
      padding: 14px clamp(8px, 1vw, 16px);
      border-right: 1px solid var(--line-soft);
    }}
    .sidenav h2 {{
      margin: 0 0 8px 0;
      font-size: 11px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: var(--muted);
    }}
    .nav-list, .nav-tb-list {{ list-style: none; margin: 0; padding: 0; }}
    .nav-list > li {{ margin-bottom: 4px; }}
    .nav-list a {{
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 6px 8px;
      border-radius: 6px;
      text-decoration: none;
      color: var(--text);
      font-size: 13px;
    }}
    .nav-list a:hover {{ background: #eef3fb; }}
    .nav-mod-link {{ font-weight: 600; }}
    .nav-tb-list {{ margin: 2px 0 6px 14px; padding-left: 8px; border-left: 1px dashed var(--line); }}
    .nav-tb-list a {{ font-size: 12px; color: var(--muted); }}
    .nav-label {{ flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }}
    .nav-counts {{ font-variant-numeric: tabular-nums; font-size: 11px; color: var(--muted); }}
    .nav-dot, .sum-dot {{
      width: 8px;
      height: 8px;
      border-radius: 50%;
      flex: 0 0 auto;
    }}
    .dot-pass {{ background: #2bb673; box-shadow: 0 0 0 3px rgba(43, 182, 115, 0.15); }}
    .dot-fail {{ background: #e0463a; box-shadow: 0 0 0 3px rgba(224, 70, 58, 0.16); }}

    .main {{
      padding: 16px clamp(16px, 2vw, 28px) 32px;
      min-width: 0;
    }}

    /* ========== Module / testbed cards ========== */
    .groups {{ display: flex; flex-direction: column; gap: 14px; }}
    .module-group {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: var(--radius);
      box-shadow: var(--shadow);
      overflow: hidden;
      scroll-margin-top: calc(var(--header-h) + 12px);
    }}
    .module-body {{
      padding: 14px;
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(min(560px, 100%), 1fr));
      gap: 14px;
      background: linear-gradient(180deg, #fbfdff 0%, #ffffff 60%);
    }}
    .testbed-group {{
      background: var(--panel);
      border: 1px solid var(--line-soft);
      border-radius: 10px;
      overflow: hidden;
      scroll-margin-top: calc(var(--header-h) + 12px);
    }}
    details > summary {{
      list-style: none;
      cursor: pointer;
      padding: 12px 14px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 12px;
      background: #f6faff;
      border-bottom: 1px solid var(--line-soft);
      user-select: none;
    }}
    details > summary::-webkit-details-marker {{ display: none; }}
    details > summary::before {{
      content: '\\25B8';
      color: #94a3b8;
      font-size: 11px;
      transition: transform 0.15s ease;
      flex: 0 0 auto;
    }}
    details[open] > summary::before {{ transform: rotate(90deg); }}
    details[open].module-group > summary {{ background: #eef5ff; }}
    .module-group > summary {{ background: #eef5ff; }}
    .module-group > summary .summary-title {{ font-size: 15px; }}
    .summary-title {{ display: flex; align-items: center; gap: 8px; font-weight: 600; color: #1f3b63; flex: 1; min-width: 0; }}
    .summary-title .sum-kind {{ color: var(--muted); font-weight: 500; font-size: 12px; }}
    .summary-title .sum-name {{ overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }}
    .summary-meta {{ display: flex; gap: 6px; flex-wrap: wrap; flex: 0 0 auto; }}
    .chip {{
      border: 1px solid #dbe5f2;
      background: var(--panel);
      color: #334155;
      border-radius: 999px;
      font-size: 12px;
      padding: 2px 9px;
      font-variant-numeric: tabular-nums;
    }}
    .pass-chip {{ color: var(--ok); border-color: var(--ok-line); background: var(--ok-bg); }}
    .fail-chip {{ color: var(--bad); border-color: var(--bad-line); background: var(--bad-bg); }}

    /* ========== Result table ========== */
    .table-scroll {{ overflow-x: auto; }}
    table {{ border-collapse: collapse; width: 100%; }}
    thead th {{
      text-align: left;
      padding: 9px 12px;
      background: #f6faff;
      border-bottom: 1px solid var(--line);
      font-size: 12px;
      color: #244268;
      font-weight: 600;
      letter-spacing: 0.02em;
    }}
    tbody td {{
      padding: 9px 12px;
      border-bottom: 1px solid #edf2f7;
      vertical-align: middle;
      font-size: 13px;
    }}
    tbody tr:last-child td {{ border-bottom: 0; }}
    tbody tr:hover {{ background: #f4f9ff; }}
    .col-idx {{ width: 44px; color: var(--muted); }}
    .col-prev {{ max-width: 260px; color: var(--muted); overflow-wrap: anywhere; }}
    .col-status {{ width: 80px; }}
    .col-rc {{ width: 56px; text-align: right; }}
    .col-dur {{ width: 92px; text-align: right; }}
    .mono {{ font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; font-variant-numeric: tabular-nums; }}
    .script a {{
      color: var(--accent);
      text-decoration: none;
      font-weight: 600;
    }}
    .script a:hover {{ text-decoration: underline; }}
    .status-badge {{
      display: inline-block;
      padding: 3px 10px;
      border-radius: 999px;
      font-size: 11px;
      font-weight: 700;
      letter-spacing: 0.04em;
    }}
    .status-pass {{ color: var(--ok); background: var(--ok-bg); border: 1px solid var(--ok-line); }}
    .status-fail {{ color: var(--bad); background: var(--bad-bg); border: 1px solid var(--bad-line); }}

    .empty {{
      background: var(--panel);
      border: 1px dashed var(--line);
      border-radius: 12px;
      padding: 24px;
      color: var(--muted);
      text-align: center;
    }}

    /* Filter states */
    body.filter-pass tr[data-status='fail'] {{ display: none; }}
    body.filter-fail tr[data-status='pass'] {{ display: none; }}
    body.filter-pass .testbed-group[data-failed]:not([data-passed='0']) {{ }}
    body.filter-pass .testbed-group[data-passed='0'] {{ display: none; }}
    body.filter-fail .testbed-group[data-failed='0'] {{ display: none; }}
    body.filter-pass .module-group[data-passed='0'] {{ display: none; }}
    body.filter-fail .module-group[data-failed='0'] {{ display: none; }}
    body.filter-pass .nav-module[data-state='fail'] > .nav-mod-link[data-state='fail'] {{ opacity: 0.45; }}
    body.filter-fail .nav-module[data-state='pass'] {{ display: none; }}
    body.filter-pass .nav-tb-list a[data-state='fail'] {{ display: none; }}
    body.filter-fail .nav-tb-list a[data-state='pass'] {{ display: none; }}
    tr.search-hidden {{ display: none; }}

    /* Responsive */
    @media (max-width: 1100px) {{
      .layout {{ grid-template-columns: 1fr; }}
      .sidenav {{
        position: static;
        max-height: none;
        border-right: 0;
        border-bottom: 1px solid var(--line-soft);
        padding: 10px clamp(12px, 2vw, 20px);
      }}
      .nav-list {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 4px; }}
      .nav-tb-list {{ display: none; }}
    }}
    @media (max-width: 720px) {{
      .topbar-inner {{ grid-template-columns: 1fr; }}
      .stats {{ justify-content: flex-start; }}
      details > summary {{ flex-direction: column; align-items: flex-start; }}
    }}
  </style>
</head>
<body>
  <header class=\"topbar\">
    <div class=\"topbar-inner\">
      <h1>
        CI Module Execution Report
        <span class=\"ts\">{html.escape(format_timestamp(started_at))} → {html.escape(format_timestamp(ended_at))} UTC · {duration_total:.1f}s</span>
      </h1>
      <div class=\"stats\">
        <span class=\"stat-pill\">Total <span class=\"v\">{total}</span></span>
        <span class=\"stat-pill pass\">Passed <span class=\"v\">{passed}</span></span>
        <span class=\"stat-pill fail\">Failed <span class=\"v\">{failed}</span></span>
        <span class=\"stat-pill\">Pass Rate <span class=\"v\">{pass_rate:.1f}%</span></span>
      </div>
    </div>
    <div class=\"toolbar\">
      <div class=\"filter-group\" role=\"group\" aria-label=\"Status filter\">
        <button class=\"filter-btn active\" data-filter=\"all\" type=\"button\">全部</button>
        <button class=\"filter-btn pass\" data-filter=\"pass\" type=\"button\">仅通过</button>
        <button class=\"filter-btn fail\" data-filter=\"fail\" type=\"button\">仅失败</button>
      </div>
      <button id=\"expand-all-btn\" class=\"tool-btn\" type=\"button\">全部展开</button>
      <button id=\"collapse-all-btn\" class=\"tool-btn\" type=\"button\">全部折叠</button>
      <div class=\"search-box\">
        <input id=\"search-input\" type=\"search\" placeholder=\"搜索脚本…\" autocomplete=\"off\">
      </div>
    </div>
  </header>

  <div class=\"layout\">
    <aside class=\"sidenav\" aria-label=\"Module navigation\">
      <h2>模块导航</h2>
      <ul class=\"nav-list\">
        {''.join(nav_items) if nav_items else '<li class="empty">No modules</li>'}
      </ul>
    </aside>
    <main class=\"main\">
      <section class=\"groups\">
        {''.join(module_blocks) if module_blocks else '<div class="empty">No checks executed.</div>'}
      </section>
    </main>
  </div>
  <script>
    (function () {{
      var nodes = Array.prototype.slice.call(document.querySelectorAll('details.group-node'));
      function setOpen(openState) {{ nodes.forEach(function (n) {{ n.open = openState; }}); }}

      var expandBtn = document.getElementById('expand-all-btn');
      if (expandBtn) expandBtn.addEventListener('click', function () {{ setOpen(true); }});
      var collapseBtn = document.getElementById('collapse-all-btn');
      if (collapseBtn) collapseBtn.addEventListener('click', function () {{ setOpen(false); }});

      var filterBtns = document.querySelectorAll('.filter-btn');
      filterBtns.forEach(function (btn) {{
        btn.addEventListener('click', function () {{
          var f = btn.getAttribute('data-filter');
          filterBtns.forEach(function (b) {{ b.classList.remove('active'); }});
          btn.classList.add('active');
          document.body.classList.remove('filter-pass', 'filter-fail');
          if (f === 'pass') document.body.classList.add('filter-pass');
          else if (f === 'fail') document.body.classList.add('filter-fail');
        }});
      }});

      var searchInput = document.getElementById('search-input');
      if (searchInput) {{
        searchInput.addEventListener('input', function () {{
          var q = searchInput.value.trim().toLowerCase();
          var rows = document.querySelectorAll('tbody tr[data-script]');
          rows.forEach(function (r) {{
            var name = r.getAttribute('data-script') || '';
            if (!q || name.indexOf(q) >= 0) r.classList.remove('search-hidden');
            else r.classList.add('search-hidden');
          }});
          // Auto-open groups that have hits when searching
          if (q) {{
            document.querySelectorAll('details.group-node').forEach(function (d) {{
              var hasHit = d.querySelector('tbody tr[data-script]:not(.search-hidden)');
              if (hasHit) d.open = true;
            }});
          }}
        }});
      }}

      // Highlight current nav target on click
      document.querySelectorAll('.nav-list a').forEach(function (a) {{
        a.addEventListener('click', function () {{
          var href = a.getAttribute('href') || '';
          if (href.charAt(0) === '#') {{
            var target = document.querySelector(href);
            if (target && target.tagName === 'DETAILS') target.open = true;
          }}
        }});
      }});
    }})();
  </script>
</body>
</html>
"""
    path.write_text(doc, encoding="utf-8")


def ensure_report_dir(report_dir: Path) -> tuple[Path, Path, Path, Path]:
    logs_dir = report_dir / "logs"
    checks_dir = report_dir / "checks"
    logs_dir.mkdir(parents=True, exist_ok=True)
    checks_dir.mkdir(parents=True, exist_ok=True)
    return report_dir / "report.html", report_dir / "summary.json", logs_dir, checks_dir


def make_artifact_base_name(index: int, result: CheckResult, modules_dir: Path) -> str:
    try:
        rel_case = result.case_dir.resolve().relative_to(modules_dir.resolve())
        case_name = str(rel_case)
    except ValueError:
        case_name = result.case_dir.name
    case_token = sanitize_name(case_name.replace(os.sep, "-"))
    script_token = sanitize_name(result.script.stem)
    return f"{index:02d}-{case_token}-{script_token}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run CI module checks and generate HTML report")
    parser.add_argument("--modules-dir", default="scripts/ci/modules", help="module case root directory")
    parser.add_argument("--image", required=False, help="docker image tag (fallback to top.image when omitted)")
    parser.add_argument("--report-dir", default="scripts/ci/reports", help="output directory for reports")
    parser.add_argument("--keep", action="store_true", help="keep case containers/networks for debugging")
    parser.add_argument(
        "--pause-on-fail",
        action="store_true",
        help="on check failure, skip the script's own cleanup and (with --keep) preserve runtime state so you can docker exec into the container to debug",
    )
    parser.add_argument("--cmd-timeout", type=int, default=30, help="CLI command timeout seconds")
    parser.add_argument("--connect-timeout", type=int, default=90, help="CLI initial connect timeout seconds")
    parser.add_argument("--verbose-modules", action="store_true", help="enable verbose runtime CLI logs")
    parser.add_argument(
        "--script",
        action="append",
        default=[],
        help=(
            "run only selected check script (can repeat); supports absolute path, "
            "path relative to --modules-dir, or unique basename"
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.pause_on_fail:
        os.environ["NN_PAUSE_ON_FAIL"] = "1"
        # 自动启用 --keep，否则容器会被 runner 清掉
        args.keep = True
    modules_dir = Path(args.modules_dir).resolve()
    report_dir = Path(args.report_dir)
    report_html, summary_json, logs_dir, checks_dir = ensure_report_dir(report_dir)

    run_started = time.time()
    selected_by_case: dict[Path, list[Path]] = {}
    if args.script:
        for raw_selector in args.script:
            try:
                script = resolve_script_selector(raw_selector, modules_dir)
            except Exception as exc:
                print(f"invalid --script '{raw_selector}': {exc}", file=sys.stderr)
                return 1
            case = script.parent.resolve()
            selected_by_case.setdefault(case, []).append(script)

        for case, scripts in selected_by_case.items():
            selected_by_case[case] = sorted(set(scripts))
        case_dirs = sorted(selected_by_case.keys())
    else:
        case_dirs = discover_case_dirs(modules_dir)

    results: list[CheckResult] = []

    if not case_dirs:
        run_ended = time.time()
        write_summary_json(summary_json, results, run_started, run_ended)
        write_html_report(report_html, results, run_started, run_ended, check_html_relpaths=[], modules_dir=modules_dir)
        print(f"No CI cases found under {modules_dir}")
        print(f"Report: {report_html}")
        return 1

    for case_dir in case_dirs:
        case_results = run_case(
            case_dir=case_dir,
            image_arg=args.image,
            cmd_timeout=args.cmd_timeout,
            connect_timeout=args.connect_timeout,
            verbose=args.verbose_modules,
            keep=args.keep,
            container_logs_dir=report_dir / "containers",
            core_dumps_dir=report_dir / "core-dumps",
            scripts_override=selected_by_case.get(case_dir),
        )
        results.extend(case_results)

    check_html_relpaths: list[str] = []
    for idx, result in enumerate(results, start=1):
        base = make_artifact_base_name(idx, result, modules_dir)
        log_name = f"{base}.log"
        html_name = f"{base}.html"
        write_check_log(logs_dir / log_name, result)
        write_check_html(checks_dir / html_name, result, index=idx)
        check_html_relpaths.append(f"checks/{html_name}")

    run_ended = time.time()
    write_summary_json(summary_json, results, run_started, run_ended)
    write_html_report(
        report_html,
        results,
        run_started,
        run_ended,
        check_html_relpaths=check_html_relpaths,
        modules_dir=modules_dir,
    )

    failed = sum(1 for r in results if r.returncode != 0)
    print(f"Report written: {report_html}")
    print(f"Summary written: {summary_json}")
    print(f"Logs directory: {logs_dir}")
    print(f"Check HTML directory: {checks_dir}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
