/**
 * @file   ldp_db_interface.c
 * @brief  LDP interface 表：schema、CRUD、启动恢复
 * @author jhb
 * @date   2026/05/05
 */

#include <string.h>

#include "errcode.h"
#include "ldp_db_internal.h"

static const db_column_def_t LDP_IF_COLS[] = {
    {"ifname", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"enabled", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"hello_interval_ms", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"hold_time_ms", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

const db_table_def_t LDP_IF_TABLE = {
    .table_name = LDP_TABLE_INTERFACE,
    .cols = LDP_IF_COLS,
    .num_cols = G_N_ELEMENTS(LDP_IF_COLS),
};

int ldp_db_set_interface(const char *ifname, const ldp_if_cfg_t *cfg)
{
    if (!ifname || ifname[0] == '\0' || !cfg)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ldp_db_if_pk(&pk, ifname);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, LDP_TABLE_INTERFACE, &pk.filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        return ERRCODE_FAIL;
    }

    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_INT("enabled", cfg->enabled ? 1 : 0),
            DB_COL_INT("hello_interval_ms", (int64_t)cfg->hello_interval_ms),
            DB_COL_INT("hold_time_ms", (int64_t)cfg->hold_time_ms),
        };
        int rows = db_rpc_update_cols(ctx, LDP_TABLE_INTERFACE, &pk.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&pk);
        return (rows >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    db_filter_clear(&pk);

    db_col_t cols[] = {
        DB_COL_TEXT("ifname", ifname),
        DB_COL_INT("enabled", cfg->enabled ? 1 : 0),
        DB_COL_INT("hello_interval_ms", (int64_t)cfg->hello_interval_ms),
        DB_COL_INT("hold_time_ms", (int64_t)cfg->hold_time_ms),
    };
    return db_rpc_insert_cols(ctx, LDP_TABLE_INTERFACE, cols, G_N_ELEMENTS(cols));
}

int ldp_db_del_interface(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ldp_db_if_pk(&pk, ifname);
    int rc = db_rpc_delete(ctx, LDP_TABLE_INTERFACE, &pk.filter);
    db_filter_clear(&pk);
    return (rc >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int ldp_db_get_interface(const char *ifname, ldp_if_cfg_t *cfg_out)
{
    if (!ifname || !cfg_out)
    {
        return ERRCODE_FAIL;
    }
    memset(cfg_out, 0, sizeof(*cfg_out));

    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ldp_db_if_pk(&pk, ifname);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, LDP_TABLE_INTERFACE, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS || !result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    db_row_t *row = result->rows[0];
    g_strlcpy(cfg_out->ifname, db_row_get_text(row, "ifname", ""), sizeof(cfg_out->ifname));
    cfg_out->enabled = (uint8_t)(db_row_get_int(row, "enabled", 0) ? 1 : 0);
    cfg_out->hello_interval_ms = (uint32_t)db_row_get_int(row, "hello_interval_ms", 0);
    cfg_out->hold_time_ms = (uint32_t)db_row_get_int(row, "hold_time_ms", 0);
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

void ldp_db_restore_interfaces(void)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, LDP_TABLE_INTERFACE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }
    /* M1：仅占位，后续 M2 把使能接口推送给 worker */
    db_result_free(result);
}
