#!/bin/bash
#
# NetNexus Development Startup Script
# Starts NetNexus in development mode with proper environment
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
DATA_DIR="${PROJECT_ROOT}/data"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Check if build exists
if [ ! -f "${BUILD_DIR}/bin/netnexus" ]; then
    echo -e "${RED}Error: Binary not found${NC}"
    echo "Please build first: ./scripts/dev/build.sh"
    exit 1
fi

# Create data directory
mkdir -p "${DATA_DIR}"

# Set environment for development
export LD_LIBRARY_PATH="${BUILD_DIR}/lib:${LD_LIBRARY_PATH}"

# XML files will be auto-discovered from src/ in development mode
# No need to set NN_RESOURCES_DIR

echo "==================================="
echo "NetNexus Development Mode"
echo "==================================="
echo -e "${GREEN}Binary:${NC}  ${BUILD_DIR}/bin/netnexus"
echo -e "${GREEN}Config:${NC}  Auto-discover from src/"
echo -e "${GREEN}Data:${NC}    ${DATA_DIR}/"
echo -e "${GREEN}Port:${NC}    3788"
echo ""
echo -e "${YELLOW}Press Ctrl+C to stop${NC}"
echo "==================================="
echo ""

# Change to build/bin directory
cd "${BUILD_DIR}/bin"

# Start NetNexus
exec ./netnexus
