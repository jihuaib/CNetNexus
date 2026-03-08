#!/bin/bash
#
# GNS3 热重载脚本：重新构建 Docker 镜像并清除旧容器
#
# 用法:
#   ./scripts/dev/gns3-reload.sh            # 构建 + 清理容器
#   ./scripts/dev/gns3-reload.sh --clean    # 同上，强制清理（包括运行中的容器）
#
# 流程:
#   1. 重新构建 netnexus Docker 镜像
#   2. 找出所有基于该镜像的 GNS3 容器并删除
#   3. 下次在 GNS3 中点击"Start"，节点自动从新镜像创建
#
# 无需在 GNS3 中删除设备或重新组网。
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

IMAGE_NAME="netnexus"
FORCE=0

for arg in "$@"; do
    case "$arg" in
        --clean) FORCE=1 ;;
        *) echo "用法: $0 [--clean]"; exit 1 ;;
    esac
done

# ============================================================
# 1. 构建新镜像
# ============================================================
echo -e "${YELLOW}[1/2] 构建 Docker 镜像...${NC}"
"$SCRIPT_DIR/build-gns3-image.sh"

# ============================================================
# 2. 清理基于该镜像的所有旧容器
# ============================================================
echo ""
echo -e "${YELLOW}[2/2] 清理旧容器...${NC}"

# GNS3 用镜像 ID 而非镜像名启动容器，按容器名前缀 "GNS3.<IMAGE_NAME>" 匹配
GNS3_PREFIX="GNS3.${IMAGE_NAME}"
CONTAINER_IDS=$(docker ps -a --format "{{.ID}}\t{{.Names}}\t{{.Status}}" \
    | awk -F'\t' -v p="$GNS3_PREFIX" '$2 ~ p {print $1}')

if [ -z "$CONTAINER_IDS" ]; then
    echo "  没有找到名称含 ${GNS3_PREFIX} 的容器"
else
    echo "  找到以下容器:"
    docker ps -a --format "{{.ID}}\t{{.Names}}\t{{.Status}}" \
        | awk -F'\t' -v p="$GNS3_PREFIX" '$2 ~ p {printf "    %s  %-55s  %s\n", $1, $2, $3}'
    echo ""

    # 检查是否有运行中的容器
    RUNNING=$(docker ps --format "{{.ID}}\t{{.Names}}" \
        | awk -F'\t' -v p="$GNS3_PREFIX" '$2 ~ p {print $1}')
    if [ -n "$RUNNING" ] && [ "$FORCE" -eq 0 ]; then
        echo -e "${RED}  警告: 存在运行中的容器，请先在 GNS3 中停止所有节点，或使用 --clean 强制删除${NC}"
        exit 1
    fi

    # 强制停止并删除
    COUNT=$(echo "$CONTAINER_IDS" | wc -w)
    docker rm -f $CONTAINER_IDS > /dev/null
    echo -e "  ${GREEN}已删除 ${COUNT} 个旧容器${NC}"
fi

# ============================================================
# 完成
# ============================================================
echo ""
echo -e "${GREEN}======================================"
echo "完成！"
echo -e "======================================${NC}"
echo ""
NEW_ID=$(docker inspect --format '{{.Id}}' "${IMAGE_NAME}:latest" 2>/dev/null | cut -c1-12)
echo "新镜像 ID: ${NEW_ID}"
echo ""
echo "下一步："
echo "  1. 在 GNS3 中右键节点 → Change template（或 Edit）→ 重新选择镜像 netnexus:latest"
echo "     （GNS3 记录的是旧镜像 ID，需手动更新一次，之后无需重复）"
echo "  2. 点击 Start，GNS3 会从新镜像创建容器，无需重新组网"
echo ""
