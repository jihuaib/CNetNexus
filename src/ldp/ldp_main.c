/**
 * @file   ldp_main.c
 * @brief  LDP 模块主入口：生命周期与 IPC 消息分发
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_main.h"

#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "if.h"
#include "ldp_bdr.h"
#include "ldp_cli.h"
#include "ldp_db.h"
#include "log.h"
#include "work/ldp_worker.h"

ldp_local_t *g_ldp_local = NULL;

static uint8_t ldp_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_LDP,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
}

static void ldp_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");

    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_IF, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_IF);
    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_ROUTE, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_ROUTE);
    (void)dev_ipc_connect(ctx, DEV_MODULE_ID_TUNNEL, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_TUNNEL);

    send_phase_response(ctx, msg);
}

static void ldp_on_connect(dev_ipc_message_t *msg)
{
    send_phase_response(ldp_local_ipc_ctx(), msg);
}

static void ldp_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Initializing LDP DB and worker");

    if (ldp_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: DB init failed");
        send_phase_response(ctx, msg);
        return;
    }

    if (ldp_worker_prepare() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: worker prepare failed");
        send_phase_response(ctx, msg);
        return;
    }

    if (ldp_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LDP: worker launch failed");
        ldp_worker_shutdown();
        send_phase_response(ctx, msg);
        return;
    }

    if (if_api_subscribe_all(ctx) == ERRCODE_SUCCESS)
    {
        LOG_INFO("LDP: Subscribed to IF events via if_api");
    }
    else
    {
        LOG_WARN("LDP: Failed to subscribe IF events");
    }

    if (ldp_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("LDP: restore failed");
    }

    send_phase_response(ctx, msg);
}

void ldp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            ldp_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            ldp_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            ldp_on_ready(msg);
            return;

        case CLI_MSG_TYPE:
        {
            uint8_t flags = ldp_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (ldp_worker_post_show_cli(msg) != 0)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                (void)ldp_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }

        case CLI_MSG_TYPE_CONTINUE:
            if (ldp_worker_post_show_cli(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)ldp_bdr_handle_show_config(msg);
            dev_ipc_message_free(msg);
            return;

        case IF_MSG_TYPE_EVENT:
            if (ldp_worker_post_if_event(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

int ldp_module_init(void)
{
    log_set_tag("ldp");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_LDP, "ldp", DEV_MODULE_PORT_LDP, ldp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("LDP: IPC initialization failed");
        return -1;
    }

    g_ldp_local = g_malloc0(sizeof(ldp_local_t));
    if (!g_ldp_local)
    {
        LOG_ERROR("LDP: failed to allocate local context");
        dev_ipc_destroy(ctx);
        return -1;
    }

    g_ldp_local->dev_ipc_ctx = ctx;
    return 0;
}

void ldp_module_cleanup(void)
{
    if (!g_ldp_local)
    {
        return;
    }

    ldp_worker_shutdown();

    dev_ipc_context_t *ctx = g_ldp_local->dev_ipc_ctx;
    g_ldp_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_ldp_local);
    g_ldp_local = NULL;
}
