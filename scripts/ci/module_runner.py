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
import shlex
import sys
import time
import traceback
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


CI_DIR = Path(__file__).resolve().parent
if str(CI_DIR) not in sys.path:
    sys.path.insert(0, str(CI_DIR))

from module_api import load_global_top  # noqa: E402
from top_runner import PAGER_DISABLE_CMD, TopologyRuntime, load_topology, sanitize_name  # noqa: E402


MAX_HTML_OUTPUT_CHARS = 200000
TOP_CANDIDATES = ("top.yaml", "top.yml", "top.json")
SHOW_CURRENT_CONFIG_CMD = "show current-configuration"
PROMPT_LINE_RE = re.compile(r"^\s*<NetNexus[^>]*>.*$")
MAX_CONFIG_DIFF_LINES = 300
STEP_MARKER_RE = re.compile(r"^(?:\[[^\]]+\]\s*)?\s*=+\s*STEP:\s*(.*?)\s*=+\s*$")
FAIL_STEP_HINTS = (
    "===== CHECK FAIL:",
    "Traceback (most recent call last):",
    "RuntimeError:",
    "ERROR:",
    "missing:",
)
WARN_STEP_HINTS = (
    "WARNING:",
    "疑似未清理本次脚本配置",
    "config drift",
)
TIMESTAMP_FMT = "%Y-%m-%dT%H:%M:%S.%fZ"
MODULE_ROW_RE = re.compile(
    r"^\s*(?P<id>\d+)\s+(?P<name>[A-Za-z0-9_-]+)\s+(?P<phase>[A-Za-z0-9_-]+)\s+(?P<port>\d+)\s+(?P<ipc>[A-Za-z0-9_-]+)\s*$"
)


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


def ensure_device_modules_ready(rt: TopologyRuntime, top: dict[str, Any]) -> None:
    devices = top.get("devices", {})
    if not isinstance(devices, dict) or not devices:
        raise RuntimeError("top.devices must be a non-empty mapping for module precheck")

    print("===== STEP: Precheck device modules =====")
    for dev in sorted(devices.keys()):
        out = rt.exec_cmd(dev, "show dev modules", strict=False)
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
            raise RuntimeError(f"{dev}: failed to parse module table from 'show dev modules'\n{out}")

        bad = [
            f"{r['name']}(phase={r['phase']},ipc={r['ipc']})"
            for r in rows
            if r["phase"].upper() != "READY" or r["ipc"].lower() != "up"
        ]
        if bad:
            raise RuntimeError(
                f"{dev}: modules not healthy (require Phase=READY and IPC=up): {', '.join(bad)}\n{out}"
            )

        print(f"[{dev}] modules READY/up OK ({len(rows)} modules)")


def ensure_cli_pager_disabled(rt: TopologyRuntime, top: dict[str, Any]) -> None:
    devices = top.get("devices", {})
    if not isinstance(devices, dict) or not devices:
        raise RuntimeError("top.devices must be a non-empty mapping for pager precheck")

    print("===== STEP: Disable CLI pager =====")
    rt.disable_pager_for_all_sessions()
    for dev in sorted(devices.keys()):
        print(f"[{dev}] pager disabled via '{PAGER_DISABLE_CMD}'")


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
        out = rt.exec_cmd(dev, SHOW_CURRENT_CONFIG_CMD, strict=False, timeout=timeout)
        normalized = normalize_show_current_config(out)
        snapshots[dev] = normalized
        print(f"[{dev}] collected '{SHOW_CURRENT_CONFIG_CMD}' ({len(normalized.splitlines())} lines)")
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


def run_check(script: Path, rt: TopologyRuntime, top: dict[str, Any]) -> CheckResult:
    command = ["run(rt, top)"]
    started = time.time()

    out_buf = TimestampedBuffer()
    err_buf = TimestampedBuffer()
    tee_out = Tee(sys.stdout, out_buf)
    tee_err = Tee(sys.stderr, err_buf)

    rc = 0
    try:
        load_global_top(top)
        run_fn = load_run_callable(script)
        with contextlib.redirect_stdout(tee_out), contextlib.redirect_stderr(tee_err):
            print(f"===== RUN CHECK: {script} =====")
            load_global_top(top)
            ensure_cli_pager_disabled(rt, top)
            ensure_device_modules_ready(rt, top)

            before_cfg = collect_show_current_config(rt, top, stage="before")
            run_failed = False
            try:
                run_fn(rt, top)
            except Exception:
                run_failed = True
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
        with contextlib.redirect_stderr(tee_err):
            print(f"===== CHECK FAIL: {script} =====", file=sys.stderr)
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
    )


def synth_failed_result(case_dir: Path, script: Path, err: str) -> CheckResult:
    now = time.time()
    return CheckResult(
        case_dir=case_dir,
        script=script,
        command=["run(rt, top)"],
        started_at=now,
        ended_at=now,
        returncode=1,
        stdout="",
        stderr=err,
    )


def run_case(
    case_dir: Path,
    image_arg: str | None,
    cmd_timeout: int,
    connect_timeout: int,
    verbose: bool,
    keep: bool,
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

    base_modules = (CI_DIR / "modules").resolve()
    case_abs = case_dir.resolve()
    try:
        rel = case_abs.relative_to(base_modules)
    except ValueError:
        rel = Path(sanitize_name(str(case_abs)))
    prefix = sanitize_name(f"nn-case-{rel}-{os.getpid()}")

    results: list[CheckResult] = []
    case_failed = False
    rt: TopologyRuntime | None = None
    startup_stdout = ""
    startup_stderr = ""

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

        for idx, script in enumerate(scripts):
            result = run_check(script, rt, top)
            if idx == 0:
                prefix_parts: list[str] = []
                if startup_stdout:
                    prefix_parts.append(startup_stdout)
                if startup_stderr:
                    prefix_parts.append(f"===== STDERR =====\n{startup_stderr}")
                if prefix_parts:
                    result.stdout = "\n\n".join(prefix_parts) + ("\n" if result.stdout else "") + result.stdout
            results.append(result)
            if result.returncode != 0:
                case_failed = True

    except Exception as exc:
        case_failed = True
        err = f"case startup/runtime failed for {case_dir}: {exc}\n{traceback.format_exc()}"
        print(err, file=sys.stderr)
        executed = {r.script for r in results}
        results.extend(synth_failed_result(case_dir, script, err) for script in scripts if script not in executed)
    finally:
        if rt is not None:
            rt.close(failed=case_failed)
        print(f"===== END CASE: {case_dir} =====")

    return results


def write_check_log(log_path: Path, result: CheckResult) -> None:
    lines = [
        f"case: {result.case_dir}",
        f"script: {result.script}",
        f"status: {result.status}",
        f"returncode: {result.returncode}",
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


def render_step_blocks(steps: list[tuple[str, str]], open_all: bool) -> str:
    blocks: list[str] = []
    for i, (title, content) in enumerate(steps, start=1):
        open_attr = " open" if open_all or i == 1 else ""
        step_text = f"{title}\n{content}"
        if any(token in step_text for token in FAIL_STEP_HINTS):
            status = "fail"
            summary_cls = "step-summary-fail"
        elif any(token in step_text for token in WARN_STEP_HINTS):
            status = "warn"
            summary_cls = "step-summary-warn"
        else:
            status = "pass"
            summary_cls = "step-summary-pass"
        safe_title = html.escape(title)
        safe_content = html.escape(content.rstrip("\n")) or "(no output)"
        blocks.append(
            "".join(
                [
                    f"<details class='step step-{status}'{open_attr}>",
                    f"<summary class='{summary_cls}'>Step {i}: {safe_title}</summary>",
                    f"<pre>{safe_content}</pre>",
                    "</details>",
                ]
            )
        )
    return "".join(blocks)


def write_check_html(path: Path, result: CheckResult, *, index: int) -> None:
    badge_cls = "status-badge-pass" if result.returncode == 0 else "status-badge-fail"
    case_name = html.escape(str(result.case_dir))
    script_name = html.escape(str(result.script))
    command = html.escape(" ".join(shlex.quote(part) for part in result.command))

    combined = result.stdout
    if result.stderr:
        combined = f"{combined}\n\n===== STDERR =====\n{result.stderr}"
    clipped, truncated = truncate_for_html(combined)
    trunc_note = "<p class='trunc'>Output truncated in HTML. See logs/*.log for full content.</p>" if truncated else ""
    steps = split_output_steps(clipped)
    step_blocks = render_step_blocks(steps, open_all=(result.returncode != 0))

    doc = f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
  <title>CI Check Detail #{index}</title>
  <style>
    :root {{
      --bg0: #f2f6ff;
      --bg1: #eaf5f2;
      --panel: #ffffff;
      --line: #d7e2ea;
      --text: #0f172a;
      --muted: #4b5563;
      --ok: #12754b;
      --ok-bg: #e9f8f0;
      --warn: #a16207;
      --warn-bg: #fff8e6;
      --bad: #b42318;
      --bad-bg: #fdeceb;
      --mono-bg: #0f172a;
      --mono-fg: #e2e8f0;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      font-family: 'Segoe UI', 'Helvetica Neue', Arial, sans-serif;
      color: var(--text);
      background: radial-gradient(1400px 700px at 0% 0%, var(--bg0), var(--bg1));
      padding: 18px;
    }}
    .wrap {{ max-width: 1100px; margin: 0 auto; }}
    .top-link {{ margin-bottom: 10px; }}
    .top-link a {{
      color: #0f3d91;
      text-decoration: none;
      font-weight: 600;
      border-bottom: 1px dashed #7ea5e0;
    }}
    .card {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 14px;
      box-shadow: 0 8px 28px rgba(15, 23, 42, 0.08);
      padding: 16px 18px;
      margin-bottom: 14px;
    }}
    .title-row {{
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      flex-wrap: wrap;
    }}
    h1 {{ margin: 0; font-size: 24px; line-height: 1.3; }}
    .meta-grid {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 8px 14px;
      margin-top: 12px;
      font-size: 14px;
    }}
    .meta-item {{ color: var(--muted); }}
    .meta-item b {{ color: var(--text); }}
    .trunc {{
      color: #8a4b00;
      background: #fff3df;
      border: 1px solid #f7d8a6;
      padding: 10px 12px;
      border-radius: 10px;
      font-weight: 600;
    }}
    .status-badge-pass, .status-badge-fail {{
      padding: 4px 10px;
      border-radius: 999px;
      font-size: 12px;
      font-weight: 700;
      letter-spacing: 0.03em;
    }}
    .status-badge-pass {{ color: var(--ok); background: var(--ok-bg); border: 1px solid #a7dec3; }}
    .status-badge-fail {{ color: var(--bad); background: var(--bad-bg); border: 1px solid #f2b3af; }}
    code {{
      background: #eef3fb;
      border: 1px solid #d6e1f0;
      padding: 2px 6px;
      border-radius: 6px;
      font-size: 12px;
      word-break: break-all;
    }}
    details {{
      margin: 10px 0;
      border: 1px solid var(--line);
      border-radius: 10px;
      background: #fff;
      overflow: hidden;
    }}
    details > summary {{
      cursor: pointer;
      font-weight: 700;
      padding: 10px 12px;
      background: #f7fbff;
    }}
    .script-output > summary {{ font-size: 15px; }}
    .step {{
      margin: 10px;
      border-left: 4px solid #cdd9e6;
      border-radius: 8px;
    }}
    .step-pass {{ border-left-color: var(--ok); }}
    .step-warn {{ border-left-color: var(--warn); }}
    .step-fail {{ border-left-color: var(--bad); }}
    .step-summary-pass {{ color: var(--ok); }}
    .step-summary-warn {{ color: var(--warn); }}
    .step-summary-fail {{ color: var(--bad); }}
    pre {{
      margin: 0;
      padding: 12px;
      background: var(--mono-bg);
      color: var(--mono-fg);
      overflow: auto;
      border-top: 1px solid rgba(255, 255, 255, 0.08);
      font-size: 12px;
      line-height: 1.5;
    }}
    @media (max-width: 680px) {{
      body {{ padding: 10px; }}
      .card {{ padding: 12px; border-radius: 12px; }}
      h1 {{ font-size: 19px; }}
    }}
  </style>
</head>
<body>
  <div class=\"wrap\">
    <p class=\"top-link\"><a href=\"../report.html\">Back to Summary</a></p>
    <section class=\"card\">
      <div class=\"title-row\">
        <h1>#{index} {script_name}</h1>
        <span class=\"{badge_cls}\">{result.status}</span>
      </div>
      <div class=\"meta-grid\">
        <div class=\"meta-item\"><b>Case:</b> <code>{case_name}</code></div>
        <div class=\"meta-item\"><b>Command:</b> <code>{command}</code></div>
        <div class=\"meta-item\"><b>Start (UTC):</b> {html.escape(format_timestamp(result.started_at))}</div>
        <div class=\"meta-item\"><b>End (UTC):</b> {html.escape(format_timestamp(result.ended_at))}</div>
        <div class=\"meta-item\"><b>Duration:</b> {result.duration_sec:.2f}s</div>
      </div>
    </section>
    {trunc_note}
    <section class=\"card\">
      <details class='script-output' open>
        <summary>Execution Output ({len(steps)} steps)</summary>
        {step_blocks}
      </details>
    </section>
  </div>
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
                "duration_sec": round(r.duration_sec, 3),
                "started_at_utc": format_timestamp(r.started_at),
                "ended_at_utc": format_timestamp(r.ended_at),
                "command": r.command,
            }
            for r in results
        ],
    }
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=True), encoding="utf-8")


def write_html_report(
    path: Path,
    results: list[CheckResult],
    started_at: float,
    ended_at: float,
    check_html_relpaths: list[str],
) -> None:
    passed = sum(1 for r in results if r.returncode == 0)
    failed = sum(1 for r in results if r.returncode != 0)
    total = len(results)
    duration_total = ended_at - started_at
    pass_rate = (passed / total * 100.0) if total > 0 else 0.0

    rows: list[str] = []
    for idx, (result, check_html_relpath) in enumerate(zip(results, check_html_relpaths), start=1):
        cls = "pass" if result.returncode == 0 else "fail"
        case_name = html.escape(str(result.case_dir))
        script_name = html.escape(str(result.script))
        link = html.escape(check_html_relpath)
        script_link = f"<a href=\"{link}\">{script_name}</a>"
        status_badge = f"<span class='status-badge status-{cls}'>{result.status}</span>"

        rows.append(
            "".join(
                [
                    "<tr>",
                    f"<td class='mono'>{idx}</td>",
                    f"<td>{case_name}</td>",
                    f"<td class='script'>{script_link}</td>",
                    f"<td>{status_badge}</td>",
                    f"<td class='mono'>{result.returncode}</td>",
                    f"<td class='mono'>{result.duration_sec:.2f}</td>",
                    "</tr>",
                ]
            )
        )

    doc = f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
  <title>CI Module Report</title>
  <style>
    :root {{
      --bg0: #f1f5ff;
      --bg1: #ecf7f2;
      --panel: #ffffff;
      --line: #d8e1ea;
      --text: #0f172a;
      --muted: #4b5563;
      --ok: #12754b;
      --ok-bg: #e9f8f0;
      --bad: #b42318;
      --bad-bg: #fdeceb;
      --shadow: 0 8px 26px rgba(15, 23, 42, 0.08);
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      font-family: 'Segoe UI', 'Helvetica Neue', Arial, sans-serif;
      color: var(--text);
      background: radial-gradient(1400px 700px at 0% 0%, var(--bg0), var(--bg1));
      padding: 18px;
    }}
    .wrap {{ max-width: 1200px; margin: 0 auto; }}
    .hero {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 14px;
      box-shadow: var(--shadow);
      padding: 18px;
      margin-bottom: 14px;
    }}
    h1 {{ margin: 0 0 8px 0; font-size: 26px; }}
    .sub {{ color: var(--muted); font-size: 14px; line-height: 1.6; }}
    .stats {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
      gap: 10px;
      margin-top: 12px;
    }}
    .stat {{
      border: 1px solid var(--line);
      border-radius: 10px;
      background: #fbfdff;
      padding: 10px 12px;
    }}
    .stat .label {{ color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0.05em; }}
    .stat .value {{ font-size: 22px; font-weight: 700; margin-top: 2px; }}
    .pass-val {{ color: var(--ok); }}
    .fail-val {{ color: var(--bad); }}
    .table-card {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 14px;
      box-shadow: var(--shadow);
      overflow: hidden;
    }}
    .table-scroll {{ overflow-x: auto; }}
    table {{ border-collapse: collapse; width: 100%; min-width: 720px; }}
    thead th {{
      text-align: left;
      padding: 12px;
      background: #f6faff;
      border-bottom: 1px solid var(--line);
      font-size: 13px;
      color: #244268;
    }}
    tbody td {{
      padding: 11px 12px;
      border-bottom: 1px solid #edf2f7;
      vertical-align: top;
      font-size: 14px;
    }}
    tbody tr:nth-child(even) {{ background: #fbfdff; }}
    tbody tr:hover {{ background: #f4f9ff; }}
    .mono {{ font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; }}
    .script a {{
      color: #0f3d91;
      text-decoration: none;
      border-bottom: 1px dashed #9ab7e8;
      font-weight: 600;
    }}
    .script a:hover {{ color: #0b2e6e; }}
    .status-badge {{
      display: inline-block;
      padding: 4px 10px;
      border-radius: 999px;
      font-size: 12px;
      font-weight: 700;
      letter-spacing: 0.03em;
    }}
    .status-pass {{ color: var(--ok); background: var(--ok-bg); border: 1px solid #a7dec3; }}
    .status-fail {{ color: var(--bad); background: var(--bad-bg); border: 1px solid #f2b3af; }}
    code {{
      background: #eef3fb;
      border: 1px solid #d6e1f0;
      padding: 1px 6px;
      border-radius: 6px;
      font-size: 12px;
    }}
    @media (max-width: 760px) {{
      body {{ padding: 10px; }}
      .hero, .table-card {{ border-radius: 12px; }}
      h1 {{ font-size: 22px; }}
    }}
  </style>
</head>
<body>
  <div class=\"wrap\">
    <section class=\"hero\">
      <h1>CI Module Execution Report</h1>
      <p class=\"sub\">
        Started: {html.escape(format_timestamp(started_at))} UTC<br>
        Ended: {html.escape(format_timestamp(ended_at))} UTC<br>
        Artifacts: <code>checks/</code> detail pages + <code>logs/</code> raw logs
      </p>
      <div class=\"stats\">
        <div class=\"stat\"><div class=\"label\">Total</div><div class=\"value\">{total}</div></div>
        <div class=\"stat\"><div class=\"label\">Passed</div><div class=\"value pass-val\">{passed}</div></div>
        <div class=\"stat\"><div class=\"label\">Failed</div><div class=\"value fail-val\">{failed}</div></div>
        <div class=\"stat\"><div class=\"label\">Pass Rate</div><div class=\"value\">{pass_rate:.1f}%</div></div>
        <div class=\"stat\"><div class=\"label\">Duration</div><div class=\"value\">{duration_total:.2f}s</div></div>
      </div>
    </section>

    <section class=\"table-card\">
      <div class=\"table-scroll\">
        <table>
          <thead>
            <tr><th>#</th><th>Case</th><th>Script Detail</th><th>Status</th><th>RC</th><th>Duration(s)</th></tr>
          </thead>
          <tbody>
            {''.join(rows)}
          </tbody>
        </table>
      </div>
    </section>
  </div>
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
    parser.add_argument("--cmd-timeout", type=int, default=30, help="CLI command timeout seconds")
    parser.add_argument("--connect-timeout", type=int, default=60, help="CLI initial connect timeout seconds")
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
        write_html_report(report_html, results, run_started, run_ended, check_html_relpaths=[])
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
    )

    failed = sum(1 for r in results if r.returncode != 0)
    print(f"Report written: {report_html}")
    print(f"Summary written: {summary_json}")
    print(f"Logs directory: {logs_dir}")
    print(f"Check HTML directory: {checks_dir}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
