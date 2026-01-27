#!/bin/bash
#
# NetNexus Development Debug Script
# Starts NetNexus with gdb for debugging
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
DATA_DIR="${PROJECT_ROOT}/data"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Check if build exists
if [ ! -f "${BUILD_DIR}/bin/netnexus" ]; then
    echo -e "${RED}Error: Binary not found${NC}"
    echo "Please build first: ./scripts/dev/build.sh"
    exit 1
fi

# Check if gdb is installed
if ! command -v gdb &> /dev/null; then
    echo -e "${RED}Error: gdb not found${NC}"
    echo "Install gdb: sudo apt install gdb"
    exit 1
fi

# Create data directory
mkdir -p "${DATA_DIR}"

# Set environment for development
export LD_LIBRARY_PATH="${BUILD_DIR}/lib:${LD_LIBRARY_PATH}"

echo "==================================="
echo "NetNexus Debug Mode (GDB)"
echo "==================================="
echo -e "${GREEN}Binary:${NC}  ${BUILD_DIR}/bin/netnexus"
echo -e "${GREEN}Config:${NC}  Auto-discover from src/"
echo -e "${GREEN}Data:${NC}    ${DATA_DIR}/"
echo ""
echo -e "${YELLOW}GDB Commands:${NC}"
echo "  run        - Start program"
echo "  break main - Set breakpoint at main"
echo "  continue   - Continue execution"
echo "  backtrace  - Show call stack"
echo "  print var  - Print variable"
echo "  quit       - Exit debugger"
echo "==================================="
echo ""

# Change to build/bin directory
cd "${BUILD_DIR}/bin"

# Start with gdb
exec gdb -ex "set breakpoint pending on" ./netnexus
