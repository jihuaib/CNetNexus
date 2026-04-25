#!/bin/bash
#
# 宿主机一次性配置脚本：让 netnexus（含 docker/GNS3 容器）崩溃时能产出 core
#
# 背景：
#   /proc/sys/kernel/core_pattern 是内核全局参数，所有容器共享宿主机的设置，
#   无法在容器内单独修改。Ubuntu 默认是 "|/usr/share/apport/apport ..."，
#   会把 core 管道送给宿主机的 apport，难以收集。
#
# 方案：
#   把 core_pattern 改成绝对路径 "/var/lib/netnexus-cores/core.%e.%p.%t"，
#   崩溃时内核在 *进程自己的文件系统命名空间* 下写入此路径。
#   - 宿主机：直接用 /var/lib/netnexus-cores/
#   - 容器：在容器里挂载或 mkdir /var/lib/netnexus-cores 即可（推荐用 volume
#     从宿主机挂载，便于事后 gdb 分析）
#
# 用法：
#   sudo ./scripts/host/setup-coredump.sh           # 仅本次启动生效
#   sudo ./scripts/host/setup-coredump.sh --persist # 写入 sysctl 永久生效
#   sudo ./scripts/host/setup-coredump.sh --revert  # 恢复 apport
#

set -e

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: must run as root (sudo)" >&2
    exit 1
fi

CORE_DIR="/var/lib/netnexus-cores"
CORE_PATTERN="${CORE_DIR}/core.%e.%p.%t"
SYSCTL_FILE="/etc/sysctl.d/50-netnexus-coredump.conf"

ensure_core_dir()
{
    mkdir -p "${CORE_DIR}"
    chmod 1777 "${CORE_DIR}"  # sticky + world-writable，普通用户和容器内进程都能写
}

case "${1:-}" in
    --revert)
        echo "[*] Restoring core_pattern to apport default..."
        if [ -x /usr/share/apport/apport ]; then
            echo "|/usr/share/apport/apport -p%p -s%s -c%c -d%d -P%P -u%u -g%g -F%F -- %E" > /proc/sys/kernel/core_pattern
        else
            echo "core" > /proc/sys/kernel/core_pattern
        fi
        rm -f "${SYSCTL_FILE}"
        echo "[OK] reverted (cores in ${CORE_DIR} kept)"
        ;;
    --persist)
        ensure_core_dir
        echo "[*] Setting core_pattern = ${CORE_PATTERN} (runtime + persistent)"
        echo "${CORE_PATTERN}" > /proc/sys/kernel/core_pattern
        cat > "${SYSCTL_FILE}" <<EOF
# NetNexus: absolute core path so containers + host all dump to a known location
kernel.core_pattern = ${CORE_PATTERN}
kernel.core_uses_pid = 0
EOF
        echo "[OK] runtime + ${SYSCTL_FILE}"
        ;;
    "")
        ensure_core_dir
        echo "[*] Setting core_pattern = ${CORE_PATTERN} (runtime only, lost on reboot)"
        echo "${CORE_PATTERN}" > /proc/sys/kernel/core_pattern
        echo "[OK] runtime"
        echo "[hint] use --persist to keep across reboots"
        ;;
    *)
        echo "Usage: $0 [--persist|--revert]" >&2
        exit 1
        ;;
esac

echo ""
echo "Current core_pattern: $(cat /proc/sys/kernel/core_pattern)"
echo "Cores directory:      ${CORE_DIR}  (perms: $(stat -c '%a' ${CORE_DIR} 2>/dev/null))"
echo ""
echo "For Docker containers, mount this dir as volume:"
echo "  -v ${CORE_DIR}:${CORE_DIR}"
echo "For GNS3 nodes, add to extra_volumes in netnexus.gns3a:"
echo "  \"extra_volumes\": [\"${CORE_DIR}\"]"
