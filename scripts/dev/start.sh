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

# Coredump 目录：异常退出时 core 文件落到这里（依赖宿主机 core_pattern 是相对名）
CORE_DIR="${DATA_DIR}/cores"
mkdir -p "${CORE_DIR}"

# 放开 core dump 限制（继承到 netnexus 进程）
ulimit -c unlimited

# Set environment for development
export LD_LIBRARY_PATH="${BUILD_DIR}/lib:${LD_LIBRARY_PATH}"

# XML files will be auto-discovered from src/ in development mode
# No need to set NN_WORK_DIR in dev mode

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

# 注意：不要 cd 到 cores 目录！部分模块（SQLite 等）用 cwd 相对路径打开
# data/netnexus.db，切 cwd 会导致数据库被建到错误位置。
# core 文件会落到 cwd（即 build/bin/），需要时 mv 到 cores 目录或 grep 查找。
cd "${BUILD_DIR}/bin"

# Start NetNexus
exec ./netnexus
