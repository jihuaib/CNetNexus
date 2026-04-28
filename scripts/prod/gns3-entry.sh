#!/bin/bash
#
# NetNexus GNS3 入口脚本
# 后台启动 netnexus，前台等待用户回车后通过 telnet 连接 CLI
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# 统一工作目录：resources/data/log 均从此派生
export NN_WORK_DIR="${INSTALL_DIR}"
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH}"

# 创建数据目录和日志目录
mkdir -p "${INSTALL_DIR}/data" 2>/dev/null
mkdir -p "${INSTALL_DIR}/log" 2>/dev/null
mkdir -p "${INSTALL_DIR}/data/cores" 2>/dev/null

# 放开 core dump 限制（GNS3 节点需在 extra_docker_options 加 --ulimit core=-1）
ulimit -c unlimited 2>/dev/null || echo "[WARN] ulimit -c unlimited failed; GNS3 node needs --ulimit core=-1"

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

# 检测 ASAN 构建：如果二进制链接了 libasan，自动配置 ASAN 运行时选项
if ldd "${INSTALL_DIR}/bin/netnexus" 2>/dev/null | grep -q libasan; then
    export ASAN_OPTIONS="detect_leaks=1:log_path=${INSTALL_DIR}/log/asan:abort_on_error=0:halt_on_error=0"
    echo "[ASAN] AddressSanitizer enabled, logs: ${INSTALL_DIR}/log/asan.*"
fi

# 注意：不切 cwd 到 cores 目录！部分模块用相对路径打开 data 文件，
# 切 cwd 会破坏路径解析。core 默认落到 cwd（即 INSTALL_DIR/bin），
# 通过 setup-coredump.sh 配置绝对路径方案见下方"core 落地位置"说明。
"${INSTALL_DIR}/bin/netnexus" </dev/null >/dev/null 2>/dev/null &
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
