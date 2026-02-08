#!/bin/bash
#
# NetNexus GNS3 入口脚本
# 后台启动 netnexus，前台等待用户回车后通过 telnet 连接 CLI
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# 设置环境变量
export RESOURCES_DIR="${INSTALL_DIR}/resources"
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH}"

# 创建数据目录和日志目录
mkdir -p "${INSTALL_DIR}/data" 2>/dev/null
mkdir -p "${INSTALL_DIR}/log" 2>/dev/null

LOG_FILE="${INSTALL_DIR}/log/netnexus.log"

# 后台启动 netnexus，日志重定向到文件
"${INSTALL_DIR}/bin/netnexus" > "${LOG_FILE}" 2>&1 &
NETNEXUS_PID=$!

# 等待端口就绪
for i in $(seq 1 30); do
    if netstat -tlnp 2>/dev/null | grep -q ':3788'; then
        break
    fi
    sleep 0.5
done

echo ""
echo "========================================"
echo "        NetNexus Network Device"
echo "========================================"
echo ""
echo "  Press ENTER to connect to CLI..."
echo ""

# 循环：断开后可以重新回车连接
while kill -0 $NETNEXUS_PID 2>/dev/null; do
    read -r
    telnet 127.0.0.1 3788
    echo ""
    echo "  Connection closed."
    echo "  Press ENTER to reconnect..."
    echo ""
done
