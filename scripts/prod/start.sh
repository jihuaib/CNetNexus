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

# 某些环境（常见于部分 x86 线上节点）默认将容器内 IPv6 关闭，导致地址下发报 Permission denied。
# 这里尽力开启 all/default/当前接口的 IPv6；失败仅告警，不中断启动。
if [ -d /proc/sys/net/ipv6/conf ]; then
    ipv6_set_ok=0
    ipv6_set_fail=0
    for f in /proc/sys/net/ipv6/conf/*/disable_ipv6; do
        [ -f "$f" ] || continue
        if echo 0 > "$f" 2>/dev/null; then
            ipv6_set_ok=1
        else
            ipv6_set_fail=1
        fi
    done
    if [ "$ipv6_set_fail" -eq 1 ]; then
        echo "[WARN] IPv6 enable attempt partially failed; please ensure container has NET_ADMIN and IPv6 sysctls enabled"
    elif [ "$ipv6_set_ok" -eq 1 ]; then
        echo "[INFO] IPv6 sysctl enabled in container"
    fi
fi

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
