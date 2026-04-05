from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest
from functools import lru_cache
from pathlib import Path


def _resolve_repo_root() -> Path:
    cur = Path(__file__).resolve().parent
    for candidate in [cur, *cur.parents]:
        if (candidate / "scripts" / "ci" / "module_runner.py").is_file():
            return candidate
    raise RuntimeError("failed to locate repository root from scripts/ci/test_ci_modules.py")


REPO_ROOT = _resolve_repo_root()
MODULES_DIR = REPO_ROOT / "scripts" / "ci" / "modules"
MODULE_RUNNER = REPO_ROOT / "scripts" / "ci" / "module_runner.py"
BUILD_IMAGE_SCRIPT = REPO_ROOT / "scripts" / "dev" / "build-docker-image.sh"
REPORT_ROOT_DEFAULT = REPO_ROOT / "scripts" / "ci" / "reports" / "vscode"
TRUTHY = {"1", "true", "yes", "on"}
DEFAULT_LOCAL_CI_IMAGE = "netnexus-ci:localtest"


def _discover_module_scripts() -> list[str]:
    if not MODULES_DIR.is_dir():
        return []

    scripts: list[str] = []
    for path in MODULES_DIR.rglob("*.py"):
        if not path.is_file():
            continue
        if path.name == "__init__.py" or path.name.startswith("_"):
            continue
        scripts.append(path.relative_to(MODULES_DIR).as_posix())
    return sorted(scripts)


def _make_test_name(script_rel: str) -> str:
    base = script_rel.removesuffix(".py")
    token = re.sub(r"[^0-9A-Za-z_]+", "_", base)
    token = re.sub(r"_+", "_", token).strip("_")
    if not token:
        token = "unnamed"
    return f"test_{token}"


def _env_flag(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in TRUTHY


def _tail(text: str, limit: int = 3000) -> str:
    if len(text) <= limit:
        return text
    return text[-limit:]


@lru_cache(maxsize=1)
def _default_local_ci_image() -> str | None:
    image = DEFAULT_LOCAL_CI_IMAGE
    try:
        result = subprocess.run(
            ["docker", "image", "inspect", image],
            cwd=REPO_ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if result.returncode == 0:
        return image
    return None


@lru_cache(maxsize=1)
def _ensure_local_ci_image() -> str | None:
    auto_build = os.environ.get("CNETNEXUS_CI_AUTO_BUILD", "1").strip().lower()
    if auto_build in TRUTHY:
        if not BUILD_IMAGE_SCRIPT.is_file():
            raise RuntimeError(f"build script not found: {BUILD_IMAGE_SCRIPT}")

        build_cmd = [str(BUILD_IMAGE_SCRIPT), "--docker-image", DEFAULT_LOCAL_CI_IMAGE]
        if _env_flag("CNETNEXUS_CI_BUILD_RELEASE"):
            build_cmd.insert(1, "--release")

        print(f"[ci-test] building local image before tests: {DEFAULT_LOCAL_CI_IMAGE}", flush=True)
        print(f"[ci-test] build command: {' '.join(build_cmd)}", flush=True)
        proc = subprocess.Popen(
            build_cmd,
            cwd=REPO_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=1,
        )
        assert proc.stdout is not None
        try:
            for line in proc.stdout:
                print(line, end="", flush=True)
        finally:
            proc.stdout.close()

        if proc.wait() != 0:
            raise RuntimeError("failed to build local CI image")
    else:
        print("[ci-test] auto build disabled, skip image build", flush=True)

    image = _default_local_ci_image()
    if image:
        return image
    raise RuntimeError(f"CI image not found: {DEFAULT_LOCAL_CI_IMAGE}")


class CiModuleScriptsTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls) -> None:
        cls._discovered_scripts = _discover_module_scripts()
        cls._resolved_image = os.environ.get("CNETNEXUS_CI_IMAGE")
        if cls._resolved_image:
            print(f"[ci-test] using explicit image from env: {cls._resolved_image}", flush=True)
            return

        try:
            cls._resolved_image = _ensure_local_ci_image()
        except RuntimeError as exc:
            raise RuntimeError(f"CI image prepare failed: {exc}") from exc

        if cls._resolved_image:
            print(f"[ci-test] using local image: {cls._resolved_image}", flush=True)
        else:
            print("[ci-test] image not set, fallback to top.yaml image", flush=True)

    def test__discovery_finds_ci_scripts(self) -> None:
        self.assertTrue(
            self._discovered_scripts,
            f"no CI module scripts found under {MODULES_DIR}",
        )

    def _run_script(self, script_rel: str) -> None:
        report_root = Path(os.environ.get("CNETNEXUS_CI_REPORT_ROOT", str(REPORT_ROOT_DEFAULT)))
        report_token = script_rel.removesuffix(".py").replace("/", "__")
        report_dir = report_root / report_token
        report_dir.mkdir(parents=True, exist_ok=True)
        report_html = report_dir / "report.html"
        summary_json = report_dir / "summary.json"
        logs_dir = report_dir / "logs"
        checks_dir = report_dir / "checks"

        cmd = [
            sys.executable,
            str(MODULE_RUNNER),
            "--modules-dir",
            str(MODULES_DIR),
            "--script",
            script_rel,
            "--report-dir",
            str(report_dir),
        ]

        image = getattr(self.__class__, "_resolved_image", None)
        if image:
            cmd.extend(["--image", image])

        cmd_timeout = os.environ.get("CNETNEXUS_CI_CMD_TIMEOUT")
        if cmd_timeout:
            cmd.extend(["--cmd-timeout", cmd_timeout])

        connect_timeout = os.environ.get("CNETNEXUS_CI_CONNECT_TIMEOUT")
        if connect_timeout:
            cmd.extend(["--connect-timeout", connect_timeout])

        if _env_flag("CNETNEXUS_CI_KEEP"):
            cmd.append("--keep")

        if _env_flag("CNETNEXUS_CI_VERBOSE"):
            cmd.append("--verbose-modules")

        print(f"[ci-test] script: {script_rel}", flush=True)
        print(f"[ci-test] report dir: {report_dir}", flush=True)
        print(f"[ci-test] report html: {report_html}", flush=True)
        print(f"[ci-test] summary json: {summary_json}", flush=True)
        print(f"[ci-test] logs dir: {logs_dir}", flush=True)
        print(f"[ci-test] checks dir: {checks_dir}", flush=True)
        print(f"[ci-test] image: {image or '<top.yaml default>'}", flush=True)

        try:
            proc = subprocess.Popen(
                cmd,
                cwd=REPO_ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=1,
            )
        except OSError as exc:
            self.fail(f"failed to start module_runner for {script_rel}: {exc}")
            return

        assert proc.stdout is not None
        merged_output: list[str] = []
        try:
            for line in proc.stdout:
                print(line, end="", flush=True)
                merged_output.append(line)
        finally:
            proc.stdout.close()

        returncode = proc.wait()
        if returncode == 0:
            print(f"[ci-test] PASS: {script_rel}", flush=True)
            return

        debug_lines = [
            f"script: {script_rel}",
            f"cwd: {REPO_ROOT}",
            f"return code: {returncode}",
            f"command: {' '.join(cmd)}",
            f"report html: {report_html}",
            f"summary json: {summary_json}",
            f"logs dir: {logs_dir}",
            f"checks dir: {checks_dir}",
            "combined output tail:",
            _tail("".join(merged_output).strip()),
        ]
        self.fail("\n".join(debug_lines))


def _bind_script_test(script_rel: str):
    def _test(self: CiModuleScriptsTest) -> None:
        self._run_script(script_rel)

    _test.__name__ = _make_test_name(script_rel)
    _test.__doc__ = f"Run CI module script: {script_rel}"
    return _test


for _script_rel in _discover_module_scripts():
    setattr(CiModuleScriptsTest, _make_test_name(_script_rel), _bind_script_test(_script_rel))
