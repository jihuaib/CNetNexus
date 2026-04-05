/**
 * @file   if_main.c
 * @brief  接口模块主入口，三阶段初始化和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "if_main.h"

#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if_bdr.h"
#include "if_cfg_apply.h"
#include "if_cli.h"
#include "if_event.h"
#include "if_pub.h"
#include "log.h"
#include "net_addr.h"
#include "path_utils.h"

if_local_t *g_if_local = NULL;

/**
 * @brief 启动时将映射表中的接口写入数据库
 */
static gboolean if_init_db_foreach(gpointer key, gpointer val, gpointer user_data)
{
    (void)key;
    (void)user_data;
    if_map_entry_t *e = (if_map_entry_t *)val;
    const char *logical_name = e->logical_name;

    /* loop 和 null0 接口不写入 DB（由 loop_create 或虚拟，不持久化） */
    if (strncmp(logical_name, "loop", 4) == 0 || strcmp(logical_name, "null0") == 0)
    {
        return FALSE;
    }

    db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(logical_name)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    gboolean exists = FALSE;
    int ret = db_rpc_exists(if_local_ipc_ctx(), "if_interface", &filter, &exists);
    db_value_free(&cond.value);

    if (ret == ERRCODE_SUCCESS && !exists)
    {
        db_record_t *rec = db_record_new();
        db_record_set_text(rec, "name", logical_name);
        db_record_set_text(rec, "ip_address", "");
        db_record_set_int(rec, "prefix_len", 0);
        db_record_set_text(rec, "ipv6_address", "");
        db_record_set_int(rec, "ipv6_prefix_len", 0);
        db_record_set_int(rec, "shutdown", 0);
        db_rpc_insert_record(if_local_ipc_ctx(), "if_interface", rec);
        db_record_free(rec);
        LOG_INFO("Interface %s written to database", logical_name);
    }
    else if (ret == ERRCODE_SUCCESS && exists)
    {
        LOG_DEBUG("Interface %s already in database, keeping existing config", logical_name);
    }
    return FALSE;
}

static void if_init_db(void)
{
    if (!g_if_local->interface_map.all_entries)
    {
        return;
    }
    g_tree_foreach(g_if_local->interface_map.all_entries, if_init_db_foreach, NULL);
}

/**
 * @brief 从数据库恢复接口配置到内存态
 *
 * 复用 if_cfg_apply_* 流程，与 CLI 配置路径完全一致。
 */
static void if_db_restore(void)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, "if_interface", NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        LOG_WARN("IF: Failed to restore database config");
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *name = db_row_get_text(row, "name", NULL);
        const char *ip4_str = db_row_get_text(row, "ip_address", NULL);
        int64_t prefix4_len = db_row_get_int(row, "prefix_len", 0);
        const char *ip6_str = db_row_get_text(row, "ipv6_address", NULL);
        int64_t prefix6_len = db_row_get_int(row, "ipv6_prefix_len", 0);
        int64_t shutdown = db_row_get_int(row, "shutdown", 0);

        if (!name)
        {
            continue;
        }

        /* loop 接口：先确保内存条目和 OS 接口存在 */
        if (strncmp(name, "loop", 4) == 0 && name[4] >= '1' && name[4] <= '9')
        {
            uint32_t loop_id = (uint32_t)atoi(name + 4);
            if (loop_id >= 1 && loop_id <= 1024)
            {
                if_cfg_loop_ensure(loop_id);
            }
        }

        /* 恢复 IPv4（非空时） */
        if (ip4_str && ip4_str[0] != '\0')
        {
            net_prefix_t pfx;
            memset(&pfx, 0, sizeof(pfx));
            if (net_addr_from_str(ip4_str, &pfx.addr) == 0 && pfx.addr.family == AF_INET)
            {
                pfx.prefix_len = (uint8_t)prefix4_len;
                if (if_cfg_apply_ip(FALSE, name, &pfx) != ERRCODE_SUCCESS)
                {
                    char pfx_buf[70];
                    net_prefix_to_str(&pfx, pfx_buf, sizeof(pfx_buf));
                    LOG_WARN("IF: skip invalid restored address %s on %s", pfx_buf, name);
                }
            }
            else
            {
                LOG_WARN("IF: skip invalid restored IPv4 address %s on %s", ip4_str, name);
            }
        }
        /* 恢复 IPv6（非空时） */
        if (ip6_str && ip6_str[0] != '\0')
        {
            net_prefix_t pfx;
            memset(&pfx, 0, sizeof(pfx));
            if (net_addr_from_str(ip6_str, &pfx.addr) == 0 && pfx.addr.family == AF_INET6)
            {
                pfx.prefix_len = (uint8_t)prefix6_len;
                if (if_cfg_apply_ip(FALSE, name, &pfx) != ERRCODE_SUCCESS)
                {
                    char pfx_buf[70];
                    net_prefix_to_str(&pfx, pfx_buf, sizeof(pfx_buf));
                    LOG_WARN("IF: skip invalid restored address %s on %s", pfx_buf, name);
                }
            }
            else
            {
                LOG_WARN("IF: skip invalid restored IPv6 address %s on %s", ip6_str, name);
            }
        }

        /* 恢复 shutdown 状态 */
        if (shutdown)
        {
            if_cfg_apply_shutdown(FALSE, name); /* shutdown */
        }
    }

    db_result_free(result);
    LOG_INFO("IF: Database config restoration complete");
}

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

static void send_if_ack(dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    if (!ctx || !msg)
    {
        return;
    }

    if_msg_ack_t *ack = (if_msg_ack_t *)g_malloc(sizeof(if_msg_ack_t));
    if (!ack)
    {
        dev_ipc_message_free(msg);
        return;
    }
    ack->result = result;

    dev_ipc_message_t *resp = dev_ipc_message_create(IF_MSG_TYPE_ACK, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, ack, sizeof(if_msg_ack_t), g_free);
    if (!resp)
    {
        g_free(ack);
        dev_ipc_message_free(msg);
        return;
    }

    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

static void handle_if_subscribe(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(if_subscribe_req_t))
    {
        LOG_WARN("IF: subscribe payload invalid, len=%u", msg->payload_len);
        send_if_ack(msg, ERRCODE_FAIL);
        return;
    }

    const if_subscribe_req_t *req = (const if_subscribe_req_t *)msg->payload;
    if (req->if_type_mask == 0 || req->event_mask == 0)
    {
        LOG_WARN("IF: subscribe request invalid: type_mask=0x%08X event_mask=0x%08X", req->if_type_mask,
                 req->event_mask);
        send_if_ack(msg, ERRCODE_FAIL);
        return;
    }

    for (GList *l = g_if_local->subscribers; l; l = l->next)
    {
        if_subscriber_t *sub = (if_subscriber_t *)l->data;
        if (sub->module_id == msg->src_module_id && sub->if_type_mask == req->if_type_mask &&
            sub->event_mask == req->event_mask)
        {
            LOG_DEBUG("IF: duplicate subscribe ignored: module=0x%08X type=0x%08X event=0x%08X", msg->src_module_id,
                      req->if_type_mask, req->event_mask);
            send_if_ack(msg, ERRCODE_SUCCESS);
            return;
        }
    }

    if_subscriber_t *sub = (if_subscriber_t *)g_malloc0(sizeof(if_subscriber_t));
    if (!sub)
    {
        send_if_ack(msg, ERRCODE_FAIL);
        return;
    }
    sub->module_id = msg->src_module_id;
    sub->if_type_mask = req->if_type_mask;
    sub->event_mask = req->event_mask;
    g_if_local->subscribers = g_list_append(g_if_local->subscribers, sub);

    LOG_INFO("IF: module 0x%08X subscribed: type=0x%08X event=0x%08X", msg->src_module_id, req->if_type_mask,
             req->event_mask);

    send_if_ack(msg, ERRCODE_SUCCESS);
}

/* 收集 all_entries 条目到 GArray */
static gboolean collect_intf_map_foreach(gpointer key, gpointer val, gpointer user_data)
{
    (void)key;
    if_map_entry_t *e = (if_map_entry_t *)val;
    GArray *arr = (GArray *)user_data;

    if (e->ifindex == 0)
    {
        return FALSE; /* 跳过无 OS 接口的虚拟条目（null0 等） */
    }

    if_intf_map_item_t item;
    item.ifindex = e->ifindex;
    snprintf(item.logical_name, sizeof(item.logical_name), "%s", e->logical_name);
    g_array_append_val(arr, item);
    return FALSE;
}

static void handle_if_get_intf_map(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(if_intf_map_item_t));

    if (g_if_local && g_if_local->interface_map.all_entries)
    {
        g_tree_foreach(g_if_local->interface_map.all_entries, collect_intf_map_foreach, arr);
    }

    uint32_t count = arr->len;
    size_t payload_size = sizeof(if_intf_map_resp_t) + (count > 0 ? (count - 1) * sizeof(if_intf_map_item_t) : 0);
    if_intf_map_resp_t *resp_payload = (if_intf_map_resp_t *)g_malloc0(payload_size);
    resp_payload->result = ERRCODE_SUCCESS;
    resp_payload->count = count;
    if (count > 0)
    {
        memcpy(resp_payload->items, arr->data, count * sizeof(if_intf_map_item_t));
    }
    g_array_free(arr, TRUE);

    dev_ipc_message_t *resp = dev_ipc_message_create(IF_MSG_TYPE_GET_INTF_MAP, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, resp_payload, (uint32_t)payload_size, g_free);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

static void handle_if_unsubscribe(dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(if_subscribe_req_t))
    {
        LOG_WARN("IF: unsubscribe payload invalid, len=%u", msg->payload_len);
        send_if_ack(msg, ERRCODE_FAIL);
        return;
    }

    const if_subscribe_req_t *req = (const if_subscribe_req_t *)msg->payload;
    uint32_t type_mask = req->if_type_mask;
    uint32_t event_mask = req->event_mask;

    int removed = 0;
    GList *l = g_if_local->subscribers;
    while (l)
    {
        if_subscriber_t *sub = (if_subscriber_t *)l->data;
        GList *next = l->next;

        if (sub->module_id == msg->src_module_id)
        {
            gboolean match_exact = (sub->if_type_mask == type_mask && sub->event_mask == event_mask);
            gboolean clear_all = (type_mask == 0 && event_mask == 0);
            if (clear_all || match_exact)
            {
                g_if_local->subscribers = g_list_delete_link(g_if_local->subscribers, l);
                g_free(sub);
                removed++;
            }
        }
        l = next;
    }

    LOG_INFO("IF: module 0x%08X unsubscribed, removed=%d (type=0x%08X event=0x%08X)", msg->src_module_id, removed,
             type_mask, event_mask);

    send_if_ack(msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 1: MODULE_START - Establishing IPC connections到 CFG
// ============================================================================

static void if_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");

    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    dev_ipc_connect(ctx, DEV_MODULE_ID_ROUTE, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_ROUTE);

    LOG_INFO("Connected to CFG, DB and ROUTE");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void if_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 将接口写入数据库
// ============================================================================

/* if_interface 表结构定义 */
static const db_column_def_t IF_INTERFACE_COLS[] = {
    {"name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},           {"ip_address", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},      {"ipv6_address", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"ipv6_prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"}, {"shutdown", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

static const db_table_def_t IF_INTERFACE_TABLE = {
    .table_name = "if_interface",
    .cols = IF_INTERFACE_COLS,
    .num_cols = G_N_ELEMENTS(IF_INTERFACE_COLS),
};

static void if_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Initializing IF database");

    /* 建表（IF NOT EXISTS，幂等操作） */
    int ret = db_rpc_create_table_from_def(ctx, &IF_INTERFACE_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF table creation failed, skipping database initialization");
        send_phase_response(ctx, msg, ERRCODE_SUCCESS);
        return;
    }

    /* 将新接口写入数据库（已有条目保留） */
    if_init_db();

    /* 从数据库恢复配置到内存态 */
    if_db_restore();

    LOG_INFO("IF module ready");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void if_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            if_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            if_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            if_on_ready(msg);
            return;

        /* ---- CLI 消息 ---- */
        case CLI_MSG_TYPE:
            LOG_DEBUG("Received CLI command message");
            if_cli_handle_message(msg);
            break;

        case CLI_MSG_TYPE_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            if_cli_handle_continue(msg);
            break;

        case CLI_MSG_TYPE_QUERY_CANDIDATES:
            if_cli_handle_query_candidates(msg);
            return;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            if_bdr_show_config(msg);
            break;

        case IF_MSG_TYPE_SUBSCRIBE:
            handle_if_subscribe(msg);
            return;

        case IF_MSG_TYPE_UNSUBSCRIBE:
            handle_if_unsubscribe(msg);
            return;

        case IF_MSG_TYPE_GET_INTF_MAP:
            handle_if_get_intf_map(msg);
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

int if_module_init(void)
{
    LOG_INFO("Module initialization");

    /* 创建 IPC 上下文 */
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_IF, "if", DEV_MODULE_PORT_IF, if_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    /* 初始化本地状态（原 if_on_start 逻辑） */
    g_if_local = g_malloc0(sizeof(if_local_t));
    g_if_local->dev_ipc_ctx = ctx;

    /* 初始化接口映射
     * 查找顺序：
     *   1. 生产环境：NN_WORK_DIR/resources/if/if_map.conf.gns3
     *   2. 开发环境：可执行文件相对路径 ../src 和 ../../src */
    char *if_map_path = NULL;

    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir != NULL)
    {
        if_map_path = g_build_filename(work_dir, "resources", "if", "if_map.conf.gns3", NULL);
        LOG_INFO("Using GNS3 interface mapping: %s", if_map_path);
    }
    else
    {
        char exe_dir[PATH_MAX];
        if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
        {
            LOG_ERROR("Failed to get exe directory");
            return -1;
        }

        if_map_path = g_build_filename(exe_dir, "..", "..", "src", "if", "resources", "if_map.conf.local", NULL);
        LOG_INFO("Using local interface mapping: %s", if_map_path);
    }

    if (if_map_init(if_map_path) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Failed to load interface mapping");
    }
    g_free(if_map_path);
    return 0;
}

void if_module_cleanup(void)
{
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

    if (!g_if_local)
    {
        return;
    }

    if_cli_cleanup_state();

    g_list_free_full(g_if_local->subscribers, g_free);
    g_if_local->subscribers = NULL;

    if (g_if_local->interface_map.all_entries)
    {
        g_tree_destroy(g_if_local->interface_map.all_entries);
        g_if_local->interface_map.all_entries = NULL;
    }

    g_free(g_if_local);
    g_if_local = NULL;
}
