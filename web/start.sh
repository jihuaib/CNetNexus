#!/usr/bin/env bash
# 一键安装并启动 NetNexus 拓扑编排 Web UI
# 用法:
#   ./web/start.sh            # 监听 5173 (前端) + 5174 (后端)
#   USE_SUDO=1 ./web/start.sh # docker 命令通过 sudo 执行
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if ! command -v node >/dev/null 2>&1; then
    echo "[err] 未检测到 node，请先安装 Node.js 18+ (https://nodejs.org/)"
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "[warn] 未检测到 docker；可以打开界面但启动设备会失败"
fi

echo "[1/3] 安装后端依赖..."
(cd backend && npm install --silent)

echo "[2/3] 安装前端依赖..."
(cd frontend && npm install --silent)

echo "[3/3] 启动后端 (http://localhost:5174) 与前端 (http://localhost:5173)"

cleanup() {
    echo
    echo "[bye] 关闭服务..."
    [[ -n "${BACK_PID:-}" ]] && kill "$BACK_PID" 2>/dev/null || true
    [[ -n "${FRONT_PID:-}" ]] && kill "$FRONT_PID" 2>/dev/null || true
    exit 0
}
trap cleanup INT TERM

(cd backend && USE_SUDO="${USE_SUDO:-0}" PORT=5174 npm start) &
BACK_PID=$!

sleep 1

(cd frontend && BACKEND_URL=http://localhost:5174 npm run dev -- --host) &
FRONT_PID=$!

wait
