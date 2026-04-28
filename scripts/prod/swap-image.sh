#!/usr/bin/env bash
#
# swap-image.sh - 在不重建容器的前提下,把容器内 NetNexus 的 bin/lib 替换为
#                 指定镜像中的对应内容。供 dev CLI 命令 `dev swap-image <image>`
#                 调用。脚本运行在容器内部,通过挂载的 docker.sock 访问宿主 docker。
#
# 用法:
#   swap-image.sh <image>
#
# 退出码:
#   0  成功(调用方随后会触发软件 reboot)
#   非0 失败,标准输出包含失败原因
#
# 前置条件:
#   - 容器内可执行 `docker`(/usr/bin/docker 已安装)
#   - /var/run/docker.sock 已挂载进容器
#   - 目标镜像已经在宿主 docker 上存在(本脚本不做 docker pull)
#   - 新旧镜像 base 兼容(glibc/动态库 ABI 一致)
#

set -euo pipefail

IMAGE="${1:-}"
INSTALL_DIR="${NN_WORK_DIR:-/opt/netnexus}"

log() { echo "[swap-image $(date '+%H:%M:%S')] $*"; }
fail() { echo "[swap-image ERROR] $*" >&2; exit 1; }

[[ -n "$IMAGE" ]] || fail "missing <image> argument"

# 校验镜像名:仅允许 [A-Za-z0-9._:/-]
if [[ ! "$IMAGE" =~ ^[A-Za-z0-9._:/-]+$ ]]; then
    fail "invalid image name: $IMAGE"
fi

command -v docker >/dev/null 2>&1 || fail "docker CLI not found inside container"
[[ -S /var/run/docker.sock ]] || fail "/var/run/docker.sock not mounted"

docker image inspect "$IMAGE" >/dev/null 2>&1 \
    || fail "image '$IMAGE' not found on host docker (please pull first)"

# 临时容器名首字符必须是 [a-zA-Z0-9],下划线开头会被 docker 拒(参见 Docker daemon 校验)
STAGE_NAME="nn-swap-$$-$(date +%s)"
ROOTFS_TAR="/tmp/${STAGE_NAME}.tar"
EXTRACT_DIR="/tmp/${STAGE_NAME}-root"

cleanup() {
    docker rm -f "$STAGE_NAME" >/dev/null 2>&1 || true
    rm -rf "$ROOTFS_TAR" "$EXTRACT_DIR" || true
}
trap cleanup EXIT

# 1. 从新镜像导出 rootfs(不启动)
log "exporting rootfs from $IMAGE ..."
docker create --name "$STAGE_NAME" "$IMAGE" >/dev/null
docker export "$STAGE_NAME" -o "$ROOTFS_TAR"
docker rm "$STAGE_NAME" >/dev/null

# 2. 解出 bin/、lib/、resources/(数据库 data/ 在 volume 上,不动)
log "extracting bin/ , lib/ and resources/ ..."
mkdir -p "$EXTRACT_DIR"
# 兼容镜像内 NetNexus 安装在 /opt/netnexus 的标准路径
tar -xf "$ROOTFS_TAR" -C "$EXTRACT_DIR" \
    opt/netnexus/bin opt/netnexus/lib opt/netnexus/resources \
    || fail "tar extract failed (image may not contain /opt/netnexus/{bin,lib,resources})"

NEW_BIN="$EXTRACT_DIR/opt/netnexus/bin"
NEW_LIB="$EXTRACT_DIR/opt/netnexus/lib"
NEW_RES="$EXTRACT_DIR/opt/netnexus/resources"
[[ -d "$NEW_BIN" && -d "$NEW_LIB" && -d "$NEW_RES" ]] \
    || fail "new image missing bin/, lib/ or resources/"
[[ -f "$NEW_BIN/netnexus" ]] || fail "new image missing bin/netnexus"

# 3. 备份当前版本(用于回滚审计;最近一次保留)
log "backing up current bin/lib/resources to ${INSTALL_DIR}/.prev ..."
rm -rf "${INSTALL_DIR}/.prev"
mkdir -p "${INSTALL_DIR}/.prev"
cp -a "${INSTALL_DIR}/bin"       "${INSTALL_DIR}/.prev/" 2>/dev/null || true
cp -a "${INSTALL_DIR}/lib"       "${INSTALL_DIR}/.prev/" 2>/dev/null || true
cp -a "${INSTALL_DIR}/resources" "${INSTALL_DIR}/.prev/" 2>/dev/null || true

# 4. 原子替换 bin/、lib/、resources/(先 rename old → new,失败可回退)
log "swapping bin/ ..."
rm -rf "${INSTALL_DIR}/bin.old"
[[ -d "${INSTALL_DIR}/bin" ]] && mv "${INSTALL_DIR}/bin" "${INSTALL_DIR}/bin.old"
mv "$NEW_BIN" "${INSTALL_DIR}/bin"
chmod +x "${INSTALL_DIR}/bin"/* 2>/dev/null || true

log "swapping lib/ ..."
rm -rf "${INSTALL_DIR}/lib.old"
[[ -d "${INSTALL_DIR}/lib" ]] && mv "${INSTALL_DIR}/lib" "${INSTALL_DIR}/lib.old"
mv "$NEW_LIB" "${INSTALL_DIR}/lib"

log "swapping resources/ (per-file overlay) ..."
# 不能整体 mv:CI / 调试场景会把 resources/ 下的某些文件
# (如 if/if_map.conf.gns3)用 docker bind mount 单独挂进容器;mv 旧目录时
# 删除 bind-mounted 文件会 EBUSY。改为逐文件覆盖,bind mount 的文件保持外部
# 提供的内容,跳过即可。
rm -rf "${INSTALL_DIR}/resources.old"
mkdir -p "${INSTALL_DIR}/resources.old"
cp -a "${INSTALL_DIR}/resources/." "${INSTALL_DIR}/resources.old/" 2>/dev/null || true

(cd "$NEW_RES" && find . -mindepth 1) | while IFS= read -r p; do
    rel="${p#./}"
    src="$NEW_RES/$rel"
    dst="${INSTALL_DIR}/resources/$rel"
    if [[ -d "$src" ]]; then
        mkdir -p "$dst"
    else
        # bind mount 的目标文件直接 cp 会 EBUSY;先 rm 也会失败 → 视为外部托管文件,跳过
        if ! { rm -f "$dst" 2>/dev/null; cp -f "$src" "$dst" 2>/dev/null; }; then
            log "  skip bind-mounted/busy: $rel"
        fi
    fi
done
rm -rf "$NEW_RES"

# 5. 记录已生效的镜像 tag(便于 show version 等审计)
echo "$IMAGE" > "${INSTALL_DIR}/.image_tag"

# 6. 旧目录延后清理(下次 swap 前删除),避免动态库句柄在主进程内仍持有时被立即回收
log "swap done, image=$IMAGE"
exit 0
