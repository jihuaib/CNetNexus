/**
 * @file   ipc_subscribe.c
 * @brief  IPC 订阅 / 按需启动支持：subscribe / notify_ready / wait_module_ready
 *         + IO 线程上的 MODULE_EVENT 路由
 * @author jhb
 * @date   2026/05/21
 */

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dev.h"
#include "errcode.h"
#include "log.h"

/* ============================================================================
 * 订阅条目与管理器
 * ============================================================================ */

typedef struct ipc_subscription
{
    uint32_t target_module_id;
    dev_module_event_fn callback;
    void *user;
    uint32_t last_epoch; /**< 上次收到 READY 时的 epoch，0=尚未收到 */
    uint32_t down_epoch; /**< 最近一次接受的 DOWN epoch */
    int target_down;     /**< 已接受 DOWN，等待更高 epoch 的 READY */
    int auto_start;      /**< 初次 subscribe 时的 auto_start 标志，wait_all 中重订阅时复用 */
    char last_host[64];  /**< 上次见过的目标 host（订阅响应或 MODULE_EVENT），用于强制重连 */
    uint16_t last_port;  /**< 上次见过的目标 port */
} ipc_subscription_t;

struct dev_ipc_subscribe_mgr
{
    GHashTable *subs;     /**< target_module_id (GUINT) → ipc_subscription_t* */
    pthread_mutex_t lock; /**< subs 表锁 */
};

dev_ipc_subscribe_mgr_t *dev_ipc_subscribe_mgr_create(void)
{
    dev_ipc_subscribe_mgr_t *mgr = g_malloc0(sizeof(*mgr));
    mgr->subs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    pthread_mutex_init(&mgr->lock, NULL);
    return mgr;
}

void dev_ipc_subscribe_mgr_destroy(dev_ipc_subscribe_mgr_t *mgr)
{
    if (!mgr)
    {
        return;
    }
    pthread_mutex_lock(&mgr->lock);
    if (mgr->subs)
    {
        g_hash_table_destroy(mgr->subs);
        mgr->subs = NULL;
    }
    pthread_mutex_unlock(&mgr->lock);
    pthread_mutex_destroy(&mgr->lock);
    g_free(mgr);
}

/* 持锁查找订阅条目（不存在返回 NULL） */
static ipc_subscription_t *sub_mgr_find_locked(dev_ipc_subscribe_mgr_t *mgr, uint32_t target_id)
{
    return (ipc_subscription_t *)g_hash_table_lookup(mgr->subs, GUINT_TO_POINTER(target_id));
}

/* ============================================================================
 * MODULE_EVENT 路由（IO 线程上下文调用）
 *
 * 由 ipc_context.c 的 handle_frame 在收到 DEV_IPC_MSG_TYPE_DEV_MODULE_EVENT 时调用。
 * 本函数自动建联 / 关闭连接 + 触发业务回调。
 * 回调禁止阻塞或调用 dev_ipc_query。
 * ============================================================================ */

void dev_ipc_dispatch_module_event(dev_ipc_context_t *ctx, const dev_module_event_payload_t *pl)
{
    if (!ctx || !pl || !ctx->sub_mgr)
    {
        return;
    }

    uint32_t module_id = ntohl(pl->module_id);
    uint8_t event = pl->event;
    uint16_t port = ntohs(pl->port);
    uint32_t epoch = ntohl(pl->epoch);

    /* 先在 ctx 的单一线性化点按 epoch 应用事件。即使首次 SUBSCRIBE
     * 尚未插入本地条目，DOWN tombstone 也必须保留，阻止同 epoch 的
     * 迟到合成 READY 重新打开重连。 */
    int available = event == DEV_MODULE_EVENT_READY;
    if (!dev_ipc_apply_target_event(ctx, module_id, epoch, available))
    {
        LOG_DEBUG("<%s> Drop stale MODULE_EVENT for 0x%08X (event=%u epoch=%u)", ctx->name, module_id, event, epoch);
        return;
    }

    /* 拷贝订阅条目的回调/user，避免持锁期间触发业务回调导致死锁 */
    dev_module_event_fn cb = NULL;
    void *user = NULL;
    uint32_t prev_epoch = 0;
    int stale_event = 0;

    pthread_mutex_lock(&ctx->sub_mgr->lock);
    ipc_subscription_t *sub = sub_mgr_find_locked(ctx->sub_mgr, module_id);
    if (sub)
    {
        cb = sub->callback;
        user = sub->user;
        prev_epoch = sub->last_epoch;
        if (event == DEV_MODULE_EVENT_READY)
        {
            /* 同 epoch 的 DOWN 权威性更高：拒绝 SUBSCRIBE 响应线程迟到合成的
             * READY，避免刚停止的按需模块重新进入永久重连。 */
            if ((sub->target_down && epoch <= sub->down_epoch) || (sub->last_epoch != 0 && epoch < sub->last_epoch))
            {
                stale_event = 1;
            }
            else
            {
                sub->target_down = 0;
                sub->last_epoch = epoch;
                /* 缓存最新 host/port：之后 wait_all 卡住时可拿来强制重连 */
                if (pl->host[0] != '\0' && port != 0)
                {
                    snprintf(sub->last_host, sizeof(sub->last_host), "%s", pl->host);
                    sub->last_port = port;
                }
            }
        }
        else if (event == DEV_MODULE_EVENT_DOWN)
        {
            if (sub->last_epoch != 0 && epoch < sub->last_epoch)
            {
                stale_event = 1;
            }
            else
            {
                sub->target_down = 1;
                sub->down_epoch = epoch;
            }
        }
    }
    pthread_mutex_unlock(&ctx->sub_mgr->lock);

    if (!sub)
    {
        LOG_DEBUG("<%s> Drop MODULE_EVENT for 0x%08X (not subscribed)", ctx->name, module_id);
        return;
    }

    if (stale_event)
    {
        LOG_DEBUG("<%s> Drop stale MODULE_EVENT for 0x%08X (event=%u epoch=%u last_ready=%u)", ctx->name, module_id,
                  event, epoch, prev_epoch);
        return;
    }

    if (event == DEV_MODULE_EVENT_READY)
    {
        /* 自动建联：若已连接到旧 epoch 的进程，需要先关闭再重连 */
        if (prev_epoch != 0 && prev_epoch != epoch)
        {
            LOG_INFO("<%s> Target 0x%08X restarted (epoch %u→%u), reconnecting", ctx->name, module_id, prev_epoch,
                     epoch);
        }
        /* dev_ipc_connect 内部对已有连接幂等返回。重启场景下旧连接会被对端断开，
         * IPC 库自带重连机制会重新建联到新进程。 */
        if (pl->host[0] != '\0' && port != 0)
        {
            dev_ipc_connect(ctx, module_id, pl->host, port);
        }
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        /* 不立即关闭 socket：BGP/ISIS 等 self-exit 模块可能刚写出 RESP_EXITING。
         * 只暂停主动重连，等 EOF 正常回收；下一个更高 epoch READY 再恢复。 */
    }

    if (cb)
    {
        cb(module_id, event, pl->host, port, epoch, user);
    }
}

/* ============================================================================
 * 公共 API：subscribe / unsubscribe / notify_ready
 * ============================================================================ */

int dev_ipc_subscribe_module(dev_ipc_context_t *ctx, uint32_t target_id, int auto_start, dev_module_event_fn cb,
                             void *user)
{
    if (!ctx || target_id == 0)
    {
        return ERRCODE_FAIL;
    }
    if (!ctx->sub_mgr)
    {
        LOG_ERROR("<%s> subscribe: sub_mgr not initialized", ctx->name);
        return ERRCODE_FAIL;
    }

    /* 1. 向 DEV 发送 SUBSCRIBE RPC（冷启动期间 DEV 可能因为忙于建表/批量握手
     *    导致首次请求被丢弃，加 short-timeout 重试，最多 3 次。
     *    本地 sub_mgr 条目延迟到 RPC 成功后再插入：若 RPC 失败就丢，避免
     *    wait_all_subscribed_connected 等一个 DEV 不会广播的 peer 永久卡住。 */
    dev_subscribe_req_t req;
    memset(&req, 0, sizeof(req));
    req.target_module_id = htonl(target_id);
    req.auto_start = auto_start ? 1 : 0;

    dev_ipc_message_t *resp = NULL;
    const int max_attempts = 3;
    const uint32_t per_attempt_timeout_ms = DEV_IPC_SUBSCRIBE_RPC_MS;
    for (int attempt = 1; attempt <= max_attempts; attempt++)
    {
        dev_ipc_message_t *msg = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_SUBSCRIBE_MODULE, ctx->module_id,
                                                        DEV_MODULE_ID_DEV, 0, &req, sizeof(req), NULL);
        if (!msg)
        {
            return ERRCODE_FAIL;
        }
        resp = dev_ipc_query(ctx, DEV_MODULE_ID_DEV, msg, per_attempt_timeout_ms);
        dev_ipc_message_free(msg);
        if (resp)
        {
            break;
        }
        LOG_WARN("<%s> SUBSCRIBE(0x%08X) attempt %d/%d timed out, retrying", ctx->name, target_id, attempt,
                 max_attempts);
    }
    if (!resp)
    {
        LOG_WARN("<%s> SUBSCRIBE(0x%08X) RPC timeout after %d attempts", ctx->name, target_id, max_attempts);
        return ERRCODE_FAIL;
    }

    int ret = ERRCODE_SUCCESS;
    if (resp->payload && resp->payload_len >= sizeof(dev_subscribe_resp_t))
    {
        const dev_subscribe_resp_t *r = (const dev_subscribe_resp_t *)resp->payload;
        int32_t result = (int32_t)ntohl((uint32_t)r->result);
        uint8_t state = r->current_state;
        uint16_t port = ntohs(r->port);
        uint32_t epoch = ntohl(r->epoch);

        if (result != 0)
        {
            LOG_WARN("<%s> SUBSCRIBE(0x%08X) DEV refused (result=%d)", ctx->name, target_id, result);
            ret = ERRCODE_FAIL;
        }
        else
        {
            /* DEV 接受了订阅：现在才把条目写进本地 sub_mgr。
             * 之后才能合成事件 / dispatch（dispatch 要靠 sub_mgr 找回调）。
             * 同时缓存 auto_start 和 host/port，供 wait_all 卡住时强制重连。 */
            pthread_mutex_lock(&ctx->sub_mgr->lock);
            ipc_subscription_t *sub = sub_mgr_find_locked(ctx->sub_mgr, target_id);
            if (!sub)
            {
                sub = g_malloc0(sizeof(*sub));
                sub->target_module_id = target_id;
                g_hash_table_insert(ctx->sub_mgr->subs, GUINT_TO_POINTER(target_id), sub);
            }
            sub->callback = cb;
            sub->user = user;
            sub->auto_start = auto_start ? 1 : 0;
            if (r->host[0] != '\0' && port != 0)
            {
                snprintf(sub->last_host, sizeof(sub->last_host), "%s", r->host);
                sub->last_port = port;
            }
            pthread_mutex_unlock(&ctx->sub_mgr->lock);

            if (state == DEV_MODULE_STATE_READY)
            {
                /* 已就绪：在订阅响应中直接拿到 host/port，立即合成一次 READY 事件
                 * （走和 IO 线程同样的派发逻辑，自动建联 + 回调） */
                dev_module_event_payload_t synth;
                memset(&synth, 0, sizeof(synth));
                synth.module_id = htonl(target_id);
                synth.event = DEV_MODULE_EVENT_READY;
                snprintf(synth.host, sizeof(synth.host), "%s", r->host);
                synth.port = htons(port);
                synth.epoch = htonl(epoch);
                dev_ipc_dispatch_module_event(ctx, &synth);
            }
            else if (state == DEV_MODULE_STATE_STARTING && port != 0)
            {
                /* 目标已经 fork 且 IPC listener 起来了，但还没 notify_ready：
                 * 立即把 TCP 通道建好，避免互订阅模块（IF↔ROUTE）各自卡在 wait_all 的循环死锁。
                 * 业务 READY 回调仍等真正的 MODULE_EVENT_READY 异步推送，语义不变。 */
                (void)dev_ipc_connect(ctx, target_id, r->host, port);
            }
            /* NOT_RUNNING：等异步 MODULE_EVENT */
        }
    }
    dev_ipc_message_free(resp);

    return ret;
}

char *dev_ipc_format_local_subs(dev_ipc_context_t *ctx, uint32_t *out_len)
{
    if (!ctx || !ctx->sub_mgr)
    {
        if (out_len)
        {
            *out_len = 0;
        }
        return NULL;
    }

    GString *buf = g_string_new("");
    pthread_mutex_lock(&ctx->sub_mgr->lock);
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&iter, ctx->sub_mgr->subs);
    int count = 0;
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        ipc_subscription_t *sub = (ipc_subscription_t *)val;
        if (!sub)
        {
            continue;
        }
        int connected = dev_ipc_is_connected(ctx, sub->target_module_id) ? 1 : 0;
        g_string_append_printf(buf, "  target=0x%08X auto_start=%d last_epoch=%u last_host=%s last_port=%u conn=%s\n",
                               sub->target_module_id, sub->auto_start, sub->last_epoch,
                               sub->last_host[0] ? sub->last_host : "-", sub->last_port, connected ? "up" : "down");
        count++;
    }
    pthread_mutex_unlock(&ctx->sub_mgr->lock);

    if (count == 0)
    {
        g_string_append(buf, "  (no subscriptions)\n");
    }

    uint32_t len = (uint32_t)buf->len + 1; /* 含 NUL */
    char *result = g_string_free(buf, FALSE);
    if (out_len)
    {
        *out_len = len;
    }
    return result;
}

int dev_ipc_unsubscribe_module(dev_ipc_context_t *ctx, uint32_t target_id)
{
    if (!ctx || target_id == 0 || !ctx->sub_mgr)
    {
        return ERRCODE_FAIL;
    }

    pthread_mutex_lock(&ctx->sub_mgr->lock);
    g_hash_table_remove(ctx->sub_mgr->subs, GUINT_TO_POINTER(target_id));
    pthread_mutex_unlock(&ctx->sub_mgr->lock);

    /* 通知 DEV（单向，失败不影响本地状态） */
    uint32_t id_be = htonl(target_id);
    dev_ipc_message_t *msg = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_UNSUBSCRIBE_MODULE, ctx->module_id,
                                                    DEV_MODULE_ID_DEV, 0, &id_be, sizeof(id_be), NULL);
    if (msg)
    {
        dev_ipc_send(ctx, DEV_MODULE_ID_DEV, msg);
        dev_ipc_message_free(msg);
    }

    return ERRCODE_SUCCESS;
}

int dev_ipc_notify_ready(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    /* 单向通知（无 payload）；DEV 收到后会广播 MODULE_EVENT 给订阅者 */
    dev_ipc_message_t *msg =
        dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_NOTIFY_READY, ctx->module_id, DEV_MODULE_ID_DEV, 0, NULL, 0, NULL);
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_DEV, msg);
    dev_ipc_message_free(msg);

    if (rc != ERRCODE_SUCCESS)
    {
        /* DEV 还没连上(初始化竞争):置标志,IO 线程在 DEV 握手完成时补发。
         * 对调用方返回 SUCCESS,让 init 继续向下走;DEV 视角的 READY 由补发补齐。 */
        ctx->pending_notify_ready = 1;
        LOG_INFO("<%s> notify_ready deferred: DEV not connected yet, will flush on handshake", ctx->name);
        return ERRCODE_SUCCESS;
    }
    ctx->pending_notify_ready = 0;
    LOG_INFO("<%s> Notified DEV: module ready", ctx->name);
    return ERRCODE_SUCCESS;
}

/* ============================================================================
 * pre_exit_notify：模块退出前 RPC 通知 DEV，等 ACK 后再 exit
 * ============================================================================ */

int dev_ipc_pre_exit_notify(dev_ipc_context_t *ctx, uint32_t timeout_ms)
{
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    if (timeout_ms == 0)
    {
        timeout_ms = DEV_IPC_QUERY_TIMEOUT_DEFAULT;
    }

    dev_ipc_message_t *msg =
        dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_PRE_EXIT, ctx->module_id, DEV_MODULE_ID_DEV, 0, NULL, 0, NULL);
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_DEV, msg, timeout_ms);
    dev_ipc_message_free(msg);

    if (!resp)
    {
        LOG_WARN("<%s> PRE_EXIT RPC timed out after %u ms; SIGCHLD path will clean up", ctx->name, timeout_ms);
        return ERRCODE_FAIL;
    }

    dev_ipc_message_free(resp);
    LOG_INFO("<%s> PRE_EXIT ack received from DEV", ctx->name);
    return ERRCODE_SUCCESS;
}

/* ============================================================================
 * wait_connected：轮询等待已发起的连接握手完成
 * ============================================================================ */

int dev_ipc_wait_connected(dev_ipc_context_t *ctx, uint32_t target_id, uint32_t timeout_ms)
{
    if (!ctx || target_id == 0)
    {
        return ERRCODE_FAIL;
    }
    if (timeout_ms == 0)
    {
        timeout_ms = DEV_IPC_QUERY_TIMEOUT_DEFAULT;
    }

    gint64 deadline_us = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    while (g_get_monotonic_time() < deadline_us)
    {
        if (dev_ipc_is_connected(ctx, target_id))
        {
            return ERRCODE_SUCCESS;
        }
        usleep(50 * 1000);
    }
    return ERRCODE_FAIL;
}

/* ============================================================================
 * wait_module_ready：触发 DEV 拉起 + 轮询本端连接
 *
 * 设计前提：业务模块（如 SBMP）在自身 init 中会 subscribe(CLI)，主动向 CFG 建联。
 * 因此 CFG 不需要监听 READY 事件——只要轮询 is_connected(target) 即可知 target 准备好了。
 * ============================================================================ */

/* 查 target 在 DEV 那里的当前 phase（DEV_MODULE_STATE_*）。
 * 复用 SUBSCRIBE RPC（DEV 端幂等），auto_start=0 不触发拉起；返回值即响应里的 state。
 * synth 事件会在 state=READY 时被发起（自动 connect 到 target）。 */
static int query_target_state(dev_ipc_context_t *ctx, uint32_t target_id, int auto_start, uint8_t *state_out)
{
    if (!ctx || target_id == 0 || !state_out)
    {
        return ERRCODE_FAIL;
    }

    /* 复用 dev_ipc_subscribe_module —— 它已有重试、synth READY 后 auto-connect */
    int rc = dev_ipc_subscribe_module(ctx, target_id, auto_start, NULL, NULL);
    if (rc != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    /* subscribe 成功后，sub_mgr 已记录订阅；DEV 端 phase 由后续异步 MODULE_EVENT 推送。
     * 但我们这里要的是"当前 phase"，直接再发一次 subscribe RPC 拿响应 state。
     * 为避免循环 auto-connect，第二次显式构造 RPC 不走 lib 的 dispatch。 */
    dev_subscribe_req_t req;
    memset(&req, 0, sizeof(req));
    req.target_module_id = htonl(target_id);
    req.auto_start = 0;
    dev_ipc_message_t *msg = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_SUBSCRIBE_MODULE, ctx->module_id,
                                                    DEV_MODULE_ID_DEV, 0, &req, sizeof(req), NULL);
    if (!msg)
    {
        return ERRCODE_FAIL;
    }
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_DEV, msg, DEV_IPC_SUBSCRIBE_RPC_MS);
    dev_ipc_message_free(msg);
    if (!resp || !resp->payload || resp->payload_len < sizeof(dev_subscribe_resp_t))
    {
        if (resp)
        {
            dev_ipc_message_free(resp);
        }
        return ERRCODE_FAIL;
    }
    const dev_subscribe_resp_t *r = (const dev_subscribe_resp_t *)resp->payload;
    *state_out = r->current_state;
    dev_ipc_message_free(resp);
    return ERRCODE_SUCCESS;
}

int dev_ipc_wait_module_ready_with_progress(dev_ipc_context_t *ctx, uint32_t target_id, uint32_t timeout_ms,
                                            dev_ipc_wait_progress_fn progress_cb, void *user)
{
    if (!ctx || target_id == 0)
    {
        return ERRCODE_FAIL;
    }
    if (timeout_ms == 0)
    {
        timeout_ms = DEV_IPC_QUERY_TIMEOUT_DEFAULT;
    }

    /* 轮询 target 在 DEV 那里的 phase；只接受 READY（DB restore 完成 + 已 notify_ready）。
     * 不再依赖 is_connected——业务模块的 subscribe(CLI) 顺序与 READY 解耦：
     * READY 才意味着模块业务真正可用，CFG 此时派发 config 才安全。
     *
     * 第一次轮询 auto_start=1 触发按需拉起；之后每次 200ms 复查直到 READY 或超时。 */
    int64_t start_us = (int64_t)g_get_monotonic_time();
    int64_t deadline_us = start_us + (int64_t)timeout_ms * 1000;
    int64_t next_progress_us = start_us;
    int triggered_autostart = 0;
    uint8_t last_state = DEV_MODULE_STATE_NOT_RUNNING;
    uint8_t last_reported_state = 0xFF;
    while (1)
    {
        int64_t now_us = (int64_t)g_get_monotonic_time();
        uint8_t state = DEV_MODULE_STATE_NOT_RUNNING;
        if (query_target_state(ctx, target_id, triggered_autostart ? 0 : 1, &state) == ERRCODE_SUCCESS)
        {
            now_us = (int64_t)g_get_monotonic_time();
            last_state = state;
            triggered_autostart = 1;
            if (progress_cb &&
                (state == DEV_MODULE_STATE_READY || state != last_reported_state || now_us >= next_progress_us))
            {
                int64_t elapsed_us = now_us - start_us;
                uint32_t elapsed_ms = 0;
                if (elapsed_us > 0)
                {
                    elapsed_ms = (elapsed_us / 1000 > UINT32_MAX) ? UINT32_MAX : (uint32_t)(elapsed_us / 1000);
                }
                progress_cb(target_id, state, elapsed_ms, user);
                last_reported_state = state;
                next_progress_us = now_us + 1000 * 1000;
            }
            if (state == DEV_MODULE_STATE_READY)
            {
                return ERRCODE_SUCCESS;
            }
        }
        if ((int64_t)g_get_monotonic_time() >= deadline_us)
        {
            LOG_WARN("<%s> wait_module_ready(0x%08X) timeout (last state=%u)", ctx->name, target_id, last_state);
            return ERRCODE_FAIL;
        }
        usleep(200 * 1000);
    }
}

int dev_ipc_wait_module_ready(dev_ipc_context_t *ctx, uint32_t target_id, uint32_t timeout_ms)
{
    return dev_ipc_wait_module_ready_with_progress(ctx, target_id, timeout_ms, NULL, NULL);
}

/* ============================================================================
 * dev_ipc_wait_all_subscribed_connected：DEPS_READY 阶段判定
 *
 * 业务模块自己订阅了哪些 peer 都记录在 ctx->sub_mgr 里。此函数遍历这张表，
 * 等到每个订阅 target 的 IPC 都进入 CONNECTED 状态后返回。
 * 用于业务模块 init 时在 db_restore 之前确认依赖通道齐备，再 notify_ready。
 * 内部 100ms 轮询；调用方在自身 init 主线程上调用，不阻塞 IPC worker。
 * ============================================================================ */

int dev_ipc_wait_all_subscribed_connected(dev_ipc_context_t *ctx, uint32_t timeout_ms)
{
    if (!ctx || !ctx->sub_mgr)
    {
        return ERRCODE_FAIL;
    }

    /* timeout_ms == 0 → 无超时（推荐用法）：阻塞直到全部订阅 peer CONNECTED。
     * 业务模块只有真正全连上才该宣告 READY，否则会出现 DEV 视角 READY 但 CFG 端发不出
     * 命令的 race（CFG 的 dev_ipc_query 找不到 conn）。
     *
     * timeout_ms != 0 → 老语义保留：超时返回 FAIL，由调用方决定怎么办。 */
    const uint32_t poll_step_us = 100 * 1000; /* 100ms */
    const int log_every_iters = 30;           /* ~3s 一条 progress 日志 */
    const int resubscribe_every_iters = 100;  /* ~10s 强制一次 re-subscribe + 重连，
                                               *   恢复 stuck-SYN / 丢失 MODULE_EVENT 等异常 */
    int64_t deadline_us = (timeout_ms == 0) ? 0 : (int64_t)g_get_monotonic_time() + (int64_t)timeout_ms * 1000;
    int iter_count = 0;

    while (1)
    {
        /* 快照本端的订阅 target id 列表，避免持锁期间调 is_connected（is_connected 自己也加锁） */
        GArray *targets = g_array_new(FALSE, FALSE, sizeof(uint32_t));
        pthread_mutex_lock(&ctx->sub_mgr->lock);
        GHashTableIter iter;
        gpointer key = NULL;
        gpointer val = NULL;
        g_hash_table_iter_init(&iter, ctx->sub_mgr->subs);
        while (g_hash_table_iter_next(&iter, &key, &val))
        {
            ipc_subscription_t *sub = (ipc_subscription_t *)val;
            if (sub)
            {
                g_array_append_val(targets, sub->target_module_id);
            }
        }
        pthread_mutex_unlock(&ctx->sub_mgr->lock);

        guint missing = 0;
        GString *missing_list = NULL;
        gboolean want_log = (iter_count > 0) && (iter_count % log_every_iters == 0);
        GArray *stuck = g_array_new(FALSE, FALSE, sizeof(uint32_t));
        for (guint i = 0; i < targets->len; i++)
        {
            uint32_t tid = g_array_index(targets, uint32_t, i);
            if (!dev_ipc_is_connected(ctx, tid))
            {
                missing++;
                if (want_log)
                {
                    if (!missing_list)
                    {
                        missing_list = g_string_new("");
                    }
                    else
                    {
                        g_string_append_c(missing_list, ' ');
                    }
                    g_string_append_printf(missing_list, "0x%08X", tid);
                }
                g_array_append_val(stuck, tid);
            }
        }
        g_array_free(targets, TRUE);

        if (missing == 0)
        {
            if (missing_list)
            {
                g_string_free(missing_list, TRUE);
            }
            g_array_free(stuck, TRUE);
            return ERRCODE_SUCCESS;
        }

        if (want_log)
        {
            LOG_WARN("<%s> wait_all_subscribed_connected: still waiting %u peer(s): %s", ctx->name, missing,
                     missing_list ? missing_list->str : "?");
        }
        if (missing_list)
        {
            g_string_free(missing_list, TRUE);
        }

        /* 周期性强制恢复：tear down 残连 + 重发 SUBSCRIBE，向 DEV 索取最新 state/port/epoch。
         * 处理几类不靠 IO 线程自愈的卡死：
         *   a. SYN 卡在 EINPROGRESS（kernel 没 RST），check_pending_connects 兜底之后还是连到同一陈旧 port
         *   b. 目标进程在我们订阅后被重启，新 epoch+新 port，但 DEV 推 MODULE_EVENT 那一刻投递失败
         *   c. 首次 SUBSCRIBE 时目标 NOT_RUNNING，后续 READY 广播因为 IPC 抖动丢了 */
        if (iter_count > 0 && iter_count % resubscribe_every_iters == 0 && stuck->len > 0)
        {
            for (guint i = 0; i < stuck->len; i++)
            {
                uint32_t tid = g_array_index(stuck, uint32_t, i);
                dev_module_event_fn cb = NULL;
                void *user_ptr = NULL;
                int auto_start_cached = 0;
                pthread_mutex_lock(&ctx->sub_mgr->lock);
                ipc_subscription_t *sub = sub_mgr_find_locked(ctx->sub_mgr, tid);
                if (sub)
                {
                    cb = sub->callback;
                    user_ptr = sub->user;
                    auto_start_cached = sub->auto_start;
                }
                pthread_mutex_unlock(&ctx->sub_mgr->lock);
                if (!sub)
                {
                    continue;
                }
                LOG_WARN("<%s> wait_all_subscribed_connected: forcing resubscribe for 0x%08X", ctx->name, tid);
                /* 拆掉残连（idempotent；若没有 conn 则 no-op） */
                dev_ipc_drop_connection(ctx, tid);
                /* 重发 SUBSCRIBE：里头会按响应里的最新 state 再触发 dispatch / connect */
                (void)dev_ipc_subscribe_module(ctx, tid, auto_start_cached, cb, user_ptr);
            }
        }
        g_array_free(stuck, TRUE);

        if (timeout_ms != 0 && (int64_t)g_get_monotonic_time() >= deadline_us)
        {
            LOG_WARN("<%s> wait_all_subscribed_connected: %u target(s) still not connected (timeout)", ctx->name,
                     missing);
            return ERRCODE_FAIL;
        }
        usleep(poll_step_us);
        iter_count++;
    }
}
