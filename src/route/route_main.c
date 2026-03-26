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
#include "net_addr.h"
#include "route.h"
#include "route_calc.h"
#include "route_cli.h"
#include "route_pub.h"
#include "route_relay.h"
#include "route_static.h"

route_local_t *g_route_local = NULL;

/* route_static 表：用户手动配置的静态路由 */
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

/* route_batch 表：批量路由配置（name 为主键，存储 batch 参数用于重启恢复） */
static const db_column_def_t ROUTE_BATCH_COLS[] = {
    {"name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY | DB_COL_NOT_NULL, NULL},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"start_addr", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"count", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"nexthop", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
};

static const db_table_def_t ROUTE_BATCH_TABLE = {
    .table_name = "route_batch",
    .cols = ROUTE_BATCH_COLS,
    .num_cols = G_N_ELEMENTS(ROUTE_BATCH_COLS),
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

static void route_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB) < 0)
    {
        LOG_ERROR("Failed to connect to DB module");
    }

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI) != 0)
    {
        LOG_ERROR("Failed to connect to CLI module");
    }

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_IF, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_IF) != 0)
    {
        LOG_WARN("Failed to connect to IF module (interface names will show as ifindex)");
    }

    LOG_INFO("Connected to DB, CLI, IF");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留
// ============================================================================

static void route_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

int route_add_and_notify(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                         uint32_t protocol, const net_addr_t *source_addr, const net_addr_t *nexthop_addr,
                         const net_addr_t *os_nexthop_addr, int32_t metric, int32_t preference, uint32_t out_ifindex)
{
    if (!g_route_local || !g_route_local->rib || !prefix_addr || !source_addr || !nexthop_addr)
    {
        return ERRCODE_FAIL;
    }

    int ret = route_rib_add(g_route_local->rib, vrf_id, afi, prefix_addr, prefix_len, protocol, source_addr,
                            nexthop_addr, metric, preference, out_ifindex);
    if (ret < 0)
    {
        return ret;
    }

    const route_head_t *head = route_rib_lookup_head(g_route_local->rib, vrf_id, afi, prefix_addr, prefix_len);
    if (!head)
    {
        return ret;
    }

    const route_path_t *path = route_rib_lookup_path(head, protocol, source_addr);
    if (!path)
    {
        return ret;
    }

    /* 普通路径默认不走“迭代回推”通道 */
    route_path_t *mut = (route_path_t *)path;
    mut->iter_required = 0u;
    mut->iter_exported = 0u;
    mut->iter_dirty = 0u;
    mut->iter_owner_module_id = 0u;
    mut->nh_state = ROUTE_NH_STATE_UNKNOWN;
    mut->os_nexthop = os_nexthop_addr ? *os_nexthop_addr : *nexthop_addr;

    /*
     * 始终触发优选计算与 OS 同步。
     * route_calc 内部会自行判断是否存在订阅者并决定是否发送通知。
     */
    route_calc_on_path_add(head);

    return ret;
}

// ============================================================================
// Phase 3: MODULE_READY — 初始化 DB 并恢复 RIB
// ============================================================================

static void route_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Initializing Route database");

    int ret = db_rpc_create_table_from_def(ctx, &ROUTE_STATIC_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route table creation failed: route_static");
        send_phase_response(ctx, msg, ERRCODE_SUCCESS);
        return;
    }

    ret = db_rpc_create_table_from_def(ctx, &ROUTE_BATCH_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route table creation failed: route_batch");
    }

    /* 从 DB 恢复静态路由到候选表（迭代可达时自动写入 RIB） */
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

            /* DB 边界：字符串→二进制，写入候选静态路由表（nexthop 可达时自动写 RIB） */
            net_addr_t prefix_addr, nexthop_addr;
            if (net_addr_from_str(prefix, &prefix_addr) != 0 || net_addr_from_str(nexthop, &nexthop_addr) != 0)
            {
                LOG_WARN("Route DB restore: invalid address prefix='%s' nexthop='%s'", prefix, nexthop);
                continue;
            }
            route_static_add(vrf_id, afi, &prefix_addr, prefix_len, &nexthop_addr, metric, preference);
        }
        LOG_INFO("Restored %u static routes from DB to candidate table", result->num_rows);
        db_result_free(result);
    }

    /* 从 DB 恢复 batch 路由到内存 RIB */
    route_batch_restore_from_db();

    /* 静态/直连等变化可能影响“迭代路径”可达性，恢复结束后做一次全量重算 */
    route_recompute_iter_paths();

    LOG_INFO("Route database tables ready");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void route_on_shutdown(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = route_local_ipc_ctx();
    LOG_INFO("Shutting down Route module...");

    if (g_route_local)
    {
        g_route_local->running = 0;

        route_calc_cleanup();

        route_static_cleanup();

        route_relay_cleanup();

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

static void handle_subscribe(dev_ipc_message_t *msg)
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
                route_calc_pub_dump(msg->src_module_id, protocol, vrf_id, msg->request_id);
            }
            else
            {
                route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
                ack->result = ERRCODE_SUCCESS;
                dev_ipc_message_t *resp =
                    dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id, msg->request_id,
                                           ack, sizeof(route_msg_ack_t), g_free);
                dev_ipc_send_response(route_local_ipc_ctx(), resp);
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
        route_calc_pub_dump(msg->src_module_id, protocol, vrf_id, msg->request_id);
    }
    else
    {
        route_msg_ack_t *ack = (route_msg_ack_t *)g_malloc(sizeof(route_msg_ack_t));
        ack->result = ERRCODE_SUCCESS;
        dev_ipc_message_t *resp = dev_ipc_message_create(ROUTE_MSG_TYPE_ACK, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                         msg->request_id, ack, sizeof(route_msg_ack_t), g_free);
        dev_ipc_send_response(route_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }

    dev_ipc_message_free(msg);
}

static void handle_unsubscribe(dev_ipc_message_t *msg)
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
    dev_ipc_send_response(route_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

static void route_on_path_del(const route_head_t *head, const route_path_t *path, void *userdata)
{
    (void)userdata;

    /* 触发优选重算：路径仍在 RIB 中，calc 跳过此路径选次优，同步 OS 并通知订阅者 */
    route_calc_on_path_del(head, path);
}

static void handle_inject(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_msg_entry_t))
    {
        LOG_WARN("ROUTE_INJECT payload too short: %u", msg->payload_len);
        dev_ipc_message_free(msg);
        return;
    }

    const route_msg_entry_t *entry = (const route_msg_entry_t *)msg->payload;
    if (entry->is_withdraw)
    {
        /* 先通过 route_calc 重算（在回调中同步 OS），再从 RIB 移除 */
        int ret = route_rib_del(g_route_local->rib, entry->vrf_id, entry->afi, &entry->prefix_addr, entry->prefix_len,
                                entry->protocol, &entry->source_addr, route_on_path_del, NULL);
        LOG_DEBUG("ROUTE_INJECT withdraw: vrf=%u afi=%u pfxlen=%u proto=%u ret=%d", entry->vrf_id, entry->afi,
                  entry->prefix_len, entry->protocol, ret);
        route_recompute_iter_paths();
    }
    else
    {
        /* 写入 RIB，route_add_and_notify 末尾触发 route_calc 优选并下发 OS */
        (void)route_add_and_notify(entry->vrf_id, entry->afi, &entry->prefix_addr, entry->prefix_len, entry->protocol,
                                   &entry->source_addr, &entry->nexthop_addr, &entry->nexthop_addr, entry->metric,
                                   entry->preference, entry->out_ifindex);
        route_recompute_iter_paths();
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void route_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    if (msg == NULL)
    {
        return;
    }

    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            route_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            route_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            route_on_ready(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            route_on_shutdown(msg);
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
            handle_subscribe(msg);
            return;

        case ROUTE_MSG_TYPE_UNSUBSCRIBE:
            handle_unsubscribe(msg);
            return;

        case ROUTE_MSG_TYPE_INJECT:
            handle_inject(msg);
            return;
        case ROUTE_MSG_TYPE_NH_REGISTER:
            route_relay_handle_nh_register(msg);
            return;
        case ROUTE_MSG_TYPE_NH_UNREGISTER:
            route_relay_handle_nh_unregister(msg);
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

    route_static_init();

    route_calc_init();

    return 0;
}
