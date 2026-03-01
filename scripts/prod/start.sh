#!/bin/bash
#
# NetNexus Startup Script
# Sets environment and starts the NetNexus server
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# 统一工作目录：resources/data/log 均从此派生
export NN_WORK_DIR="${INSTALL_DIR}"
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH}"

# 创建必要目录
mkdir -p "${INSTALL_DIR}/data"
mkdir -p "${INSTALL_DIR}/log"

# Display startup information
echo "==================================="
echo "NetNexus Starting"
echo "==================================="
echo "Install dir: ${INSTALL_DIR}"
echo "Work dir:    ${NN_WORK_DIR}"
echo "Library path: ${LD_LIBRARY_PATH}"
echo ""

# Check if binary exists
if [ ! -f "${INSTALL_DIR}/bin/netnexus" ]; then
    echo "Error: netnexus binary not found at ${INSTALL_DIR}/bin/netnexus"
    exit 1
fi

# Check if resources directory exists
if [ ! -d "${INSTALL_DIR}/resources" ]; then
    echo "Error: Config directory not found at ${INSTALL_DIR}/resources"
    exit 1
fi

# Start NetNexus
echo "Starting NetNexus server..."
echo "Listening on port 3788"
echo "Press Ctrl+C to stop"
echo ""

exec "${INSTALL_DIR}/bin/netnexus"
