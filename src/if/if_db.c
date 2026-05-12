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
#include "if.h"
#include "if_main.h"
#include "log.h"
#include "route.h"
#include "vrf.h"
#include "work/if_netlink.h"
#include "work/if_worker.h"

/* DB 恢复时单行的快照——挂起重试需要值语义，所以这里全是定长字段 */
typedef struct
{
    char name[IF_LOGICAL_NAME_MAX];
    char vrf_name[IF_VRF_NAME_MAX];
    char ip4[64];
    char ip6[64];
    uint8_t prefix4_len;
    uint8_t prefix6_len;
    uint8_t shutdown;
} if_db_row_snapshot_t;

static int if_db_apply_row(void *item, void *ctx);

static const db_column_def_t IF_INTERFACE_COLS[] = {
    {"name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},           {"ip_address", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},      {"ipv6_address", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"ipv6_prefix_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"}, {"shutdown", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"vrf_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
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

    /* loop 接口的 DB 行由 CLI 创建路径或 if_db_restore 维护，init 阶段不主动补 */
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

/* 应用一行恢复快照；幂等，可由首次 restore 或 pending_resolve 重复调用 */
static int if_db_apply_row(void *item, void *ctx)
{
    (void)ctx;
    const if_db_row_snapshot_t *r = (const if_db_row_snapshot_t *)item;
    const char *name = r->name;

    /* 依赖 VRF 时先通过 worker 查询其独占维护的 VRF cache，避免 IPC 线程跨线程读 cache。 */
    uint32_t vrf_id = ROUTE_VRF_DEFAULT;
    if (r->vrf_name[0] != '\0' && if_worker_resolve_vrf_id_by_name(r->vrf_name, &vrf_id) != ERRCODE_SUCCESS)
    {
        LOG_INFO("IF: %s waits on vrf '%s' (cache not yet synced), parked", name, r->vrf_name);
        pending_park(g_if_local->pending, IF_DEP_VRF, g_str_hash(r->vrf_name), r, sizeof(*r), NULL, if_db_apply_row,
                     NULL);
        return PENDING_AGAIN;
    }

    /* loop 接口创建恢复（无依赖，先做） */
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

    /* VRF 绑定：cache 已 OK 也可能 kernel 设备尚未创建（极少见，软重启时通常残留）。
     * worker 返回 DEP_MISSING 也挂起。 */
    if (r->vrf_name[0] != '\0')
    {
        if_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = IF_APPLY_OP_VRF_BIND;
        g_strlcpy(apply.u.vrf_bind.ifname, name, sizeof(apply.u.vrf_bind.ifname));
        g_strlcpy(apply.u.vrf_bind.vrf_name, r->vrf_name, sizeof(apply.u.vrf_bind.vrf_name));
        int rc = if_worker_dispatch_apply(&apply);
        if (rc == ERRCODE_DEP_MISSING)
        {
            LOG_INFO("IF: %s waits on vrf '%s' (OS device absent), parked", name, r->vrf_name);
            pending_park(g_if_local->pending, IF_DEP_VRF, g_str_hash(r->vrf_name), r, sizeof(*r), NULL, if_db_apply_row,
                         NULL);
            return PENDING_AGAIN;
        }
    }

    /* 恢复 IPv4 */
    if (r->ip4[0] != '\0')
    {
        if_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = IF_APPLY_OP_IP_SET;
        g_strlcpy(apply.u.ip_set.ifname, name, sizeof(apply.u.ip_set.ifname));
        apply.u.ip_set.is_no = 0;
        if (net_addr_from_str(r->ip4, &apply.u.ip_set.prefix.addr) == 0 && apply.u.ip_set.prefix.addr.family == AF_INET)
        {
            apply.u.ip_set.prefix.prefix_len = r->prefix4_len;
            (void)if_worker_dispatch_apply(&apply);
        }
    }

    /* 恢复 IPv6 */
    if (r->ip6[0] != '\0')
    {
        if_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = IF_APPLY_OP_IP_SET;
        g_strlcpy(apply.u.ip_set.ifname, name, sizeof(apply.u.ip_set.ifname));
        apply.u.ip_set.is_no = 0;
        if (net_addr_from_str(r->ip6, &apply.u.ip_set.prefix.addr) == 0 &&
            apply.u.ip_set.prefix.addr.family == AF_INET6)
        {
            apply.u.ip_set.prefix.prefix_len = r->prefix6_len;
            (void)if_worker_dispatch_apply(&apply);
        }
    }

    /* 恢复 shutdown */
    if_apply_cmd_t apply_sht;
    memset(&apply_sht, 0, sizeof(apply_sht));
    apply_sht.op = IF_APPLY_OP_SHUTDOWN_SET;
    g_strlcpy(apply_sht.u.shutdown_set.ifname, name, sizeof(apply_sht.u.shutdown_set.ifname));
    apply_sht.u.shutdown_set.is_no = !r->shutdown;
    (void)if_worker_dispatch_apply(&apply_sht);

    return PENDING_DONE;
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
        if (!name || name[0] == '\0')
        {
            continue;
        }

        if_db_row_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        g_strlcpy(snap.name, name, sizeof(snap.name));
        g_strlcpy(snap.vrf_name, db_row_get_text(row, "vrf_name", ""), sizeof(snap.vrf_name));
        g_strlcpy(snap.ip4, db_row_get_text(row, "ip_address", ""), sizeof(snap.ip4));
        g_strlcpy(snap.ip6, db_row_get_text(row, "ipv6_address", ""), sizeof(snap.ip6));
        snap.prefix4_len = (uint8_t)db_row_get_int(row, "prefix_len", 0);
        snap.prefix6_len = (uint8_t)db_row_get_int(row, "ipv6_prefix_len", 0);
        snap.shutdown = (uint8_t)(db_row_get_int(row, "shutdown", 0) ? 1 : 0);

        /* 乐观下发；apply_row 内部若 worker 返回 ERRCODE_DEP_MISSING 会自挂起 */
        (void)if_db_apply_row(&snap, NULL);
    }

    db_result_free(result);
    LOG_INFO("IF: Database config restoration complete (pending=%zu)", pending_count(g_if_local->pending));
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
        db_record_set_text(rec, "vrf_name", "");
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

int if_db_update_vrf(const char *ifname, const char *vrf_name)
{
    if (!ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }
    if_db_ensure_record(ifname);

    db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "vrf_name", vrf_name ? vrf_name : "");
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

int if_db_del_record(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    db_filter_init(&pk);
    db_filter_add_text(&pk, "name", ifname);

    int rows = db_rpc_delete(if_local_ipc_ctx(), "if_interface", &pk.filter);
    db_filter_clear(&pk);
    return (rows < 0) ? ERRCODE_FAIL : ERRCODE_SUCCESS;
}
