#!/usr/bin/env bash
#
# Cleanup NetNexus backend runtime resources created on host:
# - containers: nn-topo-*
# - networks:   nn-link-* / nn-stub-*
# Optional:
# - images used by nn-topo-* containers
#
# Usage:
#   ./scripts/cleanup-runtime.sh
#   ./scripts/cleanup-runtime.sh --images
#   ./scripts/cleanup-runtime.sh --dry-run
#

set -euo pipefail

DOCKER_BIN="${DOCKER_BIN:-docker}"
USE_SUDO="${USE_SUDO:-0}"
REMOVE_IMAGES=0
DRY_RUN=0

usage() {
    cat <<'EOF'
Usage:
  ./scripts/cleanup-runtime.sh [options]

Options:
  --images     Also remove images used by nn-topo-* containers
  --dry-run    Print actions only, do not execute
  -h, --help   Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --images)
            REMOVE_IMAGES=1
            ;;
        --dry-run)
            DRY_RUN=1
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
    shift
done

docker_cmd() {
    if [[ "$USE_SUDO" == "1" ]]; then
        sudo "$DOCKER_BIN" "$@"
    else
        "$DOCKER_BIN" "$@"
    fi
}

run_or_echo() {
    if [[ "$DRY_RUN" == "1" ]]; then
        echo "[dry-run] $*"
        return 0
    fi
    "$@"
}

echo "== NetNexus backend runtime cleanup =="
echo "docker: ${DOCKER_BIN} (USE_SUDO=${USE_SUDO})"
echo "remove images: ${REMOVE_IMAGES}"
echo "dry-run: ${DRY_RUN}"
echo

mapfile -t TOPO_CONTAINERS < <(docker_cmd ps -a --filter 'name=nn-topo-' --format '{{.Names}}' | sed '/^$/d' || true)
mapfile -t TOPO_IMAGES < <(docker_cmd ps -a --filter 'name=nn-topo-' --format '{{.Image}}' | sed '/^$/d' | sort -u || true)
mapfile -t TOPO_NETWORKS < <(docker_cmd network ls --format '{{.Name}}' | grep -E '^nn-(link|stub)-' || true)

if [[ "${#TOPO_CONTAINERS[@]}" -eq 0 && "${#TOPO_NETWORKS[@]}" -eq 0 ]]; then
    echo "Nothing to clean."
    exit 0
fi

if [[ "${#TOPO_CONTAINERS[@]}" -gt 0 ]]; then
    echo "[containers] removing ${#TOPO_CONTAINERS[@]} container(s)"
    for c in "${TOPO_CONTAINERS[@]}"; do
        echo "  - ${c}"
        run_or_echo docker_cmd rm -f "$c" >/dev/null 2>&1 || true
    done
else
    echo "[containers] none"
fi

if [[ "${#TOPO_NETWORKS[@]}" -gt 0 ]]; then
    echo "[networks] removing ${#TOPO_NETWORKS[@]} network(s)"
    for n in "${TOPO_NETWORKS[@]}"; do
        echo "  - ${n}"
        run_or_echo docker_cmd network rm "$n" >/dev/null 2>&1 || true
    done
else
    echo "[networks] none"
fi

if [[ "$REMOVE_IMAGES" == "1" ]]; then
    if [[ "${#TOPO_IMAGES[@]}" -gt 0 ]]; then
        echo "[images] removing ${#TOPO_IMAGES[@]} image(s) used by nn-topo-*"
        for i in "${TOPO_IMAGES[@]}"; do
            echo "  - ${i}"
            run_or_echo docker_cmd image rm "$i" >/dev/null 2>&1 || true
        done
    else
        echo "[images] none"
    fi
fi

echo
echo "Cleanup finished."
