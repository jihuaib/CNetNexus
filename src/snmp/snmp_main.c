/**
 * @file   snmp_main.c
 * @brief  SNMP 模块 IPC 初始化与事件分发
 */
#include "snmp_main.h"

#include <stddef.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "log.h"
#include "snmp_bdr.h"
#include "snmp_cli.h"
#include "snmp_db.h"

#define SNMP_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SNMP, 0x1001)

snmp_local_t *g_snmp_local = NULL;

static uint8_t snmp_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

static void snmp_handle_db_ready(void)
{
    dev_ipc_context_t *ctx = snmp_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SNMP: DB not connected in time; db restore deferred");
        return;
    }
    if (snmp_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SNMP: DB init failed");
        return;
    }
    if (snmp_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("SNMP: DB restore failed");
    }
}

static void snmp_on_db_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event != DEV_MODULE_EVENT_READY || !g_snmp_local || !g_snmp_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(SNMP_MSG_TYPE_INTERNAL_DB_READY, DEV_MODULE_ID_SNMP,
                                                  DEV_MODULE_ID_SNMP, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_snmp_local->dev_ipc_ctx->msg_queue, m);
    }
}

void snmp_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case SNMP_MSG_TYPE_INTERNAL_DB_READY:
            snmp_handle_db_ready();
            break;

        case CLI_MSG_TYPE:
        {
            uint8_t flags = snmp_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) == 0)
            {
                if (db_rpc_guard_reject(ctx, msg, "SNMP"))
                {
                    break;
                }
                (void)snmp_cli_handle_config_msg(msg);
            }
            else
            {
                snmp_cli_send_response(msg, "");
            }
            break;
        }

        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)snmp_bdr_handle_show_config(msg);
            break;

        case SNMP_MSG_TYPE_VALUE_SET:
            if (!msg->payload || msg->payload_len < sizeof(snmp_value_msg_t))
            {
                LOG_WARN("SNMP: invalid VALUE_SET payload");
                break;
            }
            (void)snmp_agent_value_set((const snmp_value_msg_t *)msg->payload);
            break;

        case SNMP_MSG_TYPE_SUBTREE_CLEAR:
            if (!msg->payload || msg->payload_len < sizeof(snmp_subtree_clear_msg_t))
            {
                LOG_WARN("SNMP: invalid SUBTREE_CLEAR payload");
                break;
            }
            (void)snmp_agent_subtree_clear((const snmp_subtree_clear_msg_t *)msg->payload);
            break;

        case SNMP_MSG_TYPE_TRAP_SEND:
            if (!msg->payload || msg->payload_len < sizeof(snmp_trap_msg_t))
            {
                LOG_WARN("SNMP: invalid TRAP_SEND payload");
                break;
            }
            snmp_agent_send_trap((const snmp_trap_msg_t *)msg->payload);
            break;

        case SNMP_MSG_TYPE_CONFIG_SET:
            if (!msg->payload || msg->payload_len < sizeof(snmp_config_msg_t))
            {
                LOG_WARN("SNMP: invalid CONFIG_SET payload");
                break;
            }
            snmp_agent_apply_config((const snmp_config_msg_t *)msg->payload);
            break;

        default:
            LOG_WARN("SNMP: unknown message type 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

int snmp_module_init(void)
{
    log_set_tag("snmp");
    LOG_INFO("SNMP: module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_SNMP, "snmp", DEV_MODULE_PORT_SNMP, snmp_ipc_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("SNMP: IPC initialization failed");
        return -1;
    }

    g_snmp_local = (snmp_local_t *)g_malloc0(sizeof(*g_snmp_local));
    if (!g_snmp_local)
    {
        dev_ipc_destroy(ctx);
        return -1;
    }
    g_snmp_local->dev_ipc_ctx = ctx;

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SNMP: timed out waiting for DEV connection");
    }

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, snmp_on_db_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SNMP: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SNMP: subscribe(CLI) failed");
    }
    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);

    if (snmp_agent_init() != 0)
    {
        LOG_ERROR("SNMP: agent initialization failed");
        return -1;
    }

    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        snmp_handle_db_ready();
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("SNMP: notify_ready to DEV failed");
    }

    LOG_INFO("SNMP: module ready");
    return 0;
}

void snmp_module_cleanup(void)
{
    dev_ipc_context_t *ctx = NULL;
    if (g_snmp_local)
    {
        ctx = g_snmp_local->dev_ipc_ctx;
    }

    snmp_agent_shutdown();

    if (ctx)
    {
        dev_ipc_pre_exit_notify(ctx, 3000);
    }

    if (g_snmp_local)
    {
        g_snmp_local->dev_ipc_ctx = NULL;
    }
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_snmp_local);
    g_snmp_local = NULL;
}
