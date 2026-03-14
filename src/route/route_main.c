/**
 * @file   route_main.c
 * @brief  Route 模块主入口，三阶段初始化和 IPC 消息分发
 * @author jhb
 * @date   2026/02/01
 */
#include "route_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "route.h"
#include "route_cli.h"
#include "route_pub.h"

route_local_t *g_route_local = NULL;

/* route_static 表结构定义（仅存储用户配置，批量生成路由不入库） */
static const db_column_def_t ROUTE_STATIC_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},     {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"prefix", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},       {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"nexthop", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},      {"metric", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"preference", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
};

static const db_table_def_t ROUTE_STATIC_TABLE = {
    .table_name = "route_static",
    .cols = ROUTE_STATIC_COLS,
    .num_cols = G_N_ELEMENTS(ROUTE_STATIC_COLS),
};

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_ROUTE,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START - Establishing IPC connections
// ============================================================================

static void route_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB) < 0)
    {
        LOG_ERROR("Failed to connect to DB module");
    }

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI) != 0)
    {
        LOG_ERROR("Failed to connect to CLI module");
    }

    LOG_INFO("Connected to DB, CLI");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留
// ============================================================================

static void route_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 初始化 DB 并恢复 RIB
// ============================================================================

static void route_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY - Initializing Route database");

    int ret = db_rpc_create_table_from_def(ctx, &ROUTE_STATIC_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route table creation failed: route_static");
        send_phase_response(ctx, msg, ERRCODE_SUCCESS);
        return;
    }

    /* 从 DB 恢复静态路由到内存 RIB */
    db_result_t *result = NULL;
    ret = db_rpc_query(ctx, "route_static", NULL, 0, NULL, &result);
    if (ret == ERRCODE_SUCCESS && result)
    {
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            db_row_t *row = result->rows[i];
            uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", 0);
            uint16_t afi = (uint16_t)db_row_get_int(row, "afi", ROUTE_AFI_IPV4);
            uint8_t prefix_len = (uint8_t)db_row_get_int(row, "prefix_len", 0);
            int32_t metric = (int32_t)db_row_get_int(row, "metric", 0);
            int32_t preference = (int32_t)db_row_get_int(row, "preference", ROUTE_ADMIN_DIST_STATIC);
            const char *prefix = db_row_get_text(row, "prefix", "");
            const char *nexthop = db_row_get_text(row, "nexthop", "");

            route_rib_add(g_route_local->rib, vrf_id, afi, prefix, prefix_len, ROUTE_PROTOCOL_STATIC, nexthop, nexthop,
                          metric, preference);
        }
        LOG_INFO("Restored %u static routes from DB to RIB", result->num_rows);
        db_result_free(result);
    }

    LOG_INFO("Route database tables ready");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void route_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Shutting down Route module...");

    if (g_route_local)
    {
        g_route_local->running = 0;
        g_route_local->dev_ipc_ctx = NULL;

        route_cli_cleanup_state();

        if (g_route_local->rib)
        {
            route_rib_destroy(g_route_local->rib);
            g_route_local->rib = NULL;
        }

        g_list_free_full(g_route_local->subscribers, g_free);
        g_route_local->subscribers = NULL;

        g_list_free_full(g_route_local->batch_entries, g_free);
        g_route_local->batch_entries = NULL;

        free(g_route_local);
        g_route_local = NULL;
    }

    LOG_INFO("Route module cleanup complete");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// subscribe/unsubscribe处理
// ============================================================================

static void handle_subscribe(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_subscribe_req_t))
    {
        LOG_WARN("SUBSCRIBE payload length insufficient: %u", msg->payload_len);
        dev_ipc_message_free(msg);
        return;
    }

    const route_subscribe_req_t *req = (const route_subscribe_req_t *)msg->payload;
    uint32_t protocol = req->protocol;
    uint32_t vrf_id = req->vrf_id;
    uint32_t flags = req->flags;

    /* 去重：若已存在相同subscribe则跳过 */
    for (GList *l = g_route_local->subscribers; l; l = l->next)
    {
        route_subscriber_t *sub = (route_subscriber_t *)l->data;
        if (sub->module_id == msg->src_module_id && sub->protocol == protocol && sub->vrf_id == vrf_id)
        {
            LOG_DEBUG("Module 0x%08X duplicate subscription, ignoring", msg->src_module_id);
            /* 如果请求全量，仍然发送 */
            if (flags & ROUTE_SUBSCRIBE_FLAG_FULL)
            {
                route_pub_dump(ctx, g_route_local->rib, msg->src_module_id, protocol, vrf_id, msg->request_id);
            }
            else
            {
                route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
                ack->result = ERRCODE_SUCCESS;
                dev_ipc_message_t *resp =
                    dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id, msg->request_id,
                                           ack, sizeof(route_msg_ack_t), g_free);
                dev_ipc_send_response(ctx, resp);
                dev_ipc_message_free(resp);
            }
            dev_ipc_message_free(msg);
            return;
        }
    }

    /* 添加subscribe者 */
    route_subscriber_t *sub = (route_subscriber_t *)g_malloc(sizeof(route_subscriber_t));
    sub->module_id = msg->src_module_id;
    sub->protocol = protocol;
    sub->vrf_id = vrf_id;
    g_route_local->subscribers = g_list_append(g_route_local->subscribers, sub);

    LOG_INFO("Module 0x%08X subscribed to routes: protocol=%u vrf=%u flags=0x%X", msg->src_module_id, protocol, vrf_id,
             flags);

    /* 响应：全量上报或 ACK */
    if (flags & ROUTE_SUBSCRIBE_FLAG_FULL)
    {
        route_pub_dump(ctx, g_route_local->rib, msg->src_module_id, protocol, vrf_id, msg->request_id);
    }
    else
    {
        route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
        ack->result = ERRCODE_SUCCESS;
        dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                         msg->request_id, ack, sizeof(route_msg_ack_t), g_free);
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }

    dev_ipc_message_free(msg);
}

static void handle_unsubscribe(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_subscribe_req_t))
    {
        dev_ipc_message_free(msg);
        return;
    }

    const route_subscribe_req_t *req = (const route_subscribe_req_t *)msg->payload;
    uint32_t protocol = req->protocol;
    uint32_t vrf_id = req->vrf_id;

    GList *l = g_route_local->subscribers;
    while (l)
    {
        route_subscriber_t *sub = (route_subscriber_t *)l->data;
        GList *next = l->next;
        if (sub->module_id == msg->src_module_id && sub->protocol == protocol && sub->vrf_id == vrf_id)
        {
            g_route_local->subscribers = g_list_delete_link(g_route_local->subscribers, l);
            g_free(sub);
            LOG_INFO("Module 0x%08X unsubscribed: protocol=%u vrf=%u", msg->src_module_id, protocol, vrf_id);
            break;
        }
        l = next;
    }

    route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
    ack->result = ERRCODE_SUCCESS;
    dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                     msg->request_id, ack, sizeof(route_msg_ack_t), g_free);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void route_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (msg == NULL)
    {
        return;
    }

    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            route_on_start(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            route_on_connect(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            route_on_ready(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            route_on_shutdown(ctx, msg);
            return;

        /* ---- CLI 消息 ---- */
        case CLI_MSG_TYPE:
            LOG_DEBUG("Received CLI command message (%u bytes)", msg->payload_len);
            route_cli_handle_message(msg);
            return;

        case CLI_MSG_TYPE_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            route_cli_handle_continue(msg);
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            route_cli_handle_show_config(msg);
            return;

        /* ---- 路由subscribe消息 ---- */
        case ROUTE_MSG_TYPE_SUBSCRIBE:
            handle_subscribe(ctx, msg);
            return;

        case ROUTE_MSG_TYPE_UNSUBSCRIBE:
            handle_unsubscribe(ctx, msg);
            return;

        default:
            LOG_WARN("Received unknown message type: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// Module initialization
// ============================================================================

int route_module_init(void)
{
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_ROUTE, "route", DEV_MODULE_PORT_ROUTE, route_ipc_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_route_local = (route_local_t *)g_malloc0(sizeof(route_local_t));
    if (!g_route_local)
    {
        LOG_ERROR("Failed to allocate route context");
        return -1;
    }

    g_route_local->dev_ipc_ctx = ctx;
    g_route_local->running = 1;

    g_route_local->rib = route_rib_create();
    if (!g_route_local->rib)
    {
        LOG_ERROR("Failed to create RIB");
        return -1;
    }

    return 0;
}
