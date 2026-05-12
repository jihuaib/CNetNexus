/**
 * @file   if_main.c
 * @brief  接口模块主入口：三阶段初始化与 IPC 消息分发
 * @author jhb
 * @date   2026/01/22
 */
#include "if_main.h"

#include <glib.h>
#include <stddef.h>
#include <stdlib.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "if.h"
#include "if_bdr.h"
#include "if_cli.h"
#include "if_db.h"
#include "if_link_monitor.h"
#include "log.h"
#include "vrf.h"
#include "work/if_worker.h"

if_local_t *g_if_local = NULL;

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_IF,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START - 建立 IPC 连接并启动 worker
// ============================================================================

static void if_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");

    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    dev_ipc_connect(ctx, DEV_MODULE_ID_ROUTE, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_ROUTE);
    dev_ipc_connect(ctx, DEV_MODULE_ID_VRF, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_VRF);

    if (if_worker_prepare() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: worker prepare failed");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    if (if_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: worker launch failed");
        if_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    LOG_INFO("Connected to CLI, DB, ROUTE and VRF; IF worker running");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT - 预留
// ============================================================================

static void if_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY - 建表、恢复配置、启动链路监控
// ============================================================================

static void if_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Initializing IF database");

    if (if_db_init() != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF database init failed");
        send_phase_response(ctx, msg, ERRCODE_SUCCESS);
        return;
    }

    if (if_link_monitor_start() != 0)
    {
        LOG_WARN("IF: link monitor start failed, link recovery disabled");
    }

    if (vrf_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF: failed to subscribe to VRF events via vrf_api");
    }
    else
    {
        LOG_INFO("IF: subscribed to VRF events via vrf_api");
    }

    if (if_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF database restore failed");
    }

    LOG_INFO("IF module ready");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息分发
// ============================================================================

void if_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息：IPC 线程直接处理 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            if_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            if_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            if_on_ready(msg);
            return;

        /* ---- show current-configuration：IPC 线程直接读 DB 生成 ---- */
        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            if_bdr_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        /* ---- IF 订阅应答：静默丢弃 ---- */
        case IF_MSG_TYPE_ACK:
            dev_ipc_message_free(msg);
            return;

        case VRF_MSG_TYPE_EVENT:
        {
            /* VRF 事件到达后，唤醒等待该 VRF 出现的挂起项。
             * 用 offsetof(rts) 而非 sizeof(vrf_event_msg_t)——后者多算了末尾变长数组的一个元素，
             * 对 rt_count=0 的 ADD/STATE/AF_ENABLE 事件会误判为载荷不足。 */
            char vrf_name[VRF_NAME_MAX_LEN] = "";
            uint32_t vrf_event = 0;
            if (msg->payload && msg->payload_len >= offsetof(vrf_event_msg_t, rts))
            {
                const vrf_event_msg_t *evt = (const vrf_event_msg_t *)msg->payload;
                vrf_event = evt->event;
                g_strlcpy(vrf_name, evt->name, sizeof(vrf_name));
            }
            if (if_worker_dispatch_vrf_event(msg) != ERRCODE_SUCCESS)
            {
                LOG_WARN("IF: failed to dispatch VRF event to worker");
                dev_ipc_message_free(msg);
                return;
            }
            if (vrf_name[0] != '\0')
            {
                if (vrf_event == VRF_EVENT_VRF_ADD)
                {
                    pending_resolve(g_if_local->pending, IF_DEP_VRF, g_str_hash(vrf_name));
                }
                else if (vrf_event == VRF_EVENT_VRF_DEL)
                {
                    pending_invalidate(g_if_local->pending, IF_DEP_VRF, g_str_hash(vrf_name));
                }
            }
            return;
        }

        case VRF_MSG_TYPE_ACK:
            dev_ipc_message_free(msg);
            return;

        /* ---- CLI 命令：show 转 worker，配置在 IPC 线程解析后 dispatch apply ---- */
        case CLI_MSG_TYPE:
        {
            uint8_t flags = 0;
            if (msg->payload && msg->payload_len >= 1)
            {
                flags = ((const uint8_t *)msg->payload)[0];
            }
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (if_worker_post_ipc_message(msg) != ERRCODE_SUCCESS)
                {
                    LOG_WARN("IF: failed to post show cmd to worker");
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                if_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        /* ---- show 分片续传 / 候选查询 / 订阅：交给 worker（共享 show_stream 与业务数据） ---- */
        case CLI_MSG_TYPE_CONTINUE:
        case CLI_MSG_TYPE_QUERY_CANDIDATES:
        case IF_MSG_TYPE_SUBSCRIBE:
        case IF_MSG_TYPE_UNSUBSCRIBE:
            if (if_worker_post_ipc_message(msg) != ERRCODE_SUCCESS)
            {
                LOG_WARN("IF: failed to post msg 0x%08X to worker", msg->msg_type);
                dev_ipc_message_free(msg);
            }
            return;

        default:
            LOG_WARN("IF: received unknown message type: 0x%08X", msg->msg_type);
            dev_ipc_message_free(msg);
            return;
    }
}

// ============================================================================
// Module initialization
// ============================================================================

int if_module_init(void)
{
    log_set_tag("if");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_IF, "if", DEV_MODULE_PORT_IF, if_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_if_local = g_malloc0(sizeof(if_local_t));
    if (!g_if_local)
    {
        LOG_ERROR("Failed to allocate IF local context");
        dev_ipc_destroy(ctx);
        return -1;
    }
    g_if_local->dev_ipc_ctx = ctx;
    g_if_local->pending = pending_new("if");

    return 0;
}

void if_module_cleanup(void)
{
    if_link_monitor_stop();
    if_worker_shutdown();

    if (!g_if_local)
    {
        return;
    }

    dev_ipc_context_t *ctx = g_if_local->dev_ipc_ctx;
    g_if_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    if (g_if_local->pending)
    {
        pending_destroy(g_if_local->pending);
        g_if_local->pending = NULL;
    }

    g_free(g_if_local);
    g_if_local = NULL;
}
