/**
 * @file   if_db.c
 * @brief  接口模块数据库操作实现
 * @author jhb
 * @date   2026/04/21
 */
#include "if_db.h"

#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "errcode.h"
#include "if_main.h"
#include "log.h"
#include "work/if.h"
#include "work/if_worker.h"

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

static gboolean if_init_db_foreach(gpointer key, gpointer val, gpointer user_data)
{
    (void)key;
    (void)user_data;
    if_map_entry_t *e = (if_map_entry_t *)val;
    const char *logical_name = e->logical_name;

    /* loop 接口不写入 DB（由 loop_create 运行时创建，不持久化） */
    if (strncmp(logical_name, "loop", 4) == 0)
    {
        return FALSE;
    }

    if_db_ensure_record(logical_name);
    return FALSE;
}

int if_db_init(void)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    if (db_rpc_create_table_from_def(ctx, &IF_INTERFACE_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF table creation failed, skipping database initialization");
        return ERRCODE_FAIL;
    }

    if (g_if_work_local && g_if_work_local->interface_map.all_entries)
    {
        g_tree_foreach(g_if_work_local->interface_map.all_entries, if_init_db_foreach, NULL);
    }

    return ERRCODE_SUCCESS;
}

int if_db_restore(void)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, "if_interface", NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        LOG_WARN("IF: Failed to restore database config");
        return ERRCODE_FAIL;
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

        if (!name || name[0] == '\0')
        {
            continue;
        }

        /* loop 接口创建恢复 */
        if (strncmp(name, "loop", 4) == 0 && name[4] >= '1' && name[4] <= '9')
        {
            uint32_t loop_id = (uint32_t)atoi(name + 4);
            if (loop_id >= 1 && loop_id <= 1024)
            {
                if_apply_cmd_t apply;
                memset(&apply, 0, sizeof(apply));
                apply.op = IF_APPLY_OP_LOOP_CREATE;
                apply.u.loop_create.loop_id = loop_id;
                (void)if_worker_dispatch_apply(&apply);
            }
        }

        /* 恢复 IPv4 */
        if (ip4_str && ip4_str[0] != '\0')
        {
            if_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.op = IF_APPLY_OP_IP_SET;
            g_strlcpy(apply.u.ip_set.ifname, name, sizeof(apply.u.ip_set.ifname));
            apply.u.ip_set.is_no = 0;
            if (net_addr_from_str(ip4_str, &apply.u.ip_set.prefix.addr) == 0 &&
                apply.u.ip_set.prefix.addr.family == AF_INET)
            {
                apply.u.ip_set.prefix.prefix_len = (uint8_t)prefix4_len;
                (void)if_worker_dispatch_apply(&apply);
            }
        }

        /* 恢复 IPv6 */
        if (ip6_str && ip6_str[0] != '\0')
        {
            if_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.op = IF_APPLY_OP_IP_SET;
            g_strlcpy(apply.u.ip_set.ifname, name, sizeof(apply.u.ip_set.ifname));
            apply.u.ip_set.is_no = 0;
            if (net_addr_from_str(ip6_str, &apply.u.ip_set.prefix.addr) == 0 &&
                apply.u.ip_set.prefix.addr.family == AF_INET6)
            {
                apply.u.ip_set.prefix.prefix_len = (uint8_t)prefix6_len;
                (void)if_worker_dispatch_apply(&apply);
            }
        }

        /* 恢复 shutdown */
        if_apply_cmd_t apply_sht;
        memset(&apply_sht, 0, sizeof(apply_sht));
        apply_sht.op = IF_APPLY_OP_SHUTDOWN_SET;
        g_strlcpy(apply_sht.u.shutdown_set.ifname, name, sizeof(apply_sht.u.shutdown_set.ifname));
        apply_sht.u.shutdown_set.is_no = !shutdown;
        (void)if_worker_dispatch_apply(&apply_sht);
    }

    db_result_free(result);
    LOG_INFO("IF: Database config restoration complete");
    return ERRCODE_SUCCESS;
}

int if_db_ensure_record(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, "if_interface", &filter, &exists);
    db_value_free(&cond.value);

    if (ret == ERRCODE_SUCCESS && !exists)
    {
        db_record_t *rec = db_record_new();
        db_record_set_text(rec, "name", ifname);
        db_record_set_text(rec, "ip_address", "");
        db_record_set_int(rec, "prefix_len", 0);
        db_record_set_text(rec, "ipv6_address", "");
        db_record_set_int(rec, "ipv6_prefix_len", 0);
        db_record_set_int(rec, "shutdown", 0);
        db_rpc_insert_record(ctx, "if_interface", rec);
        db_record_free(rec);
    }
    return ERRCODE_SUCCESS;
}

int if_db_update_ip(const char *ifname, int is_ipv6, const char *ip_str, uint8_t prefix_len)
{
    if (!ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }
    if_db_ensure_record(ifname);

    db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_record_t *rec = db_record_new();

    if (is_ipv6)
    {
        db_record_set_text(rec, "ipv6_address", ip_str ? ip_str : "");
        db_record_set_int(rec, "ipv6_prefix_len", (int64_t)prefix_len);
    }
    else
    {
        db_record_set_text(rec, "ip_address", ip_str ? ip_str : "");
        db_record_set_int(rec, "prefix_len", (int64_t)prefix_len);
    }

    db_rpc_update_record(if_local_ipc_ctx(), "if_interface", rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);
    return ERRCODE_SUCCESS;
}

int if_db_update_shutdown(const char *ifname, int shutdown)
{
    if (!ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }
    if_db_ensure_record(ifname);

    db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "shutdown", shutdown ? 1 : 0);
    db_rpc_update_record(if_local_ipc_ctx(), "if_interface", rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);
    return ERRCODE_SUCCESS;
}
