#!/bin/bash
#
# NetNexus DEV 进程监护脚本：在循环里启动 netnexus，DEV 异常退出后按 backoff 自动拉起。
#
# 语义跟 src/main.c 里"模块崩溃自愈"分支对齐：
#   - 窗口 NN_SUPERVISE_WINDOW_SEC （默认 120s）内累计 crash_count
#   - <= NN_SUPERVISE_MAX_RETRIES （默认 10 次）：sleep NN_SUPERVISE_BACKOFF_SEC 再起
#   - 超过上限：打 FATAL 日志后退出，让外层（systemd / docker --restart / 人工）兜底
#
# 调用方：
#   - gns3-entry.sh 在后台调用本脚本
#   - CI top_runner._start_netnexus_process 用 `bash supervise.sh` 替代 `exec netnexus`
#
# 假设：
#   - $NN_WORK_DIR 已 export，指向 /opt/netnexus 之类
#   - netnexus 二进制在 ${NN_WORK_DIR}/bin/netnexus
#   - $LD_LIBRARY_PATH / $ASAN_OPTIONS 等已由调用方 export

set -u

NN_WORK_DIR="${NN_WORK_DIR:-/opt/netnexus}"
NN_BINARY="${NN_BINARY:-${NN_WORK_DIR}/bin/netnexus}"
NN_LOG_DIR="${NN_LOG_DIR:-${NN_WORK_DIR}/log}"

NN_SUPERVISE_WINDOW_SEC="${NN_SUPERVISE_WINDOW_SEC:-120}"
NN_SUPERVISE_MAX_RETRIES="${NN_SUPERVISE_MAX_RETRIES:-10}"
NN_SUPERVISE_BACKOFF_SEC="${NN_SUPERVISE_BACKOFF_SEC:-3}"

# 跟踪当前 child pid，便于 SIGTERM/SIGINT 时优雅停掉 netnexus 再退出，
# 避免 wrapper 死掉之后 child 变成 1 号进程托管的孤儿
CHILD_PID=0
_propagate_signal() {
    local sig="$1"
    if [ "$CHILD_PID" -gt 0 ] 2>/dev/null; then
        kill "-${sig}" "$CHILD_PID" 2>/dev/null || true
        # 等 child 自行 graceful exit；超时后 wrapper 也直接退
        wait "$CHILD_PID" 2>/dev/null || true
    fi
    exit 0
}
trap '_propagate_signal TERM' TERM
trap '_propagate_signal INT' INT

mkdir -p "$NN_LOG_DIR" 2>/dev/null || true
SUP_LOG="${NN_LOG_DIR}/supervise.log"

if [ ! -x "$NN_BINARY" ]; then
    echo "[$(date '+%F %T')] [FATAL] supervise: binary not found or not executable: $NN_BINARY" \
        | tee -a "$SUP_LOG" >&2
    exit 1
fi

crash_count=0
last_crash_time=0

while true; do
    echo "[$(date '+%F %T')] [INFO] supervise: starting netnexus (attempt $((crash_count + 1)))" \
        >> "$SUP_LOG"

    "$NN_BINARY" </dev/null &
    CHILD_PID=$!
    wait "$CHILD_PID"
    rc=$?
    CHILD_PID=0

    now=$(date +%s)
    if [ "$((now - last_crash_time))" -gt "$NN_SUPERVISE_WINDOW_SEC" ]; then
        crash_count=0
    fi
    last_crash_time="$now"
    crash_count=$((crash_count + 1))

    echo "[$(date '+%F %T')] [WARN] supervise: netnexus exited rc=$rc (count=$crash_count/$NN_SUPERVISE_MAX_RETRIES)" \
        >> "$SUP_LOG"

    if [ "$crash_count" -gt "$NN_SUPERVISE_MAX_RETRIES" ]; then
        echo "[$(date '+%F %T')] [FATAL] supervise: netnexus crashed $crash_count times within ${NN_SUPERVISE_WINDOW_SEC}s; giving up" \
            | tee -a "$SUP_LOG" >&2
        echo "[$(date '+%F %T')] [FATAL] supervise: check ${NN_LOG_DIR}/asan/ and core dumps" \
            | tee -a "$SUP_LOG" >&2
        exit 1
    fi

    sleep "$NN_SUPERVISE_BACKOFF_SEC"
done
