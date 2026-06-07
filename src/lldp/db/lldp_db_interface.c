/**
 * @file   lldp_db_interface.c
 * @brief  LLDP interface 表
 * @author jhb
 * @date   2026/06/07
 */
#include <string.h>

#include "errcode.h"
#include "lldp_db_internal.h"
#include "work/lldp_worker.h"

static const db_column_def_t LLDP_IF_COLS[] = {
    {"ifname", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"enabled", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
    {"admin_status", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "3"},
    {"tx_interval_sec", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"hold_multiplier", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"port_desc", DB_TYPE_TEXT, 0, "''"},
};

const db_table_def_t LLDP_IF_TABLE = {
    .table_name = LLDP_TABLE_INTERFACE,
    .cols = LLDP_IF_COLS,
    .num_cols = G_N_ELEMENTS(LLDP_IF_COLS),
};

int lldp_db_set_interface(const char *ifname, const lldp_if_cfg_t *cfg)
{
    if (!ifname || ifname[0] == '\0' || !cfg)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    lldp_db_if_pk(&pk, ifname);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, LLDP_TABLE_INTERFACE, &pk.filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        return ERRCODE_FAIL;
    }

    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_INT("enabled", cfg->enabled ? 1 : 0),
            DB_COL_INT("admin_status", (int64_t)cfg->admin_status),
            DB_COL_INT("tx_interval_sec", (int64_t)cfg->tx_interval_sec),
            DB_COL_INT("hold_multiplier", (int64_t)cfg->hold_multiplier),
            DB_COL_TEXT("port_desc", cfg->port_desc),
        };
        int rows = db_rpc_update_cols(ctx, LLDP_TABLE_INTERFACE, &pk.filter, cols, G_N_ELEMENTS(cols));
        db_filter_clear(&pk);
        return (rows >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    db_filter_clear(&pk);

    db_col_t cols[] = {
        DB_COL_TEXT("ifname", ifname),
        DB_COL_INT("enabled", cfg->enabled ? 1 : 0),
        DB_COL_INT("admin_status", (int64_t)cfg->admin_status),
        DB_COL_INT("tx_interval_sec", (int64_t)cfg->tx_interval_sec),
        DB_COL_INT("hold_multiplier", (int64_t)cfg->hold_multiplier),
        DB_COL_TEXT("port_desc", cfg->port_desc),
    };
    return db_rpc_insert_cols(ctx, LLDP_TABLE_INTERFACE, cols, G_N_ELEMENTS(cols));
}

int lldp_db_del_interface(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    lldp_db_if_pk(&pk, ifname);
    int rc = db_rpc_delete(ctx, LLDP_TABLE_INTERFACE, &pk.filter);
    db_filter_clear(&pk);
    return (rc >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int lldp_db_get_interface(const char *ifname, lldp_if_cfg_t *cfg_out)
{
    if (!ifname || !cfg_out)
    {
        return ERRCODE_FAIL;
    }
    memset(cfg_out, 0, sizeof(*cfg_out));

    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    lldp_db_if_pk(&pk, ifname);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, LLDP_TABLE_INTERFACE, NULL, 0, &pk.filter, &result);
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
    cfg_out->admin_status = (uint8_t)db_row_get_int(row, "admin_status", LLDP_IF_ADMIN_TX_RX);
    cfg_out->tx_interval_sec = (uint32_t)db_row_get_int(row, "tx_interval_sec", 0);
    cfg_out->hold_multiplier = (uint32_t)db_row_get_int(row, "hold_multiplier", 0);
    g_strlcpy(cfg_out->port_desc, db_row_get_text(row, "port_desc", ""), sizeof(cfg_out->port_desc));
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

void lldp_db_restore_interfaces(void)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, LLDP_TABLE_INTERFACE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ifname = db_row_get_text(row, "ifname", NULL);
        if (!ifname || ifname[0] == '\0')
        {
            continue;
        }
        lldp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = LLDP_APPLY_OP_IF_SET;
        g_strlcpy(apply.u.if_set.ifname, ifname, sizeof(apply.u.if_set.ifname));
        apply.u.if_set.enabled = db_row_get_int(row, "enabled", 0) ? 1u : 0u;
        apply.u.if_set.admin_status = (uint8_t)db_row_get_int(row, "admin_status", LLDP_IF_ADMIN_TX_RX);
        apply.u.if_set.tx_interval_sec = (uint32_t)db_row_get_int(row, "tx_interval_sec", 0);
        apply.u.if_set.hold_multiplier = (uint32_t)db_row_get_int(row, "hold_multiplier", 0);
        g_strlcpy(apply.u.if_set.port_desc, db_row_get_text(row, "port_desc", ""), sizeof(apply.u.if_set.port_desc));
        (void)lldp_worker_dispatch_apply(&apply);
    }

    db_result_free(result);
}
