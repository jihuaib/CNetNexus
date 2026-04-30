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

# 提前校验 INSTALL_DIR 可写,避免到 mv/写 .image_tag 时才失败、给出含糊错误。
# 触发原因常见为容器以非 root 用户启动 / AppArmor / /opt 权限异常。
if [[ ! -d "$INSTALL_DIR" ]]; then
    fail "INSTALL_DIR=$INSTALL_DIR does not exist (or not searchable). uid=$(id -u) gid=$(id -g)"
fi
if [[ ! -w "$INSTALL_DIR" ]]; then
    owner_info="$(stat -c '%U:%G mode=%a' "$INSTALL_DIR" 2>/dev/null || echo 'unknown')"
    log "ERROR: $INSTALL_DIR not writable. current uid=$(id -u) gid=$(id -g), dir owner=$owner_info"
    log "  hint: run container as root, or 'chown -R \$(id -u):\$(id -g) $INSTALL_DIR'"
    fail "$INSTALL_DIR not writable"
fi

# 临时容器名首字符必须是 [a-zA-Z0-9],下划线开头会被 docker 拒(参见 Docker daemon 校验)
STAGE_NAME="nn-swap-$$-$(date +%s)"
ROOTFS_TAR="/tmp/${STAGE_NAME}.tar"
EXTRACT_DIR="/tmp/${STAGE_NAME}-root"

# swap 进度状态机:
#   0 = 未开始改动 INSTALL_DIR
#   1 = 已改 bin/
#   2 = 已改 bin/ + lib/
#   3 = 已改 bin/ + lib/ + resources/
EXIT_OK=0
SWAP_PHASE=0

# 把 .old 副本恢复回去。任何子操作失败都尽力继续,不中断回滚链。
rollback_swap() {
    [[ "$SWAP_PHASE" -gt 0 ]] || return 0
    log "ROLLBACK: restoring previous bin/lib/resources from .old (phase=$SWAP_PHASE) ..."

    if [[ "$SWAP_PHASE" -ge 1 && -d "${INSTALL_DIR}/bin.old" ]]; then
        rm -rf "${INSTALL_DIR}/bin" 2>/dev/null || true
        mv "${INSTALL_DIR}/bin.old" "${INSTALL_DIR}/bin" 2>/dev/null \
            || log "  rollback bin/ failed (manual fix needed)"
    fi

    if [[ "$SWAP_PHASE" -ge 2 && -d "${INSTALL_DIR}/lib.old" ]]; then
        rm -rf "${INSTALL_DIR}/lib" 2>/dev/null || true
        mv "${INSTALL_DIR}/lib.old" "${INSTALL_DIR}/lib" 2>/dev/null \
            || log "  rollback lib/ failed (manual fix needed)"
    fi

    if [[ "$SWAP_PHASE" -ge 3 && -d "${INSTALL_DIR}/resources.old" ]]; then
        # resources 是 per-file 覆盖,需要 per-file 还原;bind mount 的同样跳过
        (cd "${INSTALL_DIR}/resources.old" && find . -mindepth 1) | while IFS= read -r p; do
            rel="${p#./}"
            src="${INSTALL_DIR}/resources.old/$rel"
            dst="${INSTALL_DIR}/resources/$rel"
            if [[ -d "$src" ]]; then
                mkdir -p "$dst" 2>/dev/null || true
            else
                rm -f "$dst" 2>/dev/null
                cp -f "$src" "$dst" 2>/dev/null || true
            fi
        done
    fi

    log "ROLLBACK done; old version restored."
}

cleanup() {
    docker rm -f "$STAGE_NAME" >/dev/null 2>&1 || true
    rm -rf "$ROOTFS_TAR" "$EXTRACT_DIR" || true
    if [[ "$EXIT_OK" != "1" ]]; then
        rollback_swap
    fi
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

# 3. 原子替换 bin/、lib/、resources/。每完成一段就推进 SWAP_PHASE,
#    任何步骤异常退出 → trap → rollback_swap 把 .old 还原回来。
log "swapping bin/ ..."
rm -rf "${INSTALL_DIR}/bin.old"
[[ -d "${INSTALL_DIR}/bin" ]] && mv "${INSTALL_DIR}/bin" "${INSTALL_DIR}/bin.old"
mv "$NEW_BIN" "${INSTALL_DIR}/bin"
chmod +x "${INSTALL_DIR}/bin"/* 2>/dev/null || true
SWAP_PHASE=1

log "swapping lib/ ..."
rm -rf "${INSTALL_DIR}/lib.old"
[[ -d "${INSTALL_DIR}/lib" ]] && mv "${INSTALL_DIR}/lib" "${INSTALL_DIR}/lib.old"
mv "$NEW_LIB" "${INSTALL_DIR}/lib"
SWAP_PHASE=2

log "swapping resources/ (per-file overlay) ..."
# 不能整体 mv:web/CI 启动的容器会把 resources/ 下个别文件
# (典型:if/if_map.conf.gns3)用 docker bind mount 挂进容器;mv 旧目录时
# 删除 bind-mounted 文件会 EBUSY/EROFS。改为逐文件覆盖,bind mount 的文件
# 由宿主机管理,容器内无法替换,跳过并最后明确告知调用方。
rm -rf "${INSTALL_DIR}/resources.old"
mkdir -p "${INSTALL_DIR}/resources.old"
cp -a "${INSTALL_DIR}/resources/." "${INSTALL_DIR}/resources.old/" 2>/dev/null || true
SWAP_PHASE=3   # 此时 resources/ 即将被部分覆盖,需要 per-file 回滚

SKIPPED_FILE="/tmp/${STAGE_NAME}-skipped"
: > "$SKIPPED_FILE"

(cd "$NEW_RES" && find . -mindepth 1) | while IFS= read -r p; do
    rel="${p#./}"
    src="$NEW_RES/$rel"
    dst="${INSTALL_DIR}/resources/$rel"
    if [[ -d "$src" ]]; then
        mkdir -p "$dst"
    else
        # bind mount 的目标文件直接 cp 会 EBUSY/EROFS;先 rm 也会失败 → 跳过
        if ! { rm -f "$dst" 2>/dev/null; cp -f "$src" "$dst" 2>/dev/null; }; then
            echo "$rel" >> "$SKIPPED_FILE"
        fi
    fi
done
rm -rf "$NEW_RES"

# 把 skip 的文件清单回显给调用方,避免"看起来没换"的迷惑
if [[ -s "$SKIPPED_FILE" ]]; then
    log "NOTE: the following resources/* files are bind-mounted from the host"
    log "      and CANNOT be replaced from inside the container (this is a"
    log "      docker/kernel limitation, not a script bug):"
    while IFS= read -r f; do
        log "        - resources/$f"
    done < "$SKIPPED_FILE"
    log "      → their content stays whatever the host bind mount provides."
    log "      → if you need them updated, deploy without bind mount, or"
    log "        update the bind mount source on the host before swap."
fi
rm -f "$SKIPPED_FILE"

# 4. 记录已生效的镜像 tag(便于 show version 等审计)
echo "$IMAGE" > "${INSTALL_DIR}/.image_tag"

# 5. 旧目录延后清理(下次 swap 前删除),避免动态库句柄在主进程内仍持有时被立即回收
log "swap done, image=$IMAGE"
EXIT_OK=1   # 关键:走到这一行才标记成功,trap 才不会触发回滚
exit 0
