#!/usr/bin/env python3
"""Merge sharded module_runner reports into one summary report."""

from __future__ import annotations

import argparse
import json
import shutil
from datetime import datetime
from pathlib import Path

from module_runner import CheckResult, make_artifact_base_name, write_html_report, write_summary_json


def _parse_time(value: str) -> float:
    text = value.strip()
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    return datetime.fromisoformat(text).timestamp()


def _natural_key(path: Path) -> list[object]:
    parts: list[object] = []
    buf = ""
    for ch in path.name:
        if ch.isdigit():
            buf += ch
            continue
        if buf:
            parts.append(int(buf))
            buf = ""
        parts.append(ch)
    if buf:
        parts.append(int(buf))
    return parts


def _load_shard_summary(summary_path: Path) -> dict[str, object]:
    return json.loads(summary_path.read_text(encoding="utf-8"))


def _copy_shard_report(src_dir: Path, dst_root: Path) -> Path:
    shard_dir = dst_root / src_dir.name
    if shard_dir.exists():
        shutil.rmtree(shard_dir)
    ignore = shutil.ignore_patterns("*.tar", "*.tar.gz", "debug-build")
    shutil.copytree(src_dir, shard_dir, ignore=ignore)
    return shard_dir


def _find_check_html(shard_dir: Path, idx: int, result: CheckResult, modules_dir: Path) -> Path | None:
    html_name = f"{make_artifact_base_name(idx, result, modules_dir)}.html"
    html_path = shard_dir / "checks" / html_name
    if html_path.is_file():
        return html_path

    pattern = f"{idx:02d}-*-{result.script.stem}.html"
    matches = sorted((shard_dir / "checks").glob(pattern))
    if matches:
        return matches[0]
    return None


def merge_reports(input_root: Path, output_dir: Path, modules_dir: Path) -> int:
    summary_paths = sorted(input_root.rglob("summary.json"), key=_natural_key)
    if not summary_paths:
        raise RuntimeError(f"no summary.json files found under {input_root}")

    output_dir.mkdir(parents=True, exist_ok=True)
    shards_root = output_dir / "shards"
    shards_root.mkdir(parents=True, exist_ok=True)

    results: list[CheckResult] = []
    check_links: list[str] = []
    run_started: float | None = None
    run_ended: float | None = None

    for summary_path in summary_paths:
        src_dir = summary_path.parent
        shard_dir = _copy_shard_report(src_dir, shards_root)
        payload = _load_shard_summary(summary_path)

        shard_started = _parse_time(str(payload.get("started_at_utc", "")))
        shard_ended = _parse_time(str(payload.get("ended_at_utc", "")))
        run_started = shard_started if run_started is None else min(run_started, shard_started)
        run_ended = shard_ended if run_ended is None else max(run_ended, shard_ended)

        shard_results = list(payload.get("results", []))
        for idx, item_obj in enumerate(shard_results, start=1):
            item = dict(item_obj)
            started_at = _parse_time(str(item["started_at_utc"]))
            ended_at = _parse_time(str(item["ended_at_utc"]))
            result = CheckResult(
                case_dir=Path(str(item["case"])),
                script=Path(str(item["script"])),
                command=list(item.get("command", [])),
                started_at=started_at,
                ended_at=ended_at,
                returncode=int(item.get("returncode", 1)),
                stdout="",
                stderr="",
                previous_script=item.get("previous_script"),
                previous_status=item.get("previous_status"),
                failed_step=item.get("failed_step"),
            )
            results.append(result)

            html_path = _find_check_html(shard_dir, idx, result, modules_dir)
            if html_path is not None:
                check_links.append(html_path.relative_to(output_dir).as_posix())
            else:
                check_links.append("")

    if run_started is None or run_ended is None:
        raise RuntimeError("no check results found in shard summaries")

    write_summary_json(output_dir / "summary.json", results, run_started, run_ended)
    write_html_report(
        output_dir / "report.html",
        results,
        run_started,
        run_ended,
        check_html_relpaths=check_links,
        modules_dir=modules_dir,
        include_runtime_links=False,
    )
    return sum(1 for result in results if result.returncode != 0)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Merge sharded module_runner reports")
    parser.add_argument("--input-root", required=True, help="directory containing downloaded shard report artifacts")
    parser.add_argument("--output-dir", required=True, help="merged report output directory")
    parser.add_argument("--modules-dir", default="scripts/ci/modules", help="module case root directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    failed = merge_reports(
        input_root=Path(args.input_root).resolve(),
        output_dir=Path(args.output_dir),
        modules_dir=Path(args.modules_dir).resolve(),
    )
    print(f"Merged report written: {Path(args.output_dir) / 'report.html'}")
    print(f"Merged summary written: {Path(args.output_dir) / 'summary.json'}")
    print(f"Merged failed checks: {failed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
