#!/usr/bin/env bash
#
# 打包后端源码为 tar.gz，目标机解压后 npm ci --omit=dev 即可运行。
# 产物不包含 node_modules（方式 A：目标机联网装依赖）。
#
# 用法：
#   ./package.sh                    # 产出 dist/netnexus-backend-<date>-<sha>.tar.gz
#   ./package.sh -o foo.tar.gz      # 指定输出文件
#   VERSION=1.2.0 ./package.sh      # 自定义版本串
#

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

OUTPUT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--output) OUTPUT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "unknown option: $1"; exit 2 ;;
    esac
done

DATE="$(date +%Y%m%d)"
SHA="$(cd "$HERE/../.." && git rev-parse --short HEAD 2>/dev/null || echo nogit)"
VERSION="${VERSION:-${DATE}-${SHA}}"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

PKG_DIR="$STAGE/netnexus-backend-${VERSION}"
mkdir -p "$PKG_DIR"

# 1) 拷源码（只挑发布需要的文件，避免把 node_modules / 日志塞进去）
echo "[package] staging source -> $PKG_DIR"
cp -v \
    "$HERE/server.js" \
    "$HERE/config.js" \
    "$HERE/validation.js" \
    "$HERE/package.json" \
    "$HERE/package-lock.json" \
    "$HERE/DEPLOY.md" \
    "$PKG_DIR/"

# 1.1) 运行脚本（清空容器/网络等）一并打包
if [[ -d "$HERE/scripts" ]]; then
    echo "[package] staging scripts -> $PKG_DIR/scripts"
    mkdir -p "$PKG_DIR/scripts"
    cp -av "$HERE/scripts/." "$PKG_DIR/scripts/"
fi

# 2) 附带一个 install.sh，目标机解压后一条命令到位
cat > "$PKG_DIR/install.sh" <<'INSTALL_EOF'
#!/usr/bin/env bash
# 在目标机运行：解压后 cd 进来 sudo ./install.sh
set -euo pipefail

APP_USER="${APP_USER:-nn-backend}"
APP_DIR="${APP_DIR:-/opt/nn-backend/app}"
ENV_FILE="${ENV_FILE:-/etc/nn-backend.env}"
UNIT_FILE="${UNIT_FILE:-/etc/systemd/system/nn-backend.service}"

if [[ "$EUID" -ne 0 ]]; then
    echo "必须用 root 跑：sudo $0"
    exit 1
fi

# 1) 用户 + docker 组
if ! id "$APP_USER" >/dev/null 2>&1; then
    useradd --system --create-home --home-dir "/opt/${APP_USER}" \
        --shell /usr/sbin/nologin "$APP_USER"
fi
usermod -aG docker "$APP_USER" 2>/dev/null || true

# 2) 拷代码到 $APP_DIR
mkdir -p "$APP_DIR"
rsync -a --delete --exclude install.sh --exclude node_modules "$(dirname "$(readlink -f "$0")")"/ "$APP_DIR"/
chown -R "$APP_USER:$APP_USER" "$APP_DIR"

# 3) 装依赖（要求目标机能上外网）
pushd "$APP_DIR" >/dev/null
sudo -u "$APP_USER" npm ci --omit=dev
popd >/dev/null

# 4) 环境文件：已存在不覆盖，只给模板
if [[ ! -f "$ENV_FILE" ]]; then
    cat > "$ENV_FILE" <<EOF
NN_ENV=production
PORT=5174
BIND_HOST=127.0.0.1
NN_AUTH_TOKEN=$(openssl rand -hex 32 2>/dev/null || echo please-set-me)
ALLOWED_ORIGINS=http://www.netnexus.com.cn
MAX_INSTANCES=10
MAX_LINKS=30
IMAGE_ALLOWLIST=netnexus
CONTAINER_MEMORY=256m
CONTAINER_CPUS=0.5
EOF
    chmod 600 "$ENV_FILE"
    echo "[install] 已生成默认 $ENV_FILE（token 已自动生成，请记下来填到 nginx）"
else
    echo "[install] $ENV_FILE 已存在，保留不动"
fi

# 5) systemd unit
cat > "$UNIT_FILE" <<UNIT_EOF
[Unit]
Description=NetNexus Topology Backend
After=network-online.target docker.service
Requires=docker.service

[Service]
Type=simple
User=$APP_USER
Group=$APP_USER
WorkingDirectory=$APP_DIR
EnvironmentFile=$ENV_FILE
ExecStart=/usr/bin/node server.js
Restart=on-failure
RestartSec=3
ProtectSystem=strict
ReadWritePaths=/tmp /var/run/docker.sock $APP_DIR
ProtectHome=yes
NoNewPrivileges=yes

[Install]
WantedBy=multi-user.target
UNIT_EOF

systemctl daemon-reload
systemctl enable --now nn-backend
sleep 1
systemctl status nn-backend --no-pager -l | head -20

echo ""
echo "[install] 完成。"
echo "  查看日志：journalctl -u nn-backend -f"
echo "  token 位置：$ENV_FILE"
echo "  nginx 里把 X-NN-Token 和 ?token= 改成里面的值"
INSTALL_EOF
chmod +x "$PKG_DIR/install.sh"

# 3) 打包
mkdir -p "$HERE/dist"
if [[ -z "$OUTPUT" ]]; then
    OUTPUT="$HERE/dist/netnexus-backend-${VERSION}.tar.gz"
fi

tar -C "$STAGE" -czf "$OUTPUT" "netnexus-backend-${VERSION}"

echo ""
echo "[package] done"
echo "  产物：$OUTPUT"
echo "  大小：$(du -h "$OUTPUT" | cut -f1)"
echo ""
echo "部署："
echo "  scp $OUTPUT user@target:/tmp/"
echo "  ssh user@target 'cd /tmp && tar xzf $(basename "$OUTPUT") && cd netnexus-backend-${VERSION} && sudo ./install.sh'"
