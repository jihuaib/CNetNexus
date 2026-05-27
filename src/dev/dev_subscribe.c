/**
 * @file   dev_subscribe.c
 * @brief  DEV 侧订阅/按需启动消息处理：SUBSCRIBE / UNSUBSCRIBE / NOTIFY_READY
 *         + 模块就绪时向订阅者推送 MODULE_EVENT
 * @author jhb
 * @date   2026/05/21
 */

#include "dev_subscribe.h"

#include <arpa/inet.h>
#include <glib.h>
#include <string.h>

#include "dev.h"
#include "dev_main.h"
#include "dev_module.h"
#include "errcode.h"
#include "log.h"

/* ============================================================================
 * 订阅者列表维护（dev_module_t.subscribers）
 * ============================================================================ */

/* 检查订阅者列表中是否已包含某 module_id（避免重复） */
static gboolean subscriber_list_contains(GList *list, uint32_t subscriber_id)
{
    for (GList *l = list; l != NULL; l = l->next)
    {
        if (GPOINTER_TO_UINT(l->data) == subscriber_id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void subscriber_list_add(dev_module_t *target, uint32_t subscriber_id)
{
    if (subscriber_list_contains(target->subscribers, subscriber_id))
    {
        return;
    }
    target->subscribers = g_list_append(target->subscribers, GUINT_TO_POINTER(subscriber_id));
}

static void subscriber_list_remove(dev_module_t *target, uint32_t subscriber_id)
{
    target->subscribers = g_list_remove(target->subscribers, GUINT_TO_POINTER(subscriber_id));
}

/* ============================================================================
 * 推送 MODULE_EVENT 给订阅者
 * ============================================================================ */

static void push_module_event_to(uint32_t subscriber_id, const dev_module_t *target, uint8_t event)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    dev_module_event_payload_t pl;
    memset(&pl, 0, sizeof(pl));
    pl.module_id = htonl(target->module_id);
    pl.event = event;
    snprintf(pl.host, sizeof(pl.host), "%s", DEV_IPC_HOST_LOCAL);
    pl.port = htons(target->port);
    pl.epoch = htonl(target->epoch);

    dev_ipc_message_t *msg = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_EVENT, DEV_MODULE_ID_DEV, subscriber_id,
                                                    0, &pl, sizeof(pl), NULL);
    if (!msg)
    {
        return;
    }

    /* 单向通知，无响应 */
    int rc = dev_ipc_send(ctx, subscriber_id, msg);
    dev_ipc_message_free(msg);

    if (rc != ERRCODE_SUCCESS)
    {
        LOG_WARN("Failed to push MODULE_EVENT(%u) for module 0x%08X to subscriber 0x%08X", event, target->module_id,
                 subscriber_id);
    }
}

void dev_subscribe_broadcast_event(dev_module_t *target, uint8_t event)
{
    if (!target)
    {
        return;
    }
    for (GList *l = target->subscribers; l != NULL; l = l->next)
    {
        uint32_t sub_id = GPOINTER_TO_UINT(l->data);
        push_module_event_to(sub_id, target, event);
    }
}

/* ============================================================================
 * 消息处理：SUBSCRIBE
 * ============================================================================ */

static void send_subscribe_resp(dev_ipc_message_t *req, int32_t result, uint8_t state, uint16_t port, uint32_t epoch)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    dev_subscribe_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.result = (int32_t)htonl((uint32_t)result);
    resp.current_state = state;
    snprintf(resp.host, sizeof(resp.host), "%s", DEV_IPC_HOST_LOCAL);
    resp.port = htons(port);
    resp.epoch = htonl(epoch);

    dev_ipc_message_t *m = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_DEV,
                                                  req->src_module_id, req->request_id, &resp, sizeof(resp), NULL);
    if (m)
    {
        dev_ipc_send_response(ctx, m);
        dev_ipc_message_free(m);
    }
}

void dev_subscribe_handle_subscribe(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(dev_subscribe_req_t))
    {
        if (msg)
        {
            send_subscribe_resp(msg, ERRCODE_FAIL, DEV_MODULE_STATE_NOT_RUNNING, 0, 0);
        }
        return;
    }

    const dev_subscribe_req_t *req = (const dev_subscribe_req_t *)msg->payload;
    uint32_t target_id = ntohl(req->target_module_id);
    uint8_t auto_start = req->auto_start;
    uint32_t subscriber_id = msg->src_module_id;

    dev_module_t *target = dev_module_find(target_id);
    if (!target)
    {
        LOG_WARN("SUBSCRIBE: unknown target module 0x%08X (from 0x%08X)", target_id, subscriber_id);
        send_subscribe_resp(msg, ERRCODE_FAIL, DEV_MODULE_STATE_NOT_RUNNING, 0, 0);
        return;
    }

    /* 加入订阅者列表（幂等） */
    subscriber_list_add(target, subscriber_id);
    LOG_INFO("SUBSCRIBE: 0x%08X subscribed to %s (auto_start=%u)", subscriber_id, target->name, auto_start);

    /* 已 READY：响应里直接带 host/port，订阅方收到后会立即建联 */
    if (target->phase >= DEV_PHASE_READY)
    {
        send_subscribe_resp(msg, ERRCODE_SUCCESS, DEV_MODULE_STATE_READY, target->port, target->epoch);
        return;
    }

    /* 正在启动 / 等待依赖：响应 STARTING + 带上 port/epoch。
     * 模块 IPC 监听在 fork 后早期就 listen 上了，所以即便 phase 还没到 READY，
     * 订阅方此时 connect 也能成功；先把 TCP 通道建好，等 NOTIFY_READY 再推 MODULE_EVENT 触发业务回调。
     * 这避免了互订阅模块（如 IF↔ROUTE）在 cold-start 时各自等对方 READY 的循环死锁。 */
    if (target->child_pid > 0 || target->phase >= DEV_PHASE_LOADED)
    {
        send_subscribe_resp(msg, ERRCODE_SUCCESS, DEV_MODULE_STATE_STARTING, target->port, target->epoch);
        return;
    }

    /* 未运行：仅在 auto_start=1 且模块是 on-demand 时拉起 */
    if (auto_start && target->on_demand)
    {
        if (dev_module_spawn_on_demand(target) != ERRCODE_SUCCESS)
        {
            send_subscribe_resp(msg, ERRCODE_FAIL, DEV_MODULE_STATE_NOT_RUNNING, 0, 0);
            return;
        }
        /* DEV 主动连接新拉起的模块，确保后续推送 MODULE_EVENT 有通道
         * （IPC 库内部会重试，初次失败不报错） */
        dev_ipc_connect(dev_get_ipc_ctx(), target_id, DEV_IPC_HOST_LOCAL, target->port);
        send_subscribe_resp(msg, ERRCODE_SUCCESS, DEV_MODULE_STATE_STARTING, 0, 0);
        return;
    }

    /* 不可拉起：响应 NOT_RUNNING，订阅依然保留（将来重启会触发推送） */
    send_subscribe_resp(msg, ERRCODE_SUCCESS, DEV_MODULE_STATE_NOT_RUNNING, 0, 0);
}

/* ============================================================================
 * 消息处理：UNSUBSCRIBE
 * ============================================================================ */

void dev_subscribe_handle_unsubscribe(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(uint32_t))
    {
        return;
    }
    uint32_t target_id_be;
    memcpy(&target_id_be, msg->payload, sizeof(uint32_t));
    uint32_t target_id = ntohl(target_id_be);

    dev_module_t *target = dev_module_find(target_id);
    if (!target)
    {
        return;
    }
    subscriber_list_remove(target, msg->src_module_id);
    LOG_INFO("UNSUBSCRIBE: 0x%08X unsubscribed from %s", msg->src_module_id, target->name);
}

/* ============================================================================
 * 消息处理：NOTIFY_READY
 * ============================================================================ */

void dev_subscribe_handle_notify_ready(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    uint32_t sender_id = msg->src_module_id;
    dev_module_t *sender = dev_module_find(sender_id);
    if (!sender)
    {
        LOG_WARN("NOTIFY_READY from unknown module 0x%08X", sender_id);
        return;
    }

    sender->phase = DEV_PHASE_READY;
    sender->epoch++;
    LOG_INFO("Module %s ready (epoch=%u), broadcasting to %u subscriber(s)", sender->name, sender->epoch,
             g_list_length(sender->subscribers));

    dev_subscribe_broadcast_event(sender, DEV_MODULE_EVENT_READY);
}
