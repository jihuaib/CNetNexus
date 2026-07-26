/**
 * @file   ospf_main.c
 * @brief  OSPFv2 module lifecycle and IPC dispatch
 */
#include "ospf_main.h"

#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "if.h"
#include "log.h"
#include "ospf_bdr.h"
#include "ospf_cli.h"
#include "ospf_db.h"
#include "ospf_worker.h"

ospf_local_t *g_ospf_local;

static gboolean g_ospf_db_ready;
static gboolean g_ospf_if_smoothend;
static gboolean g_ospf_db_restored;

static uint8_t ospf_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1u)
    {
        return 0u;
    }
    return ((const uint8_t *)msg->payload)[0];
}

static void ospf_post_internal(uint32_t msg_type)
{
    if (!g_ospf_local || !g_ospf_local->dev_ipc_ctx)
    {
        return;
    }

    dev_ipc_message_t *msg =
        dev_ipc_message_create(msg_type, DEV_MODULE_ID_OSPF, DEV_MODULE_ID_OSPF, 0u, NULL, 0u, NULL);
    if (msg)
    {
        g_async_queue_push(g_ospf_local->dev_ipc_ctx->msg_queue, msg);
    }
}

static void ospf_on_if_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                             void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event == DEV_MODULE_EVENT_READY)
    {
        ospf_post_internal(OSPF_MSG_TYPE_INTERNAL_IF_READY);
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        ospf_post_internal(OSPF_MSG_TYPE_INTERNAL_IF_DOWN);
    }
}

static void ospf_on_route_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        ospf_post_internal(OSPF_MSG_TYPE_INTERNAL_ROUTE_READY);
    }
}

static void ospf_on_db_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                             void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        ospf_post_internal(OSPF_MSG_TYPE_INTERNAL_DB_READY);
    }
}

static void ospf_try_db_restore(void)
{
    if (g_ospf_db_restored || !g_ospf_db_ready || !g_ospf_if_smoothend)
    {
        return;
    }

    if (ospf_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: database restore failed");
        return;
    }

    g_ospf_db_restored = TRUE;
    LOG_INFO("OSPF: database restore completed");
}

static void ospf_handle_db_ready(void)
{
    if (g_ospf_db_ready)
    {
        ospf_try_db_restore();
        return;
    }
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: DB connection is not ready");
        return;
    }
    if (ospf_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("OSPF: database initialization failed");
        return;
    }
    g_ospf_db_ready = TRUE;
    ospf_try_db_restore();
}

static void ospf_handle_if_ready(void)
{
    dev_ipc_context_t *ctx = ospf_local_ipc_ctx();
    if (!ctx || dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: IF connection is not ready");
        return;
    }
    if (if_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: failed to subscribe to IF events");
    }
}

void ospf_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    if (g_ospf_local && g_ospf_local->shutting_down)
    {
        dev_ipc_message_free(msg);
        return;
    }

    switch (msg->msg_type)
    {
        case OSPF_MSG_TYPE_INTERNAL_DB_READY:
            ospf_handle_db_ready();
            break;
        case OSPF_MSG_TYPE_INTERNAL_IF_READY:
            ospf_handle_if_ready();
            break;
        case OSPF_MSG_TYPE_INTERNAL_ROUTE_READY:
            if (ospf_worker_post_route_ready() != ERRCODE_SUCCESS)
            {
                LOG_WARN("OSPF: failed to queue ROUTE replay");
            }
            break;
        case OSPF_MSG_TYPE_INTERNAL_IF_DOWN:
            if (ospf_worker_post_if_down() != ERRCODE_SUCCESS)
            {
                LOG_WARN("OSPF: failed to queue IF teardown");
            }
            break;
        case CLI_MSG_TYPE:
            if ((ospf_cli_payload_flags(msg) & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0u)
            {
                if (ospf_worker_post_show_cli(msg) != ERRCODE_SUCCESS)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                if (db_rpc_guard_reject(ctx, msg, "OSPF"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
                (void)ospf_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        case CLI_MSG_TYPE_CONTINUE:
            if (ospf_bdr_stream_active())
            {
                (void)ospf_bdr_handle_continue(msg);
                dev_ipc_message_free(msg);
                return;
            }
            if (ospf_worker_post_show_cli(msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)ospf_bdr_handle_show_config(msg);
            dev_ipc_message_free(msg);
            return;
        case IF_MSG_TYPE_EVENT:
        {
            uint32_t event = 0u;
            if (msg->payload && msg->payload_len >= sizeof(if_event_msg_t))
            {
                event = ((const if_event_msg_t *)msg->payload)->event;
            }
            if (event == IF_EVENT_SMOOTHEND)
            {
                g_ospf_if_smoothend = TRUE;
                ospf_try_db_restore();
            }
            if (ospf_worker_post_if_event(msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        }
        case IF_MSG_TYPE_ACK:
            break;
        default:
            break;
    }

    dev_ipc_message_free(msg);
}

int ospf_module_init(void)
{
    log_set_tag("ospf");
    LOG_INFO("OSPFv2 module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_OSPF, "ospf", DEV_MODULE_PORT_OSPF, ospf_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("OSPF: IPC initialization failed");
        return -1;
    }

    g_ospf_local = g_malloc0(sizeof(*g_ospf_local));
    if (!g_ospf_local)
    {
        dev_ipc_destroy(ctx);
        return -1;
    }
    g_ospf_local->dev_ipc_ctx = ctx;

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("OSPF: timed out waiting for DEV");
    }

    if (ospf_worker_prepare() != ERRCODE_SUCCESS || ospf_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("OSPF: worker startup failed");
        ospf_worker_shutdown();
        ospf_module_cleanup();
        return -1;
    }

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0u, ospf_on_route_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: subscribe(ROUTE) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0u, ospf_on_if_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: subscribe(IF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0u, ospf_on_db_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0u, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: subscribe(CLI) failed");
    }

    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0u);
    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        ospf_handle_db_ready();
    }
    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPF: notify_ready failed");
    }

    LOG_INFO("OSPFv2 module ready");
    return 0;
}

void ospf_module_cleanup(void)
{
    if (!g_ospf_local)
    {
        return;
    }

    g_ospf_local->shutting_down = 1;
    ospf_worker_shutdown();
    ospf_bdr_cleanup();

    dev_ipc_context_t *ctx = g_ospf_local->dev_ipc_ctx;
    if (ctx)
    {
        (void)dev_ipc_pre_exit_notify(ctx, 3000u);
    }
    g_ospf_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_ospf_local);
    g_ospf_local = NULL;
}
