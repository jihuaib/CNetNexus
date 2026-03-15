#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/ci/cleanup.sh [options]

Options:
  --dry-run             Show matched resources without deleting
  --name-regex <regex>  Resource name regex
                        (default: ^nn-(case|top|bgp)-)
  -h, --help            Show help

Examples:
  scripts/ci/cleanup.sh
  scripts/ci/cleanup.sh --dry-run
  scripts/ci/cleanup.sh --name-regex '^nn-case-'
EOF
}

NAME_REGEX='^nn-(case|top|bgp)-'
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --name-regex)
      NAME_REGEX="$2"
      shift 2
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

if ! command -v docker >/dev/null 2>&1; then
  echo "docker command not found" >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "cannot access docker daemon (check docker service/permissions)" >&2
  exit 1
fi

mapfile -t CONTAINERS < <(docker ps -a --format '{{.Names}}' | grep -E "${NAME_REGEX}" || true)
mapfile -t NETWORKS < <(docker network ls --format '{{.Name}}' | grep -E "${NAME_REGEX}" || true)

echo "=== matched containers (${#CONTAINERS[@]}) ==="
if [[ ${#CONTAINERS[@]} -eq 0 ]]; then
  echo "(none)"
else
  printf '%s\n' "${CONTAINERS[@]}"
fi

echo "=== matched networks (${#NETWORKS[@]}) ==="
if [[ ${#NETWORKS[@]} -eq 0 ]]; then
  echo "(none)"
else
  printf '%s\n' "${NETWORKS[@]}"
fi

if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "Dry-run mode: nothing deleted."
  exit 0
fi

if [[ ${#CONTAINERS[@]} -gt 0 ]]; then
  docker rm -f "${CONTAINERS[@]}" >/dev/null
fi

if [[ ${#NETWORKS[@]} -gt 0 ]]; then
  for net in "${NETWORKS[@]}"; do
    docker network rm "${net}" >/dev/null 2>&1 || true
  done
fi

echo "Cleanup done."
