#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/ci/run_all.sh [options]

Options:
  --image <tag>         Docker image tag (default: netnexus-ci:localtest)
  --report-dir <path>   Report output directory
                        (default: scripts/ci/reports/local-<timestamp>)
  --no-build            Skip docker build step
  --keep                Keep case containers/networks for debugging
  --cmd-timeout <sec>   CLI command timeout for runtime (default: 30)
  --connect-timeout <sec>  CLI initial connect timeout (default: 60)
  --verbose-modules     Append --verbose-modules for module scripts (default: off)
  -h, --help            Show this help

Examples:
  scripts/ci/run_all.sh
  scripts/ci/run_all.sh --no-build --image netnexus-ci:latest
  scripts/ci/run_all.sh --keep
EOF
}

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT_DIR}"

IMAGE="netnexus-ci:localtest"
REPORT_DIR="scripts/ci/reports/local-$(date -u +%Y%m%d-%H%M%S)"
BUILD_IMAGE=1
KEEP_CASE=0
CMD_TIMEOUT=30
CONNECT_TIMEOUT=60
VERBOSE_MODULES=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)
      IMAGE="$2"
      shift 2
      ;;
    --report-dir)
      REPORT_DIR="$2"
      shift 2
      ;;
    --no-build)
      BUILD_IMAGE=0
      shift
      ;;
    --keep)
      KEEP_CASE=1
      shift
      ;;
    --cmd-timeout)
      CMD_TIMEOUT="$2"
      shift 2
      ;;
    --connect-timeout)
      CONNECT_TIMEOUT="$2"
      shift 2
      ;;
    --verbose-modules)
      VERBOSE_MODULES=1
      shift
      ;;
    --no-verbose-modules)
      VERBOSE_MODULES=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ "${BUILD_IMAGE}" -eq 1 ]]; then
  echo ">>> Building image: ${IMAGE}"
  docker build --target production -t "${IMAGE}" .
fi

echo ">>> Running all CI modules"
echo "    image: ${IMAGE}"
echo "    report: ${REPORT_DIR}"

CMD=(
  python3 scripts/ci/module_runner.py
  --image "${IMAGE}"
  --report-dir "${REPORT_DIR}"
  --cmd-timeout "${CMD_TIMEOUT}"
  --connect-timeout "${CONNECT_TIMEOUT}"
)
if [[ "${KEEP_CASE}" -eq 1 ]]; then
  CMD+=(--keep)
fi
if [[ "${VERBOSE_MODULES}" -eq 1 ]]; then
  CMD+=(--verbose-modules)
fi

"${CMD[@]}"

echo ">>> Done. Open report:"
echo "    ${REPORT_DIR}/report.html"
