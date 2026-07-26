/**
 * @file   lldp_db_protocol.c
 * @brief  LLDP protocol 表
 * @author jhb
 * @date   2026/06/07
 */
#include <string.h>

#include "errcode.h"
#include "lldp_db_internal.h"
#include "work/lldp_worker.h"

static const db_column_def_t LLDP_PROTO_COLS[] = {
    {"inst_id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
    {"admin_up", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"tx_interval_sec", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "30"},
    {"hold_multiplier", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "4"},
    {"reinit_delay_sec", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "2"},
    {"tx_delay_sec", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "2"},
};

const db_table_def_t LLDP_PROTO_TABLE = {
    .table_name = LLDP_TABLE_PROTOCOL,
    .cols = LLDP_PROTO_COLS,
    .num_cols = G_N_ELEMENTS(LLDP_PROTO_COLS),
};

static void lldp_db_proto_defaults(lldp_proto_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->tx_interval_sec = LLDP_DEFAULT_TX_INTERVAL_SEC;
    cfg->hold_multiplier = LLDP_DEFAULT_HOLD_MULTIPLIER;
    cfg->reinit_delay_sec = LLDP_DEFAULT_REINIT_DELAY_SEC;
    cfg->tx_delay_sec = LLDP_DEFAULT_TX_DELAY_SEC;
}

static gboolean lldp_db_proto_is_default(const lldp_proto_cfg_t *cfg)
{
    return cfg && cfg->admin_up == 0u && cfg->tx_interval_sec == LLDP_DEFAULT_TX_INTERVAL_SEC &&
           cfg->hold_multiplier == LLDP_DEFAULT_HOLD_MULTIPLIER &&
           cfg->reinit_delay_sec == LLDP_DEFAULT_REINIT_DELAY_SEC && cfg->tx_delay_sec == LLDP_DEFAULT_TX_DELAY_SEC;
}

static int lldp_db_prune_noncanonical_proto_rows(dev_ipc_context_t *ctx)
{
    db_condition_t condition = {
        .field_name = "inst_id",
        .op = DB_CMP_NE,
        .value = db_value_int((int64_t)LLDP_PROTOCOL_INST_ID),
    };
    db_filter_t filter = {.conditions = &condition, .num_conditions = 1};
    int rows = db_rpc_delete(ctx, LLDP_TABLE_PROTOCOL, &filter);
    db_value_free(&condition.value);
    return rows < 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
}

static int lldp_db_ensure_proto_row(dev_ipc_context_t *ctx)
{
    db_filter_builder_t pk;
    lldp_db_proto_pk(&pk);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, LLDP_TABLE_PROTOCOL, &pk.filter, &exists);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (exists)
    {
        return ERRCODE_SUCCESS;
    }

    db_col_t cols[] = {
        DB_COL_INT("inst_id", LLDP_PROTOCOL_INST_ID),
        DB_COL_INT("admin_up", 0),
        DB_COL_INT("tx_interval_sec", LLDP_DEFAULT_TX_INTERVAL_SEC),
        DB_COL_INT("hold_multiplier", LLDP_DEFAULT_HOLD_MULTIPLIER),
        DB_COL_INT("reinit_delay_sec", LLDP_DEFAULT_REINIT_DELAY_SEC),
        DB_COL_INT("tx_delay_sec", LLDP_DEFAULT_TX_DELAY_SEC),
    };
    return db_rpc_insert_cols(ctx, LLDP_TABLE_PROTOCOL, cols, G_N_ELEMENTS(cols));
}

int lldp_db_ensure_proto_marker(void)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    return ctx ? lldp_db_ensure_proto_row(ctx) : ERRCODE_FAIL;
}

static int lldp_db_read_proto_cfg(dev_ipc_context_t *ctx, lldp_proto_cfg_t *cfg_out, gboolean *exists_out)
{
    if (!ctx || !cfg_out)
    {
        return ERRCODE_FAIL;
    }

    lldp_db_proto_defaults(cfg_out);
    if (exists_out)
    {
        *exists_out = FALSE;
    }

    db_filter_builder_t pk;
    lldp_db_proto_pk(&pk);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, LLDP_TABLE_PROTOCOL, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    /* db_rpc_query 用 SUCCESS + result=NULL 表示合法空结果集。 */
    if (!result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_SUCCESS;
    }

    db_row_t *row = result->rows[0];
    cfg_out->admin_up = (uint8_t)(db_row_get_int(row, "admin_up", 0) ? 1 : 0);
    cfg_out->tx_interval_sec = (uint32_t)db_row_get_int(row, "tx_interval_sec", LLDP_DEFAULT_TX_INTERVAL_SEC);
    cfg_out->hold_multiplier = (uint32_t)db_row_get_int(row, "hold_multiplier", LLDP_DEFAULT_HOLD_MULTIPLIER);
    cfg_out->reinit_delay_sec = (uint32_t)db_row_get_int(row, "reinit_delay_sec", LLDP_DEFAULT_REINIT_DELAY_SEC);
    cfg_out->tx_delay_sec = (uint32_t)db_row_get_int(row, "tx_delay_sec", LLDP_DEFAULT_TX_DELAY_SEC);
    if (exists_out)
    {
        *exists_out = TRUE;
    }
    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int lldp_db_sync_proto_marker(void)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    if (lldp_db_prune_noncanonical_proto_rows(ctx) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (lldp_db_prune_implicit_default_interfaces() != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    lldp_proto_cfg_t cfg;
    gboolean proto_exists = FALSE;
    if (lldp_db_read_proto_cfg(ctx, &cfg, &proto_exists) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (proto_exists &&
        (cfg.reinit_delay_sec != LLDP_DEFAULT_REINIT_DELAY_SEC || cfg.tx_delay_sec != LLDP_DEFAULT_TX_DELAY_SEC))
    {
        /*
         * 这两个历史字段没有 CLI/BDR 表达，不能作为可回放配置保留。
         * 在 marker 判定前归一化，保证 DB 与 CFG 冷启动一致。
         */
        db_filter_builder_t pk;
        lldp_db_proto_pk(&pk);
        db_col_t defaults[] = {
            DB_COL_INT("reinit_delay_sec", LLDP_DEFAULT_REINIT_DELAY_SEC),
            DB_COL_INT("tx_delay_sec", LLDP_DEFAULT_TX_DELAY_SEC),
        };
        int rows = db_rpc_update_cols(ctx, LLDP_TABLE_PROTOCOL, &pk.filter, defaults, G_N_ELEMENTS(defaults));
        db_filter_clear(&pk);
        if (rows < 0)
        {
            return ERRCODE_FAIL;
        }
        cfg.reinit_delay_sec = LLDP_DEFAULT_REINIT_DELAY_SEC;
        cfg.tx_delay_sec = LLDP_DEFAULT_TX_DELAY_SEC;
    }

    gboolean interface_exists = FALSE;
    if (db_rpc_exists(ctx, LLDP_TABLE_INTERFACE, NULL, &interface_exists) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    const gboolean marker_required = !lldp_db_proto_is_default(&cfg) || interface_exists;
    if (marker_required)
    {
        return proto_exists ? ERRCODE_SUCCESS : lldp_db_ensure_proto_marker();
    }
    if (!proto_exists)
    {
        return ERRCODE_SUCCESS;
    }

    db_filter_builder_t pk;
    lldp_db_proto_pk(&pk);
    int rows = db_rpc_delete(ctx, LLDP_TABLE_PROTOCOL, &pk.filter);
    db_filter_clear(&pk);
    return (rows >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int lldp_db_has_config(gboolean *has_config_out)
{
    if (!has_config_out)
    {
        return ERRCODE_FAIL;
    }
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    return db_rpc_exists(ctx, LLDP_TABLE_PROTOCOL, NULL, has_config_out);
}

static int lldp_db_update_proto_field(const char *field, int64_t value)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx || !field)
    {
        return ERRCODE_FAIL;
    }
    if (lldp_db_ensure_proto_marker() != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    lldp_db_proto_pk(&pk);
    db_col_t cols[] = {
        DB_COL_INT(field, value),
    };
    int rows = db_rpc_update_cols(ctx, LLDP_TABLE_PROTOCOL, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);
    if (rows < 0)
    {
        /*
         * ensure may have inserted a default singleton immediately before the
         * failed update.  Re-evaluate the marker so that a write failure cannot
         * turn that transient row into a false on-demand revive marker.
         */
        (void)lldp_db_sync_proto_marker();
        return ERRCODE_FAIL;
    }
    return lldp_db_sync_proto_marker();
}

int lldp_db_set_proto_admin(uint8_t admin_up)
{
    return lldp_db_update_proto_field("admin_up", admin_up ? 1 : 0);
}

int lldp_db_set_proto_tx_interval(uint32_t tx_interval_sec)
{
    return lldp_db_update_proto_field("tx_interval_sec", (int64_t)tx_interval_sec);
}

int lldp_db_set_proto_hold_multiplier(uint32_t hold_multiplier)
{
    return lldp_db_update_proto_field("hold_multiplier", (int64_t)hold_multiplier);
}

int lldp_db_get_proto_cfg(lldp_proto_cfg_t *cfg_out)
{
    if (!cfg_out)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    return lldp_db_read_proto_cfg(ctx, cfg_out, NULL);
}

void lldp_db_restore_proto(void)
{
    lldp_proto_cfg_t cfg;
    if (lldp_db_get_proto_cfg(&cfg) != ERRCODE_SUCCESS)
    {
        return;
    }
    lldp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = LLDP_APPLY_OP_PROTO_SET;
    apply.u.proto = cfg;
    (void)lldp_worker_dispatch_apply(&apply);
}
