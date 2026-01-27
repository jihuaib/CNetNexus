#!/bin/bash
#
# NetNexus Development Clean Script
# Cleans build artifacts and optional data
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
DATA_DIR="${PROJECT_ROOT}/data"
PACKAGE_DIR="${PROJECT_ROOT}/package"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Parse arguments
CLEAN_DATA=0
CLEAN_PACKAGE=0
CLEAN_ALL=0

while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--data)
            CLEAN_DATA=1
            shift
            ;;
        -p|--package)
            CLEAN_PACKAGE=1
            shift
            ;;
        -a|--all)
            CLEAN_ALL=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -d, --data      Also clean data directory"
            echo "  -p, --package   Also clean package directory"
            echo "  -a, --all       Clean everything (build + data + package)"
            echo "  -h, --help      Show this help"
            echo ""
            echo "Default: Only clean build directory"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

if [ $CLEAN_ALL -eq 1 ]; then
    CLEAN_DATA=1
    CLEAN_PACKAGE=1
fi

echo "==================================="
echo "NetNexus Development Clean"
echo "==================================="

# Clean build directory
if [ -d "${BUILD_DIR}" ]; then
    echo -e "${GREEN}[1/3]${NC} Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
    echo "  Removed: ${BUILD_DIR}"
else
    echo -e "${YELLOW}[1/3]${NC} Build directory not found (skip)"
fi

# Clean data directory
if [ $CLEAN_DATA -eq 1 ]; then
    if [ -d "${DATA_DIR}" ]; then
        echo -e "${GREEN}[2/3]${NC} Cleaning data directory..."
        rm -rf "${DATA_DIR}"
        echo "  Removed: ${DATA_DIR}"
    else
        echo -e "${YELLOW}[2/3]${NC} Data directory not found (skip)"
    fi
else
    echo -e "${YELLOW}[2/3]${NC} Data directory preserved (use -d to clean)"
fi

# Clean package directory
if [ $CLEAN_PACKAGE -eq 1 ]; then
    if [ -d "${PACKAGE_DIR}" ]; then
        echo -e "${GREEN}[3/3]${NC} Cleaning package directory..."
        rm -rf "${PACKAGE_DIR}"
        echo "  Removed: ${PACKAGE_DIR}"
    else
        echo -e "${YELLOW}[3/3]${NC} Package directory not found (skip)"
    fi
else
    echo -e "${YELLOW}[3/3]${NC} Package directory preserved (use -p to clean)"
fi

echo ""
echo "==================================="
echo "Clean complete!"
echo "==================================="
echo ""
echo "To rebuild:"
echo "  ./scripts/dev/build.sh"
echo ""
