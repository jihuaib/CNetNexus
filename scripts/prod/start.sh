#!/bin/bash
#
# NetNexus Startup Script
# Sets environment and starts the NetNexus server
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RESOURCE_DIR="${INSTALL_DIR}/resources"

# Set environment variables
export NN_RESOURCES_DIR="${RESOURCE_DIR}"
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH}"

# Create data directory if needed (for development)
if [ ! -d "${INSTALL_DIR}/data" ]; then
    mkdir -p "${INSTALL_DIR}/data"
fi

# Display startup information
echo "==================================="
echo "NetNexus Starting"
echo "==================================="
echo "Install dir: ${INSTALL_DIR}"
echo "Config dir:  ${RESOURCE_DIR}"
echo "Library path: ${LD_LIBRARY_PATH}"
echo ""

# Check if binary exists
if [ ! -f "${INSTALL_DIR}/bin/netnexus" ]; then
    echo "Error: netnexus binary not found at ${INSTALL_DIR}/bin/netnexus"
    exit 1
fi

# Check if resources directory exists
if [ ! -d "${RESOURCE_DIR}" ]; then
    echo "Error: Config directory not found at ${RESOURCE_DIR}"
    exit 1
fi

# Start NetNexus
echo "Starting NetNexus server..."
echo "Listening on port 3788"
echo "Press Ctrl+C to stop"
echo ""

exec "${INSTALL_DIR}/bin/netnexus"
