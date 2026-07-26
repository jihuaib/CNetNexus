/**
 * @file   lldp_main.c
 * @brief  LLDP 模块主入口
 * @author jhb
 * @date   2026/06/07
 */
#include "lldp_main.h"

#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "if.h"
#include "lldp_bdr.h"
#include "lldp_cli.h"
#include "lldp_db.h"
#include "log.h"
#include "work/lldp_snmp_report.h"
#include "work/lldp_worker.h"

lldp_local_t *g_lldp_local = NULL;

static uint8_t lldp_cli_payload_flags(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < 1)
    {
        return 0;
    }
    return ((const uint8_t *)msg->payload)[0];
}

static gboolean g_lldp_db_ready = FALSE;
static gboolean g_lldp_if_smoothend = FALSE;
static gboolean g_lldp_db_restored = FALSE;

static void lldp_try_db_restore(void)
{
    if (g_lldp_db_restored || !g_lldp_db_ready || !g_lldp_if_smoothend)
    {
        return;
    }
    if (lldp_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: DB restore failed");
        return;
    }
    g_lldp_db_restored = TRUE;
    LOG_INFO("LLDP: DB restore completed");
}

static void lldp_handle_db_ready(void)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: DB not connected in time; db restore deferred");
        return;
    }
    if (lldp_db_init() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LLDP: DB init failed");
        return;
    }
    g_lldp_db_ready = TRUE;
    lldp_try_db_restore();
}

static void lldp_handle_if_ready(void)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_IF, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: IF connection not ready in time; subscribe deferred to next READY");
        return;
    }
    if (if_api_subscribe_all(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: if_api_subscribe_all failed");
    }
    else
    {
        LOG_INFO("LLDP: subscribed to IF events");
    }
}

static void lldp_handle_if_smoothend(void)
{
    gboolean first = !g_lldp_if_smoothend;
    g_lldp_if_smoothend = TRUE;
    if (first)
    {
        LOG_INFO("LLDP: IF smoothend received (initial sync)");
        lldp_try_db_restore();
    }
    else
    {
        LOG_INFO("LLDP: IF smoothend received (resync)");
    }
}

static void lldp_on_if_ready_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (!g_lldp_local || !g_lldp_local->dev_ipc_ctx)
    {
        return;
    }

    uint32_t msg_type;
    if (event == DEV_MODULE_EVENT_READY)
    {
        msg_type = LLDP_MSG_TYPE_INTERNAL_IF_READY;
    }
    else if (event == DEV_MODULE_EVENT_DOWN)
    {
        msg_type = LLDP_MSG_TYPE_INTERNAL_IF_DOWN;
    }
    else
    {
        return;
    }

    dev_ipc_message_t *m = dev_ipc_message_create(msg_type, DEV_MODULE_ID_LLDP, DEV_MODULE_ID_LLDP, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_lldp_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void lldp_on_db_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event != DEV_MODULE_EVENT_READY || !g_lldp_local || !g_lldp_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(LLDP_MSG_TYPE_INTERNAL_DB_READY, DEV_MODULE_ID_LLDP,
                                                  DEV_MODULE_ID_LLDP, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_lldp_local->dev_ipc_ctx->msg_queue, m);
    }
}

static void lldp_on_snmp_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                  void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;

    if (event != DEV_MODULE_EVENT_READY || !g_lldp_local || !g_lldp_local->dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(LLDP_MSG_TYPE_INTERNAL_SNMP_READY, DEV_MODULE_ID_LLDP,
                                                  DEV_MODULE_ID_LLDP, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_lldp_local->dev_ipc_ctx->msg_queue, m);
    }
}

void lldp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case LLDP_MSG_TYPE_INTERNAL_DB_READY:
            lldp_handle_db_ready();
            break;
        case LLDP_MSG_TYPE_INTERNAL_IF_READY:
            lldp_handle_if_ready();
            break;
        case LLDP_MSG_TYPE_INTERNAL_IF_DOWN:
            (void)lldp_worker_post_if_down();
            break;
        case LLDP_MSG_TYPE_INTERNAL_SNMP_READY:
            (void)dev_ipc_wait_connected(ctx, DEV_MODULE_ID_SNMP, DEV_IPC_WAIT_PEER_MS);
            lldp_snmp_report_refresh();
            break;
        case CLI_MSG_TYPE:
        {
            uint8_t flags = lldp_cli_payload_flags(msg);
            if ((flags & CLI_PAYLOAD_FLAG_SHOW_CMD) != 0)
            {
                if (lldp_worker_post_show_cli(msg) != 0)
                {
                    dev_ipc_message_free(msg);
                }
            }
            else
            {
                if (db_rpc_guard_reject(ctx, msg, "LLDP"))
                {
                    dev_ipc_message_free(msg);
                    return;
                }
                (void)lldp_cli_handle_config_msg(msg);
                dev_ipc_message_free(msg);
            }
            return;
        }
        break;
        case CLI_MSG_TYPE_CONTINUE:
            if (lldp_worker_post_show_cli(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)lldp_bdr_handle_show_config(msg);
            dev_ipc_message_free(msg);
            return;
        case IF_MSG_TYPE_EVENT:
        {
            uint32_t if_event = 0;
            if (msg->payload && msg->payload_len >= sizeof(if_event_msg_t))
            {
                if_event = ((const if_event_msg_t *)msg->payload)->event;
            }
            if (if_event == IF_EVENT_SMOOTHEND)
            {
                lldp_handle_if_smoothend();
            }
            if (lldp_worker_post_if_event(msg) != 0)
            {
                dev_ipc_message_free(msg);
            }
            return;
        }
        default:
            break;
    }

    dev_ipc_message_free(msg);
}

int lldp_module_init(void)
{
    log_set_tag("lldp");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_LLDP, "lldp", DEV_MODULE_PORT_LLDP, lldp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("LLDP: IPC initialization failed");
        return -1;
    }

    g_lldp_local = g_malloc0(sizeof(lldp_local_t));
    if (!g_lldp_local)
    {
        dev_ipc_destroy(ctx);
        return -1;
    }
    g_lldp_local->dev_ipc_ctx = ctx;

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LLDP: timed out waiting for DEV connection; module may be unusable");
    }

    if (lldp_worker_prepare() != ERRCODE_SUCCESS || lldp_worker_launch() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("LLDP: worker start failed");
        lldp_worker_shutdown();
        return -1;
    }

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_IF, 0, lldp_on_if_ready_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: subscribe(IF) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, lldp_on_db_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: subscribe(DB) failed");
    }
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: subscribe(CLI) failed");
    }

    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_SNMP, 0, lldp_on_snmp_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: optional subscribe(SNMP) failed");
    }

    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        lldp_handle_db_ready();
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP: notify_ready to DEV failed");
    }

    LOG_INFO("LLDP: module ready");
    lldp_snmp_report_refresh();
    return 0;
}

void lldp_module_cleanup(void)
{
    if (!g_lldp_local)
    {
        return;
    }

    dev_ipc_context_t *ctx = g_lldp_local->dev_ipc_ctx;
    if (ctx)
    {
        dev_ipc_pre_exit_notify(ctx, 3000);
    }

    g_lldp_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    lldp_worker_shutdown();
    g_free(g_lldp_local);
    g_lldp_local = NULL;
}
