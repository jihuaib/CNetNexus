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

    /* 拷贝订阅条目的回调/user，避免持锁期间触发业务回调导致死锁 */
    dev_module_event_fn cb = NULL;
    void *user = NULL;
    uint32_t prev_epoch = 0;

    pthread_mutex_lock(&ctx->sub_mgr->lock);
    ipc_subscription_t *sub = sub_mgr_find_locked(ctx->sub_mgr, module_id);
    if (sub)
    {
        cb = sub->callback;
        user = sub->user;
        prev_epoch = sub->last_epoch;
        if (event == DEV_MODULE_EVENT_READY)
        {
            sub->last_epoch = epoch;
        }
    }
    pthread_mutex_unlock(&ctx->sub_mgr->lock);

    if (!sub)
    {
        LOG_DEBUG("<%s> Drop MODULE_EVENT for 0x%08X (not subscribed)", ctx->name, module_id);
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
    /* DOWN 事件：连接由 IPC 库的心跳/断连机制自动清理，无需在此干预 */

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

    /* 1. 先把订阅条目记录到本地（即使 RPC 失败也保留，IO 线程后续重连/事件还能用） */
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
    pthread_mutex_unlock(&ctx->sub_mgr->lock);

    /* 2. 向 DEV 发送 SUBSCRIBE RPC */
    dev_subscribe_req_t req;
    memset(&req, 0, sizeof(req));
    req.target_module_id = htonl(target_id);
    req.auto_start = auto_start ? 1 : 0;

    dev_ipc_message_t *msg = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_SUBSCRIBE_MODULE, ctx->module_id,
                                                    DEV_MODULE_ID_DEV, 0, &req, sizeof(req), NULL);
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_DEV, msg, 0);
    dev_ipc_message_free(msg);

    if (!resp)
    {
        LOG_WARN("<%s> SUBSCRIBE(0x%08X) RPC timeout", ctx->name, target_id);
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
        else if (state == DEV_MODULE_STATE_READY)
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
        /* STARTING / NOT_RUNNING：等异步 MODULE_EVENT */
    }
    dev_ipc_message_free(resp);

    return ret;
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
        LOG_WARN("<%s> notify_ready: failed to send to DEV", ctx->name);
        return ERRCODE_FAIL;
    }
    LOG_INFO("<%s> Notified DEV: module ready", ctx->name);
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

int dev_ipc_wait_module_ready(dev_ipc_context_t *ctx, uint32_t target_id, uint32_t timeout_ms)
{
    if (!ctx || target_id == 0)
    {
        return ERRCODE_FAIL;
    }
    if (timeout_ms == 0)
    {
        timeout_ms = DEV_IPC_QUERY_TIMEOUT_DEFAULT;
    }

    /* 快路径：已连接直接返回 */
    if (dev_ipc_is_connected(ctx, target_id))
    {
        return ERRCODE_SUCCESS;
    }

    /* 触发 DEV 把按需 target fork 出来（已在跑则 no-op；非按需且未跑则失败）。
     * 不注册回调——目标 init 完成时会主动 subscribe(CLI) 反向连接，本端被动收 inbound。 */
    int rc = dev_ipc_subscribe_module(ctx, target_id, 1, NULL, NULL);
    if (rc != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    /* 轮询等 target 反向 connect 进来（target init 中 subscribe(CLI) → 自动 connect → CFG accept） */
    return dev_ipc_wait_connected(ctx, target_id, timeout_ms);
}
