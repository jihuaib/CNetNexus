#!/bin/bash
#
# NetNexus Docker 镜像发布脚本
#
# 用法:
#   ./scripts/prod/publish.sh
#   ./scripts/prod/publish.sh amd64 arm64
#   VERSION=2.0.0 ./scripts/prod/publish.sh --github-release
#   ./scripts/prod/publish.sh --github-release --token-file ./.secrets/github_token
#   ./scripts/prod/publish.sh --github-release --no-sync-tag
#   ./scripts/prod/publish.sh --publish-only
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
GIT_COMMIT_FULL=$(git -C "${PROJECT_ROOT}" rev-parse HEAD 2>/dev/null || echo "HEAD")

# ============================================================
# 架构定义
# ============================================================
declare -A DOCKER_ARCH_INFO=(
    ["amd64"]="linux/amd64"
    ["arm64"]="linux/arm64"
)
DEFAULT_ARCHS=("amd64" "arm64")

# ============================================================
# GitHub Release 参数
# ============================================================
PUBLISH_GITHUB=0
SYNC_TAG=1
PUBLISH_ONLY=0
GITHUB_REPO="${GITHUB_REPO:-}"
GITHUB_TOKEN_FILE="${GITHUB_TOKEN_FILE:-${PROJECT_ROOT}/.secrets/github_token}"
GITHUB_TOKEN_ENV="${GITHUB_TOKEN_ENV:-GITHUB_TOKEN}"
TAG_NAME="${TAG_NAME:-v${VERSION}}"
RELEASE_NAME="${RELEASE_NAME:-${IMAGE_NAME} ${VERSION}}"
TARGET_COMMITISH="${TARGET_COMMITISH:-${GIT_COMMIT_FULL}}"
TAG_COMMIT=""

TARGETS=()
DOCKER_OK=()
DOCKER_FAIL=()
GENERATED_FILES=()

print_usage() {
    cat <<EOF
用法:
  ./scripts/prod/publish.sh [选项] [amd64] [arm64]

选项:
  --github-release          构建后自动创建/更新 GitHub Release 并上传产物
  --publish-only            仅发布 package/ 现有产物到 GitHub（跳过构建与 tag 同步）
  --no-sync-tag             发布 GitHub Release 时不自动同步 git tag 到 origin
  --repo <owner/repo>       指定 GitHub 仓库（默认从 origin 自动识别）
  --token-file <path>       GitHub token 文件路径（默认: ${GITHUB_TOKEN_FILE}）
  --tag <tag_name>          Release tag（默认: ${TAG_NAME}）
  --release-name <name>     Release 标题（默认: ${RELEASE_NAME}）
  -h, --help                显示帮助

Token 读取优先级:
  1) 环境变量: ${GITHUB_TOKEN_ENV}
  2) token 文件: ${GITHUB_TOKEN_FILE}
EOF
}

die() {
    echo "[错误] $*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"
}

check_token_file_mode() {
    local file="$1"
    local mode=""
    local group_bits=0
    local other_bits=0
    mode=$(stat -c "%a" "$file" 2>/dev/null || true)
    [ -n "$mode" ] || return 0
    group_bits=$(( (10#$mode / 10) % 10 ))
    other_bits=$(( 10#$mode % 10 ))
    if [ "$group_bits" -ne 0 ] || [ "$other_bits" -ne 0 ]; then
        echo "[警告] token 文件权限过宽 (${mode})，建议执行: chmod 600 ${file}"
    fi
}

detect_github_repo() {
    local remote_url=""
    remote_url=$(git -C "${PROJECT_ROOT}" remote get-url origin 2>/dev/null || true)
    [ -n "$remote_url" ] || die "未找到 origin remote，请使用 --repo owner/repo 指定仓库"

    if [[ "$remote_url" == git@github.com:* ]]; then
        GITHUB_REPO="${remote_url#git@github.com:}"
    elif [[ "$remote_url" == https://github.com/* ]]; then
        GITHUB_REPO="${remote_url#https://github.com/}"
    elif [[ "$remote_url" == ssh://git@github.com/* ]]; then
        GITHUB_REPO="${remote_url#ssh://git@github.com/}"
    elif [[ "$remote_url" =~ ^ssh://git@ssh\.github\.com(:[0-9]+)?/(.+)$ ]]; then
        GITHUB_REPO="${BASH_REMATCH[2]}"
    else
        die "无法从 origin 识别 GitHub 仓库: ${remote_url}"
    fi

    GITHUB_REPO="${GITHUB_REPO%.git}"
    GITHUB_REPO="${GITHUB_REPO#/}"
    GITHUB_REPO="${GITHUB_REPO%/}"
    [[ "$GITHUB_REPO" =~ ^[^/]+/[^/]+$ ]] || die "仓库格式不正确: ${GITHUB_REPO}（应为 owner/repo）"
}

load_github_token() {
    if [ -n "${!GITHUB_TOKEN_ENV:-}" ]; then
        GITHUB_TOKEN="${!GITHUB_TOKEN_ENV}"
        return 0
    fi

    if [ -f "${GITHUB_TOKEN_FILE}" ]; then
        check_token_file_mode "${GITHUB_TOKEN_FILE}"
        GITHUB_TOKEN="$(tr -d '\r\n' < "${GITHUB_TOKEN_FILE}")"
    else
        die "未找到 GitHub token。请设置 ${GITHUB_TOKEN_ENV} 或创建文件 ${GITHUB_TOKEN_FILE}"
    fi

    [ -n "${GITHUB_TOKEN}" ] || die "GitHub token 为空"
}

github_api() {
    local method="$1"
    local path="$2"
    local body_file="$3"
    local out_file="$4"
    local url="https://api.github.com${path}"
    local code=""

    if [ -n "$body_file" ]; then
        code="$(curl -sS -o "$out_file" -w "%{http_code}" \
            -X "$method" \
            -H "Accept: application/vnd.github+json" \
            -H "Authorization: Bearer ${GITHUB_TOKEN}" \
            -H "X-GitHub-Api-Version: 2022-11-28" \
            --data-binary "@${body_file}" \
            "$url")"
    else
        code="$(curl -sS -o "$out_file" -w "%{http_code}" \
            -X "$method" \
            -H "Accept: application/vnd.github+json" \
            -H "Authorization: Bearer ${GITHUB_TOKEN}" \
            -H "X-GitHub-Api-Version: 2022-11-28" \
            "$url")"
    fi

    printf "%s" "$code"
}

sync_git_tag_with_origin() {
    local target_commit=""
    local local_tag_commit=""
    local remote_tag_commit=""

    require_cmd git

    target_commit="$(git -C "${PROJECT_ROOT}" rev-parse "${TARGET_COMMITISH}^{commit}" 2>/dev/null || true)"
    [ -n "$target_commit" ] || die "无效 TARGET_COMMITISH: ${TARGET_COMMITISH}"

    echo ""
    echo "[git-tag]"
    echo "  标签: ${TAG_NAME}"
    echo "  目标: ${target_commit}"

    if git -C "${PROJECT_ROOT}" rev-parse -q --verify "refs/tags/${TAG_NAME}" >/dev/null; then
        local_tag_commit="$(git -C "${PROJECT_ROOT}" rev-list -n1 "${TAG_NAME}")"
        if [ "$local_tag_commit" != "$target_commit" ]; then
            die "本地 tag ${TAG_NAME} 指向 ${local_tag_commit}，与目标提交 ${target_commit} 不一致"
        fi
        echo "  本地 tag 已存在且一致"
    else
        git -C "${PROJECT_ROOT}" tag -a "${TAG_NAME}" -m "Release ${TAG_NAME}" "${target_commit}"
        local_tag_commit="${target_commit}"
        echo "  已创建本地 tag: ${TAG_NAME}"
    fi

    remote_tag_commit="$(git -C "${PROJECT_ROOT}" ls-remote --quiet --tags origin "refs/tags/${TAG_NAME}^{}" | awk 'NR==1{print $1}')"
    if [ -z "$remote_tag_commit" ]; then
        remote_tag_commit="$(git -C "${PROJECT_ROOT}" ls-remote --quiet --tags origin "refs/tags/${TAG_NAME}" | awk 'NR==1{print $1}')"
    fi

    if [ -z "$remote_tag_commit" ]; then
        git -C "${PROJECT_ROOT}" push origin "refs/tags/${TAG_NAME}"
        echo "  已推送 tag 到 origin"
    elif [ "$remote_tag_commit" = "$local_tag_commit" ]; then
        echo "  origin tag 已同步"
    else
        die "origin tag ${TAG_NAME} 指向 ${remote_tag_commit}，与本地 ${local_tag_commit} 不一致，请先手动处理"
    fi

    TAG_COMMIT="$local_tag_commit"
}

publish_github_release() {
    local tmp_resp=""
    local payload=""
    local assets_resp=""
    local delete_resp=""
    local upload_resp=""
    local code=""
    local upload_url=""
    local release_id=""
    local release_url=""
    local existing_asset_id=""
    local asset_path=""
    local asset_name=""
    local upload_code=""
    local release_target=""

    require_cmd curl
    require_cmd jq

    if [ -z "$GITHUB_REPO" ]; then
        detect_github_repo
    fi
    load_github_token
    release_target="${TAG_COMMIT:-${TARGET_COMMITISH}}"

    echo ""
    echo "[github-release]"
    echo "  仓库: ${GITHUB_REPO}"
    echo "  标签: ${TAG_NAME}"

    tmp_resp="$(mktemp)"
    code="$(github_api GET "/repos/${GITHUB_REPO}/releases/tags/${TAG_NAME}" "" "$tmp_resp")"

    if [ "$code" = "200" ]; then
        echo "  使用已存在 Release: ${TAG_NAME}"
    elif [ "$code" = "404" ]; then
        payload="$(mktemp)"
        jq -n \
            --arg tag "${TAG_NAME}" \
            --arg target "${release_target}" \
            --arg name "${RELEASE_NAME}" \
            '{tag_name:$tag,target_commitish:$target,name:$name,draft:false,prerelease:false,generate_release_notes:true}' \
            > "$payload"

        code="$(github_api POST "/repos/${GITHUB_REPO}/releases" "$payload" "$tmp_resp")"
        rm -f "$payload"
        payload=""
        if [ "$code" != "201" ]; then
            echo "  [错误] 创建 Release 失败 (HTTP ${code})"
            cat "$tmp_resp"
            rm -f "$tmp_resp"
            return 1
        fi
        echo "  已创建 Release: ${TAG_NAME}"
    else
        echo "  [错误] 查询 Release 失败 (HTTP ${code})"
        cat "$tmp_resp"
        rm -f "$tmp_resp"
        return 1
    fi

    release_id="$(jq -r '.id // empty' "$tmp_resp")"
    upload_url="$(jq -r '.upload_url // empty' "$tmp_resp" | sed 's/{.*$//')"
    release_url="$(jq -r '.html_url // empty' "$tmp_resp")"
    [ -n "$release_id" ] || { rm -f "$tmp_resp"; die "Release id 解析失败"; }
    [ -n "$upload_url" ] || { rm -f "$tmp_resp"; die "Release upload_url 解析失败"; }

    assets_resp="$(mktemp)"
    code="$(github_api GET "/repos/${GITHUB_REPO}/releases/${release_id}/assets?per_page=100" "" "$assets_resp")"
    [ "$code" = "200" ] || {
        echo "  [错误] 获取 assets 列表失败 (HTTP ${code})"
        cat "$assets_resp"
        rm -f "$tmp_resp" "$assets_resp"
        return 1
    }

    for asset_path in "${GENERATED_FILES[@]}"; do
        [ -f "$asset_path" ] || continue
        asset_name="$(basename "$asset_path")"

        existing_asset_id="$(jq -r --arg n "$asset_name" '.[] | select(.name==$n) | .id' "$assets_resp" | head -n1)"
        if [ -n "$existing_asset_id" ]; then
            delete_resp="$(mktemp)"
            code="$(github_api DELETE "/repos/${GITHUB_REPO}/releases/assets/${existing_asset_id}" "" "$delete_resp")"
            if [ "$code" != "204" ]; then
                echo "  [错误] 删除已存在资产失败: ${asset_name} (HTTP ${code})"
                cat "$delete_resp"
                rm -f "$tmp_resp" "$assets_resp" "$delete_resp"
                return 1
            fi
            rm -f "$delete_resp"
            echo "  替换资产: ${asset_name}"
        fi

        upload_resp="$(mktemp)"
        upload_code="$(curl -sS -o "$upload_resp" -w "%{http_code}" \
            -X POST \
            -H "Accept: application/vnd.github+json" \
            -H "Authorization: Bearer ${GITHUB_TOKEN}" \
            -H "X-GitHub-Api-Version: 2022-11-28" \
            -H "Content-Type: application/gzip" \
            --data-binary "@${asset_path}" \
            "${upload_url}?name=${asset_name}")"

        if [ "$upload_code" != "201" ]; then
            echo "  [错误] 上传失败: ${asset_name} (HTTP ${upload_code})"
            cat "$upload_resp"
            rm -f "$tmp_resp" "$assets_resp" "$upload_resp"
            return 1
        fi

        rm -f "$upload_resp"
        echo "  上传成功: ${asset_name}"
    done

    rm -f "$tmp_resp" "$assets_resp"

    if [ -n "$release_url" ]; then
        echo "  Release 地址: ${release_url}"
    fi
}

collect_existing_artifacts() {
    local file=""
    local arch=""
    local found=0
    local prefix=""
    local base=""

    prefix="${IMAGE_NAME}-${VERSION}-docker-"

    if [ ${#TARGETS[@]} -eq 0 ]; then
        shopt -s nullglob
        for file in "${PACKAGE_DIR}/${prefix}"*.tar.gz; do
            [ -f "$file" ] || continue
            found=1
            GENERATED_FILES+=("$file")
            base="$(basename "$file")"
            arch="${base#${prefix}}"
            arch="${arch%.tar.gz}"
            DOCKER_OK+=("$arch")
        done
        shopt -u nullglob
        [ "$found" -eq 1 ] || die "publish-only 模式未找到产物：${PACKAGE_DIR}/${prefix}*.tar.gz"
        return 0
    fi

    for arch in "${TARGETS[@]}"; do
        file="${PACKAGE_DIR}/${prefix}${arch}.tar.gz"
        if [ -f "$file" ]; then
            GENERATED_FILES+=("$file")
            DOCKER_OK+=("$arch")
        else
            DOCKER_FAIL+=("$arch")
        fi
    done
}

# ============================================================
# 参数解析
# ============================================================
while [ $# -gt 0 ]; do
    case "$1" in
        --github-release)
            PUBLISH_GITHUB=1
            ;;
        --publish-only)
            PUBLISH_ONLY=1
            PUBLISH_GITHUB=1
            SYNC_TAG=0
            ;;
        --no-sync-tag)
            SYNC_TAG=0
            ;;
        --repo)
            shift
            [ $# -gt 0 ] || die "--repo 需要参数: owner/repo"
            GITHUB_REPO="$1"
            ;;
        --token-file)
            shift
            [ $# -gt 0 ] || die "--token-file 需要文件路径"
            GITHUB_TOKEN_FILE="$1"
            ;;
        --tag)
            shift
            [ $# -gt 0 ] || die "--tag 需要参数"
            TAG_NAME="$1"
            ;;
        --release-name)
            shift
            [ $# -gt 0 ] || die "--release-name 需要参数"
            RELEASE_NAME="$1"
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        --*)
            die "未知参数: $1"
            ;;
        *)
            TARGETS+=("$1")
            ;;
    esac
    shift
done

[ ${#TARGETS[@]} -eq 0 ] && [ "$PUBLISH_ONLY" -eq 0 ] && TARGETS=("${DEFAULT_ARCHS[@]}")

# 校验架构名
if [ "$PUBLISH_ONLY" -eq 0 ]; then
    for arch in "${TARGETS[@]}"; do
        if [ -z "${DOCKER_ARCH_INFO[$arch]+_}" ]; then
            die "不支持的架构: ${arch}（支持: ${!DOCKER_ARCH_INFO[*]}）"
        fi
    done
fi

# ============================================================
# 打印标题
# ============================================================
echo "==========================================="
echo "NetNexus Docker 发布"
echo "==========================================="
echo "版本    : ${VERSION} (${GIT_COMMIT})"
if [ "$PUBLISH_ONLY" -eq 1 ]; then
    if [ ${#TARGETS[@]} -eq 0 ]; then
        echo "架构    : 自动识别（package/ 现有产物）"
    else
        echo "架构    : ${TARGETS[*]}"
    fi
else
    echo "架构    : ${TARGETS[*]}"
fi
echo "输出    : ${PACKAGE_DIR}/"
if [ "$PUBLISH_GITHUB" -eq 1 ]; then
    echo "GitHub  : 启用自动 Release 上传"
fi
if [ "$PUBLISH_ONLY" -eq 1 ]; then
    echo "模式    : publish-only（跳过构建，跳过 tag 同步）"
fi
echo ""

mkdir -p "${PACKAGE_DIR}"

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
if [ "$PUBLISH_ONLY" -eq 1 ]; then
    echo "[publish-only] 跳过镜像构建，读取 package/ 现有产物"
    collect_existing_artifacts
else
    rc=0
    docker_check_builder || rc=$?
    if [ "$rc" -ne 0 ]; then
        exit 1
    fi

    for arch in "${TARGETS[@]}"; do
        echo ""
        echo "[docker:${arch}]"

        rc=0
        docker_build_export "$arch" || rc=$?

        if [ "$rc" -eq 0 ]; then
            local_file="${PACKAGE_DIR}/${IMAGE_NAME}-${VERSION}-docker-${arch}.tar.gz"
            size=$(du -h "${local_file}" | cut -f1)
            echo "  完成: $(basename "${local_file}") (${size})"
            DOCKER_OK+=("$arch")
            GENERATED_FILES+=("${local_file}")
        else
            echo "  [错误] 处理失败（退出码 ${rc}）"
            DOCKER_FAIL+=("$arch")
        fi
    done
fi

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
for f in "${GENERATED_FILES[@]}"; do
    [ -f "$f" ] && echo "  $(du -h "$f" | cut -f1)  $(basename "$f")"
done

if [ "$PUBLISH_GITHUB" -eq 1 ]; then
    if [ ${#DOCKER_FAIL[@]} -ne 0 ]; then
        echo ""
        echo "[警告] 存在构建失败架构，跳过 GitHub Release 上传。"
    elif [ ${#GENERATED_FILES[@]} -eq 0 ]; then
        echo ""
        echo "[警告] 未生成发布文件，跳过 GitHub Release 上传。"
    else
        if [ "$SYNC_TAG" -eq 1 ]; then
            sync_git_tag_with_origin
        else
            echo ""
            echo "[git-tag] 跳过 tag 同步（--no-sync-tag）"
        fi
        publish_github_release
    fi
else
    echo ""
    echo "GitHub Release 上传后使用方式:"
    echo "  docker load < ${IMAGE_NAME}-${VERSION}-docker-amd64.tar.gz"
    echo "  docker run --rm --cap-add NET_ADMIN --cap-add NET_RAW \\"
    echo "    --sysctl net.ipv6.conf.all.disable_ipv6=0 \\"
    echo "    --sysctl net.ipv6.conf.default.disable_ipv6=0 \\"
    echo "    --name netnexus ${IMAGE_NAME}:${VERSION}-amd64"
    echo "  docker exec -it -e NN_CONSOLE_SOCK=/opt/netnexus/run/console.sock netnexus /opt/netnexus/bin/netnexus-console"
fi

echo ""
[ ${#DOCKER_FAIL[@]} -eq 0 ]
