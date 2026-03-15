#!/usr/bin/env python3
"""
Discover case directories under scripts/ci/modules, start topology runtime once per
case directory, run all check scripts in that case, then cleanup.
"""

from __future__ import annotations

import argparse
import contextlib
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

from top_runner import TopologyRuntime, load_topology, sanitize_name  # noqa: E402


MAX_HTML_OUTPUT_CHARS = 200000
TOP_CANDIDATES = ("top.yaml", "top.yml", "top.json")
STEP_MARKER_RE = re.compile(r"^\s*=+\s*STEP:\s*(.*?)\s*=+\s*$")


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


def run_check(script: Path, rt: TopologyRuntime, top: dict[str, Any]) -> CheckResult:
    command = ["run(rt, top)"]
    started = time.time()

    out_buf = io.StringIO()
    err_buf = io.StringIO()
    tee_out = Tee(sys.stdout, out_buf)
    tee_err = Tee(sys.stderr, err_buf)

    rc = 0
    try:
        run_fn = load_run_callable(script)
        with contextlib.redirect_stdout(tee_out), contextlib.redirect_stderr(tee_err):
            print(f"===== RUN CHECK: {script} =====")
            run_fn(rt, top)
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
) -> list[CheckResult]:
    top_file = find_top_file(case_dir)
    if top_file is None:
        return [
            synth_failed_result(
                case_dir,
                case_dir / "<case>",
                f"case has no top file ({', '.join(TOP_CANDIDATES)}): {case_dir}",
            )
        ]

    scripts = discover_case_scripts(case_dir)
    if not scripts:
        return [
            synth_failed_result(
                case_dir,
                case_dir / "<no-check-script>",
                f"no check scripts found in case directory: {case_dir}",
            )
        ]

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
        startup_out_buf = io.StringIO()
        startup_err_buf = io.StringIO()
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
        safe_title = html.escape(title)
        safe_content = html.escape(content.rstrip("\n")) or "(no output)"
        blocks.append(
            "".join(
                [
                    f"<details class='step'{open_attr}>",
                    f"<summary>Step {i}: {safe_title}</summary>",
                    f"<pre>{safe_content}</pre>",
                    "</details>",
                ]
            )
        )
    return "".join(blocks)


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


def write_html_report(path: Path, results: list[CheckResult], started_at: float, ended_at: float) -> None:
    passed = sum(1 for r in results if r.returncode == 0)
    failed = sum(1 for r in results if r.returncode != 0)
    total = len(results)

    rows: list[str] = []
    details: list[str] = []

    for idx, result in enumerate(results, start=1):
        cls = "pass" if result.returncode == 0 else "fail"
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

        rows.append(
            "".join(
                [
                    "<tr>",
                    f"<td>{idx}</td>",
                    f"<td>{case_name}</td>",
                    f"<td>{script_name}</td>",
                    f"<td class='{cls}'>{result.status}</td>",
                    f"<td>{result.returncode}</td>",
                    f"<td>{result.duration_sec:.2f}</td>",
                    "</tr>",
                ]
            )
        )

        details.append(
            "".join(
                [
                    "<section class='module'>",
                    f"<h2>#{idx} {script_name} - <span class='{cls}'>{result.status}</span></h2>",
                    f"<p><b>Case:</b> <code>{case_name}</code></p>",
                    f"<p><b>Command:</b> <code>{command}</code></p>",
                    f"<p><b>Start (UTC):</b> {html.escape(format_timestamp(result.started_at))}<br>",
                    f"<b>End (UTC):</b> {html.escape(format_timestamp(result.ended_at))}<br>",
                    f"<b>Duration:</b> {result.duration_sec:.2f}s</p>",
                    trunc_note,
                    f"<details class='script-output'><summary>Execution Output ({len(steps)} steps)</summary>",
                    step_blocks,
                    "</details>",
                    "</section>",
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
    body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; margin: 20px; color: #111; }}
    h1, h2 {{ margin: 0.4em 0; }}
    table {{ border-collapse: collapse; width: 100%; margin: 16px 0; }}
    th, td {{ border: 1px solid #ddd; padding: 8px; text-align: left; }}
    th {{ background: #f4f4f4; }}
    .pass {{ color: #087443; font-weight: 700; }}
    .fail {{ color: #b42318; font-weight: 700; }}
    .meta {{ color: #444; }}
    pre {{ background: #0b1020; color: #f4f7ff; padding: 12px; overflow: auto; border-radius: 6px; }}
    .module {{ border-top: 2px solid #eceff4; padding-top: 12px; margin-top: 18px; }}
    .trunc {{ color: #7a3e00; font-weight: 600; }}
    code {{ background: #f0f2f5; padding: 1px 4px; border-radius: 4px; }}
    details {{ margin: 8px 0; }}
    details > summary {{ cursor: pointer; font-weight: 600; }}
    .script-output > summary {{ font-size: 15px; }}
    .step {{ margin-left: 8px; }}
  </style>
</head>
<body>
  <h1>CI Module Execution Report</h1>
  <p class=\"meta\">
    Started: {html.escape(format_timestamp(started_at))} UTC<br>
    Ended: {html.escape(format_timestamp(ended_at))} UTC<br>
    Duration: {ended_at - started_at:.2f}s
  </p>
  <p>
    Total: <b>{total}</b> |
    Passed: <b class=\"pass\">{passed}</b> |
    Failed: <b class=\"fail\">{failed}</b>
  </p>

  <table>
    <thead>
      <tr><th>#</th><th>Case</th><th>Script</th><th>Status</th><th>RC</th><th>Duration(s)</th></tr>
    </thead>
    <tbody>
      {''.join(rows)}
    </tbody>
  </table>

  {''.join(details)}
</body>
</html>
"""
    path.write_text(doc, encoding="utf-8")


def ensure_report_dir(report_dir: Path) -> tuple[Path, Path, Path]:
    logs_dir = report_dir / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    return report_dir / "report.html", report_dir / "summary.json", logs_dir


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run all CI cases and generate HTML report")
    parser.add_argument("--modules-dir", default="scripts/ci/modules", help="module case root directory")
    parser.add_argument("--image", required=False, help="docker image tag (fallback to top.image when omitted)")
    parser.add_argument("--report-dir", default="scripts/ci/reports", help="output directory for reports")
    parser.add_argument("--keep", action="store_true", help="keep case containers/networks for debugging")
    parser.add_argument("--cmd-timeout", type=int, default=30, help="CLI command timeout seconds")
    parser.add_argument("--connect-timeout", type=int, default=60, help="CLI initial connect timeout seconds")
    parser.add_argument("--verbose-modules", action="store_true", help="enable verbose runtime CLI logs")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    modules_dir = Path(args.modules_dir).resolve()
    report_dir = Path(args.report_dir)
    report_html, summary_json, logs_dir = ensure_report_dir(report_dir)

    run_started = time.time()
    case_dirs = discover_case_dirs(modules_dir)
    results: list[CheckResult] = []

    if not case_dirs:
        run_ended = time.time()
        write_summary_json(summary_json, results, run_started, run_ended)
        write_html_report(report_html, results, run_started, run_ended)
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
        )
        for result in case_results:
            results.append(result)
            log_name = f"{len(results):02d}-{result.script.stem}.log"
            write_check_log(logs_dir / log_name, result)

    run_ended = time.time()
    write_summary_json(summary_json, results, run_started, run_ended)
    write_html_report(report_html, results, run_started, run_ended)

    failed = sum(1 for r in results if r.returncode != 0)
    print(f"Report written: {report_html}")
    print(f"Summary written: {summary_json}")
    print(f"Logs directory: {logs_dir}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
