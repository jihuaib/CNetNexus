#!/bin/bash
#
# NetNexus Docker 镜像发布脚本
#
# 用法:
#   ./scripts/prod/publish.sh                  # 构建所有架构（amd64 / arm64 / armv7）
#   ./scripts/prod/publish.sh amd64            # 仅指定架构
#   ./scripts/prod/publish.sh amd64 arm64      # 多个架构
#   VERSION=2.0.0 ./scripts/prod/publish.sh    # 指定版本号
#
# 输出（package/ 目录）:
#   netnexus-1.0.0-docker-amd64.tar.gz     # docker load 加载
#   netnexus-1.0.0-docker-arm64.tar.gz
#
# Docker 前置条件（一次性设置）:
#   docker run --privileged --rm tonistiigi/binfmt --install all
#   docker buildx create --name netnexus-builder --driver docker-container --driver-opt network=host --use
#   docker buildx inspect --bootstrap
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PACKAGE_DIR="${PROJECT_ROOT}/package"
DOCKER_BUILDER="netnexus-builder"
IMAGE_NAME="${IMAGE_NAME:-netnexus}"

# ============================================================
# 版本号
# ============================================================
if [ -n "${VERSION:-}" ]; then
    :
elif [ -f "${PROJECT_ROOT}/VERSION" ]; then
    VERSION=$(cat "${PROJECT_ROOT}/VERSION" | tr -d '[:space:]')
else
    VERSION="1.0.0"
fi
GIT_COMMIT=$(git -C "${PROJECT_ROOT}" rev-parse --short HEAD 2>/dev/null || echo "unknown")

# ============================================================
# 架构定义
# ============================================================
declare -A DOCKER_ARCH_INFO=(
    ["amd64"]="linux/amd64"
    ["arm64"]="linux/arm64"
)
DEFAULT_ARCHS=("amd64" "arm64")

# ============================================================
# 参数解析
# ============================================================
TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=("${DEFAULT_ARCHS[@]}")

# 校验架构名
for arch in "${TARGETS[@]}"; do
    if [ -z "${DOCKER_ARCH_INFO[$arch]+_}" ]; then
        echo "[错误] 不支持的架构: ${arch}（支持: ${!DOCKER_ARCH_INFO[*]}）"
        exit 1
    fi
done

# ============================================================
# 打印标题
# ============================================================
echo "==========================================="
echo "NetNexus Docker 发布"
echo "==========================================="
echo "版本    : ${VERSION} (${GIT_COMMIT})"
echo "架构    : ${TARGETS[*]}"
echo "输出    : ${PACKAGE_DIR}/"
echo ""

mkdir -p "${PACKAGE_DIR}"

DOCKER_OK=()
DOCKER_FAIL=()

# ============================================================
# 检查 buildx 构建器
# ============================================================
docker_check_builder() {
    if ! docker buildx ls 2>/dev/null | grep -q "${DOCKER_BUILDER}"; then
        echo "[错误] buildx 构建器 '${DOCKER_BUILDER}' 不存在，请先执行:"
        echo "  docker run --privileged --rm tonistiigi/binfmt --install all"
        echo "  docker buildx create --name ${DOCKER_BUILDER} --driver docker-container --driver-opt network=host --use"
        echo "  docker buildx inspect --bootstrap"
        return 1
    fi
    docker buildx use "${DOCKER_BUILDER}"
}

# ============================================================
# 构建并导出单个架构
# ============================================================
docker_build_export() {
    local arch="$1"
    local platform="${DOCKER_ARCH_INFO[$arch]}"
    local out_tar="${PACKAGE_DIR}/${IMAGE_NAME}-${VERSION}-docker-${arch}.tar"
    local log="${PACKAGE_DIR}/docker-build-${arch}.log"

    echo "  构建 (${platform})，日志: package/docker-build-${arch}.log"

    docker buildx build \
        --platform "${platform}" \
        --build-arg VERSION="${VERSION}" \
        --build-arg GIT_COMMIT="${GIT_COMMIT}" \
        --target production \
        --output "type=docker,dest=${out_tar}" \
        --tag "${IMAGE_NAME}:${VERSION}-${arch}" \
        --file "${PROJECT_ROOT}/Dockerfile" \
        "${PROJECT_ROOT}" \
        > "${log}" 2>&1 || { echo "  [错误] Docker 构建失败，详见 ${log}"; return 1; }

    [ -f "${out_tar}" ] || { echo "  [错误] 未找到输出文件"; return 1; }

    echo "  压缩中..."
    gzip -f "${out_tar}"
}

# ============================================================
# 主流程
# ============================================================
rc=0
docker_check_builder || rc=$?
if [ $rc -ne 0 ]; then
    exit 1
fi

for arch in "${TARGETS[@]}"; do
    echo ""
    echo "[docker:${arch}]"

    rc=0
    docker_build_export "$arch" || rc=$?

    if [ $rc -eq 0 ]; then
        size=$(du -h "${PACKAGE_DIR}/${IMAGE_NAME}-${VERSION}-docker-${arch}.tar.gz" | cut -f1)
        echo "  完成: ${IMAGE_NAME}-${VERSION}-docker-${arch}.tar.gz (${size})"
        DOCKER_OK+=("$arch")
    else
        echo "  [错误] 处理失败（退出码 ${rc}）"
        DOCKER_FAIL+=("$arch")
    fi
done

# ============================================================
# 汇总
# ============================================================
echo ""
echo "==========================================="
echo "发布完成"
echo "==========================================="
echo "成功(${#DOCKER_OK[@]}): ${DOCKER_OK[*]:-无}  失败(${#DOCKER_FAIL[@]}): ${DOCKER_FAIL[*]:-无}"

echo ""
echo "生成文件:"
for f in "${PACKAGE_DIR}/${IMAGE_NAME}-${VERSION}-docker-"*.tar.gz; do
    [ -f "$f" ] && echo "  $(du -h "$f" | cut -f1)  $(basename "$f")"
done

echo ""
echo "GitHub Release 上传后使用方式:"
echo "  docker load < ${IMAGE_NAME}-${VERSION}-docker-amd64.tar.gz"
echo "  docker run --rm -p 3788:3788 ${IMAGE_NAME}:${VERSION}-amd64"
echo ""

[ ${#DOCKER_FAIL[@]} -eq 0 ]
