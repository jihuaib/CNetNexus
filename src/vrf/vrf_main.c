/**
 * @file   vrf_main.c
 * @brief  VRF 模块主入口：IPC 线程 + 三阶段生命周期 + 消息分发
 * @author jhb
 * @date   2026/03/05
 */
#include "vrf_main.h"

#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "vrf_bdr.h"
#include "vrf_cli.h"
#include "vrf_db.h"
#include "work/vrf_worker.h"

vrf_local_t *g_vrf_local = NULL;

// ============================================================================
// 三阶段
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_VRF,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
    (void)result;
}

static void on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Connect peers, prepare and launch worker");

    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);

    if (vrf_worker_prepare() != ERRCODE_SUCCESS || vrf_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("VRF: worker prepare/launch failed");
        vrf_worker_shutdown();
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

static void on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

static void on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = vrf_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - DB init + restore");

    if (vrf_db_init() != 0)
    {
        LOG_WARN("VRF: db init failed");
    }
    else
    {
        (void)vrf_worker_dispatch_db_restore();
    }
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息分发
// ============================================================================

void vrf_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (!msg)
    {
        return;
    }
    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            on_ready(msg);
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            vrf_bdr_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        case CLI_MSG_TYPE:
        {
            uint8_t flags = 0;
            if (msg->payload && msg->payload_len >= 1)
            {
                flags = ((const uint8_t *)msg->payload)[0];
            }
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (vrf_worker_post_ipc_message(msg) != ERRCODE_SUCCESS)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                vrf_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        case CLI_MSG_TYPE_CONTINUE:
        case CLI_MSG_TYPE_QUERY_CANDIDATES:
        case VRF_MSG_TYPE_SUBSCRIBE:
        case VRF_MSG_TYPE_UNSUBSCRIBE:
            if (vrf_worker_post_ipc_message(msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;

        default:
            LOG_WARN("VRF: unknown msg type 0x%08X", msg->msg_type);
            dev_ipc_message_free(msg);
            return;
    }
}

// ============================================================================
// 模块初始化 / 清理
// ============================================================================

int vrf_module_init(void)
{
    log_set_tag("vrf");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_VRF, "vrf", DEV_MODULE_PORT_VRF, vrf_msg_handler);
    if (!ctx)
    {
        return -1;
    }
    g_vrf_local = g_malloc0(sizeof(*g_vrf_local));
    g_vrf_local->dev_ipc_ctx = ctx;
    return 0;
}

void vrf_module_cleanup(void)
{
    vrf_worker_shutdown();

    if (!g_vrf_local)
    {
        return;
    }
    dev_ipc_context_t *ctx = g_vrf_local->dev_ipc_ctx;
    g_vrf_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }
    g_free(g_vrf_local);
    g_vrf_local = NULL;
}
