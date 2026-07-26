/**
 * @file   ospfv3_main.c
 * @brief  OSPFv3 module lifecycle and IPC dispatch
 */
#include "ospfv3_main.h"

#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "if.h"
#include "log.h"
#include "ospfv3_bdr.h"
#include "ospfv3_cli.h"
#include "ospfv3_db.h"
#include "ospfv3_worker.h"

ospfv3_local_t *g_ospfv3_local;

static gboolean g_ospfv3_db_ready;
static gboolean g_ospfv3_if_smoothend;
static gboolean g_ospfv3_db_restored;

static uint8_t ospfv3_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1u)
    {
        return 0u;
    }
    return ((const uint8_t *)msg->payload)[0];
}

static void ospfv3_post_internal(uint32_t msg_type)
{
    if (!g_ospfv3_local || !g_ospfv3_local->dev_ipc_ctx)
    {
        return;
    }

    dev_ipc_message_t *msg =
        dev_ipc_message_create(msg_type, DEV_MODULE_ID_OSPFV3, DEV_MODULE_ID_OSPFV3, 0u, NULL, 0u, NULL);
    if (msg)
    {
        g_async_queue_push(g_ospfv3_local->dev_ipc_ctx->msg_queue, msg);
    }
}

static void ospfv3_on_if_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                               void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event == DEV_MODULE_EVENT_READY)
    {
        ospfv3_post_internal(OSPFV3_MSG_TYPE_INTERNAL_IF_READY);
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        ospfv3_post_internal(OSPFV3_MSG_TYPE_INTERNAL_IF_DOWN);
    }
}

static void ospfv3_on_route_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                  void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        ospfv3_post_internal(OSPFV3_MSG_TYPE_INTERNAL_ROUTE_READY);
    }
}

static void ospfv3_on_db_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                               void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY)
    {
        ospfv3_post_internal(OSPFV3_MSG_TYPE_INTERNAL_DB_READY);
    }
}

static void ospfv3_try_db_restore(void)
{
    if (g_ospfv3_db_restored || !g_ospfv3_db_ready || !g_ospfv3_if_smoothend)
    {
        return;
    }

    if (ospfv3_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: database restore failed");
        return;
    }

    g_ospfv3_db_restored = TRUE;
    LOG_INFO("OSPFV3: database restore completed");
}

static void ospfv3_handle_db_ready(void)
{
    if (g_ospfv3_db_ready)
    {
        ospfv3_try_db_restore();
        return;
    }
    dev_ipc_context_t *ctx = ospfv3_local_ipc_ctx();
    if (!ctx || dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: DB connection is not ready");
        return;
    }
    if (ospfv3_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("OSPFV3: database initialization failed");
        return;
    }
    g_ospfv3_db_ready = TRUE;
    ospfv3_try_db_restore();
}

static void ospfv3_handle_if_ready(void)
{
    dev_ipc_context_t *ctx = ospfv3_local_ipc_ctx();
    if (!ctx || dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: IF connection is not ready");
        return;
    }
    if (if_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: failed to subscribe to IF events");
    }
}

void ospfv3_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    if (g_ospfv3_local && g_ospfv3_local->shutting_down)
    {
        dev_ipc_message_free(msg);
        return;
    }

    switch (msg->msg_type)
    {
        case OSPFV3_MSG_TYPE_INTERNAL_DB_READY:
            ospfv3_handle_db_ready();
            break;
        case OSPFV3_MSG_TYPE_INTERNAL_IF_READY:
            ospfv3_handle_if_ready();
            break;
        case OSPFV3_MSG_TYPE_INTERNAL_ROUTE_READY:
            if (ospfv3_worker_post_route_ready() != ERRCODE_SUCCESS)
            {
                LOG_WARN("OSPFV3: failed to queue ROUTE replay");
            }
            break;
        case OSPFV3_MSG_TYPE_INTERNAL_IF_DOWN:
            if (ospfv3_worker_post_if_down() != ERRCODE_SUCCESS)
            {
                LOG_WARN("OSPFV3: failed to queue IF teardown");
            }
            break;
        case CLI_MSG_TYPE:
            if ((ospfv3_cli_payload_flags(msg) & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0u)
            {
                if (ospfv3_worker_post_show_cli(msg) != ERRCODE_SUCCESS)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                if (db_rpc_guard_reject(ctx, msg, "OSPFV3"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
                (void)ospfv3_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        case CLI_MSG_TYPE_CONTINUE:
            if (ospfv3_bdr_stream_active())
            {
                (void)ospfv3_bdr_handle_continue(msg);
                dev_ipc_message_free(msg);
                return;
            }
            if (ospfv3_worker_post_show_cli(msg) != ERRCODE_SUCCESS)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)ospfv3_bdr_handle_show_config(msg);
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
                g_ospfv3_if_smoothend = TRUE;
                ospfv3_try_db_restore();
            }
            if (ospfv3_worker_post_if_event(msg) != ERRCODE_SUCCESS)
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

int ospfv3_module_init(void)
{
    log_set_tag("ospfv3");
    LOG_INFO("OSPFv3 module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_OSPFV3, "ospfv3", DEV_MODULE_PORT_OSPFV3, ospfv3_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("OSPFV3: IPC initialization failed");
        return -1;
    }

    g_ospfv3_local = g_malloc0(sizeof(*g_ospfv3_local));
    if (!g_ospfv3_local)
    {
        dev_ipc_destroy(ctx);
        return -1;
    }
    g_ospfv3_local->dev_ipc_ctx = ctx;

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("OSPFV3: timed out waiting for DEV");
    }

    if (ospfv3_worker_prepare() != ERRCODE_SUCCESS || ospfv3_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("OSPFV3: worker startup failed");
        ospfv3_worker_shutdown();
        ospfv3_module_cleanup();
        return -1;
    }

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_ROUTE, 0u, ospfv3_on_route_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: subscribe(ROUTE) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0u, ospfv3_on_if_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: subscribe(IF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0u, ospfv3_on_db_event, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0u, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: subscribe(CLI) failed");
    }

    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0u);
    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        ospfv3_handle_db_ready();
    }
    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("OSPFV3: notify_ready failed");
    }

    LOG_INFO("OSPFv3 module ready");
    return 0;
}

void ospfv3_module_cleanup(void)
{
    if (!g_ospfv3_local)
    {
        return;
    }

    g_ospfv3_local->shutting_down = 1;
    ospfv3_worker_shutdown();
    ospfv3_bdr_cleanup();

    dev_ipc_context_t *ctx = g_ospfv3_local->dev_ipc_ctx;
    if (ctx)
    {
        (void)dev_ipc_pre_exit_notify(ctx, 3000u);
    }
    g_ospfv3_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    g_free(g_ospfv3_local);
    g_ospfv3_local = NULL;
}
