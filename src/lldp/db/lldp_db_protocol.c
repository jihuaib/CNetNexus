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

static int lldp_db_update_proto_field(const char *field, int64_t value)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx || !field)
    {
        return ERRCODE_FAIL;
    }
    if (lldp_db_ensure_proto_row(ctx) != ERRCODE_SUCCESS)
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
    return (rows >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
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
    memset(cfg_out, 0, sizeof(*cfg_out));
    cfg_out->tx_interval_sec = LLDP_DEFAULT_TX_INTERVAL_SEC;
    cfg_out->hold_multiplier = LLDP_DEFAULT_HOLD_MULTIPLIER;
    cfg_out->reinit_delay_sec = LLDP_DEFAULT_REINIT_DELAY_SEC;
    cfg_out->tx_delay_sec = LLDP_DEFAULT_TX_DELAY_SEC;

    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    if (lldp_db_ensure_proto_row(ctx) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    lldp_db_proto_pk(&pk);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, LLDP_TABLE_PROTOCOL, NULL, 0, &pk.filter, &result);
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
    cfg_out->admin_up = (uint8_t)(db_row_get_int(row, "admin_up", 0) ? 1 : 0);
    cfg_out->tx_interval_sec = (uint32_t)db_row_get_int(row, "tx_interval_sec", LLDP_DEFAULT_TX_INTERVAL_SEC);
    cfg_out->hold_multiplier = (uint32_t)db_row_get_int(row, "hold_multiplier", LLDP_DEFAULT_HOLD_MULTIPLIER);
    cfg_out->reinit_delay_sec = (uint32_t)db_row_get_int(row, "reinit_delay_sec", LLDP_DEFAULT_REINIT_DELAY_SEC);
    cfg_out->tx_delay_sec = (uint32_t)db_row_get_int(row, "tx_delay_sec", LLDP_DEFAULT_TX_DELAY_SEC);
    db_result_free(result);
    return ERRCODE_SUCCESS;
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
