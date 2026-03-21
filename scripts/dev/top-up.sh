#!/bin/bash

set -euo pipefail

# ./scripts/dev/top-up.sh --top scripts/ci/modules/bgp/two-node-r1ge1-r2ge1/top.yaml --image netnexus:latest

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "${SCRIPT_DIR}/top_up.py" "$@"
