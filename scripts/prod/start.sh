#!/bin/bash
#
# NetNexus Startup Script
# Sets environment and starts the NetNexus server
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# 统一工作目录：resources/data/log 均从此派生
export NN_WORK_DIR="${INSTALL_DIR}"
export NN_CONSOLE_SOCK="${INSTALL_DIR}/run/console.sock"
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH}"

# 创建必要目录
mkdir -p "${INSTALL_DIR}/data"
mkdir -p "${INSTALL_DIR}/log"
mkdir -p "${INSTALL_DIR}/run"
mkdir -p "${INSTALL_DIR}/data/cores"

# 放开 core dump 限制（异常退出时尽可能产出 core）
# 注意：容器内还需 docker 启动时带 --ulimit core=-1 才能突破 daemon 默认硬上限
ulimit -c unlimited 2>/dev/null || echo "[WARN] ulimit -c unlimited failed; check docker --ulimit core=-1"


check_mpls_modules()
{
    local modules="mpls_router mpls_iptunnel mpls_gso"
    local missing=""

    for mod in ${modules}; do
        if ! grep -qw "^${mod} " /proc/modules 2>/dev/null; then
            missing="${missing} ${mod}"
        fi
    done

    if [ -z "${missing}" ]; then
        echo "[INFO] MPLS kernel modules ready"
    else
        echo "[WARN] MPLS kernel modules missing on host:${missing}; MPLS forwarding may not work"
        echo "[WARN] Load them on the host: sudo modprobe mpls_router mpls_iptunnel mpls_gso"
    fi
}

# /proc/modules is the kernel module view. Startup only reports MPLS readiness;
# host module loading belongs in deploy.sh or manual host setup.
check_mpls_modules

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
echo "Console socket: ${NN_WORK_DIR}/run/console.sock"
echo "Press Ctrl+C to stop"
echo ""

# 注意：不切 cwd 到 cores 目录！部分模块用相对路径打开 data 文件，
# 切 cwd 会破坏路径解析。core 文件默认会落到 cwd（即 INSTALL_DIR/bin）。
exec "${INSTALL_DIR}/bin/netnexus"
