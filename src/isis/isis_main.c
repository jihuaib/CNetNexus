/**
 * @file   isis_main.c
 * @brief  ISIS 模块主入口：生命周期处理与消息分发
 * @author jhb
 * @date   2026/04/11
 */
#include "isis_main.h"

#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "if.h"
#include "isis_bdr.h"
#include "isis_cli.h"
#include "isis_db.h"
#include "isis_worker.h"
#include "log.h"

isis_local_t *g_isis_local = NULL;

static uint8_t isis_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_ISIS,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
}

static void isis_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");

    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_IF, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_IF);
    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_ROUTE, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_ROUTE);

    send_phase_response(ctx, msg);
}

static void isis_on_connect(dev_ipc_message_t *msg)
{
    send_phase_response(isis_local_ipc_ctx(), msg);
}

static void isis_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = isis_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Initializing ISIS DB and worker");

    if (isis_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: DB init failed");
        send_phase_response(ctx, msg);
        return;
    }

    if (isis_worker_prepare() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: worker prepare failed");
        send_phase_response(ctx, msg);
        return;
    }

    if (isis_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ISIS: worker launch failed");
        isis_worker_shutdown();
        send_phase_response(ctx, msg);
        return;
    }

    if (if_api_subscribe_all(ctx) == ERRCODE_SUCCESS)
    {
        LOG_INFO("ISIS: Subscribed to IF events via if_api");
    }
    else
    {
        LOG_WARN("ISIS: Failed to subscribe IF events");
    }

    if (isis_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("ISIS: restore failed");
    }

    send_phase_response(ctx, msg);
}

void isis_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            isis_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            isis_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            isis_on_ready(msg);
            return;

        case CLI_MSG_TYPE:
        {
            uint8_t flags = isis_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (isis_worker_post_show_cli(msg) != 0)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                (void)isis_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        case CLI_MSG_TYPE_CONTINUE:
            if (isis_worker_post_show_cli(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)isis_bdr_handle_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        case IF_MSG_TYPE_EVENT:
            if (isis_worker_post_if_event(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case IF_MSG_TYPE_ACK:
            break;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

int isis_module_init(void)
{
    log_set_tag("isis");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_ISIS, "isis", DEV_MODULE_PORT_ISIS, isis_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("ISIS: IPC initialization failed");
        return -1;
    }

    g_isis_local = g_malloc0(sizeof(isis_local_t));
    if (!g_isis_local)
    {
        LOG_ERROR("ISIS: failed to allocate local context");
        dev_ipc_destroy(ctx);
        return -1;
    }

    g_isis_local->dev_ipc_ctx = ctx;
    return 0;
}

void isis_module_cleanup(void)
{
    if (!g_isis_local)
    {
        return;
    }

    isis_worker_shutdown();

    dev_ipc_context_t *ctx = g_isis_local->dev_ipc_ctx;
    g_isis_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_isis_local);
    g_isis_local = NULL;
}
