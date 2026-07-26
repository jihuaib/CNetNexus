/**
 * @file   ldp_db_protocol.c
 * @brief  LDP protocol 表：schema、CRUD、字段级 setter、启动恢复
 * @author jhb
 * @date   2026/05/05
 */

#include <string.h>

#include "errcode.h"
#include "ldp_db_internal.h"
#include "log.h"
#include "work/ldp_worker.h"

static const db_column_def_t LDP_PROTO_COLS[] = {
    {"inst_id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
    {"admin_up", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"lsr_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"hello_interval_ms", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "5000"},
    {"hold_time_ms", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "15000"},
    {"keepalive_ms", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "10000"},
};

const db_table_def_t LDP_PROTO_TABLE = {
    .table_name = LDP_TABLE_PROTOCOL,
    .cols = LDP_PROTO_COLS,
    .num_cols = G_N_ELEMENTS(LDP_PROTO_COLS),
};

int ldp_db_ensure_proto_row(dev_ipc_context_t *ctx)
{
    db_filter_builder_t pk;
    ldp_db_proto_pk(&pk);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, LDP_TABLE_PROTOCOL, &pk.filter, &exists);
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
        DB_COL_INT("inst_id", LDP_PROTOCOL_INST_ID),
        DB_COL_INT("admin_up", 0),
        DB_COL_INT("lsr_id", 0),
        DB_COL_INT("hello_interval_ms", LDP_DEFAULT_HELLO_INTERVAL_MS),
        DB_COL_INT("hold_time_ms", LDP_DEFAULT_HOLD_TIME_MS),
        DB_COL_INT("keepalive_ms", LDP_DEFAULT_KEEPALIVE_INTERVAL_MS),
    };
    return db_rpc_insert_cols(ctx, LDP_TABLE_PROTOCOL, cols, G_N_ELEMENTS(cols));
}

static int ldp_db_prune_disabled_interfaces(dev_ipc_context_t *ctx)
{
    /*
     * 当前语义中接口未配置即为 disabled，`no ldp enable` 会删除接口行。
     * 旧版本遗留的 enabled=0 行既不会被 BDR 输出，也不应维持 revive marker。
     */
    db_filter_builder_t disabled;
    db_filter_init(&disabled);
    db_filter_add_int(&disabled, "enabled", 0);
    int rows = db_rpc_delete(ctx, LDP_TABLE_INTERFACE, &disabled.filter);
    db_filter_clear(&disabled);
    if (rows < 0)
    {
        return ERRCODE_FAIL;
    }
    if (rows > 0)
    {
        LOG_INFO("LDP: removed %d legacy disabled interface row(s)", rows);
    }
    return ERRCODE_SUCCESS;
}

static int ldp_db_prune_noncanonical_proto_rows(dev_ipc_context_t *ctx)
{
    db_condition_t condition = {
        .field_name = "inst_id",
        .op = DB_CMP_NE,
        .value = db_value_int((int64_t)LDP_PROTOCOL_INST_ID),
    };
    db_filter_t filter = {.conditions = &condition, .num_conditions = 1};
    int rows = db_rpc_delete(ctx, LDP_TABLE_PROTOCOL, &filter);
    db_value_free(&condition.value);
    if (rows < 0)
    {
        return ERRCODE_FAIL;
    }
    if (rows > 0)
    {
        LOG_WARN("LDP: removed %d non-canonical protocol singleton row(s)", rows);
    }
    return ERRCODE_SUCCESS;
}

int ldp_db_sync_revive_marker(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    if (ldp_db_prune_noncanonical_proto_rows(ctx) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    if (ldp_db_prune_disabled_interfaces(ctx) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ldp_db_proto_pk(&pk);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, LDP_TABLE_PROTOCOL, NULL, 0, &pk.filter, &result);
    if (rc != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }

    if (!result || result->num_rows == 0u)
    {
        if (result)
        {
            db_result_free(result);
        }
        db_filter_clear(&pk);

        /* 修复旧库中“接口配置存在但 marker 缺失”的不一致状态。 */
        gboolean have_interfaces = FALSE;
        if (db_rpc_exists(ctx, LDP_TABLE_INTERFACE, NULL, &have_interfaces) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
        return have_interfaces ? ldp_db_ensure_proto_row(ctx) : ERRCODE_SUCCESS;
    }

    db_row_t *row = result->rows[0];
    gboolean proto_enabled = db_row_get_int(row, "admin_up", 0) != 0;
    db_result_free(result);

    if (proto_enabled)
    {
        db_filter_clear(&pk);
        return ERRCODE_SUCCESS;
    }

    gboolean have_interfaces = FALSE;
    if (db_rpc_exists(ctx, LDP_TABLE_INTERFACE, NULL, &have_interfaces) != ERRCODE_SUCCESS)
    {
        db_filter_clear(&pk);
        return ERRCODE_FAIL;
    }
    if (have_interfaces)
    {
        /*
         * admin-down 的全局字段不会被 BDR 输出。保留接口 marker 时必须把
         * 旧库隐藏的 LSR-ID/timer 归一成默认值，保证 DB 与 CFG 冷启动等价。
         */
        db_col_t defaults[] = {
            DB_COL_INT("admin_up", 0),
            DB_COL_INT("lsr_id", 0),
            DB_COL_INT("hello_interval_ms", LDP_DEFAULT_HELLO_INTERVAL_MS),
            DB_COL_INT("hold_time_ms", LDP_DEFAULT_HOLD_TIME_MS),
            DB_COL_INT("keepalive_ms", LDP_DEFAULT_KEEPALIVE_INTERVAL_MS),
        };
        int rows = db_rpc_update_cols(ctx, LDP_TABLE_PROTOCOL, &pk.filter, defaults, G_N_ELEMENTS(defaults));
        db_filter_clear(&pk);
        return rows < 0 ? ERRCODE_FAIL : ERRCODE_SUCCESS;
    }

    /*
     * ldp_protocol 同时是 DEV 的 revive-table。只有 admin_up 或接口行才是
     * 可回放配置；旧版本遗留的 admin_up=0 隐藏 lsr/timer 不能让模块复活。
     * 依赖配置先清空、marker 最后清空。
     */
    int rows = db_rpc_delete(ctx, LDP_TABLE_PROTOCOL, &pk.filter);
    db_filter_clear(&pk);
    if (rows < 0)
    {
        return ERRCODE_FAIL;
    }
    if (rows > 0)
    {
        LOG_INFO("LDP: removed empty revive marker");
    }
    return ERRCODE_SUCCESS;
}

static int ldp_db_update_proto_field(const char *field, int64_t value)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx || !field)
    {
        return ERRCODE_FAIL;
    }
    if (ldp_db_ensure_proto_row(ctx) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ldp_db_proto_pk(&pk);
    db_col_t cols[] = {
        DB_COL_INT(field, value),
    };
    int rows = db_rpc_update_cols(ctx, LDP_TABLE_PROTOCOL, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);
    if (rows < 0)
    {
        /* ensure 后 update 失败时，不得遗留一条全默认的假 marker。 */
        (void)ldp_db_sync_revive_marker(ctx);
        return ERRCODE_FAIL;
    }
    return ldp_db_sync_revive_marker(ctx);
}

int ldp_db_set_proto_admin(uint8_t admin_up)
{
    return ldp_db_update_proto_field("admin_up", admin_up ? 1 : 0);
}

int ldp_db_set_lsr_id(uint32_t lsr_id)
{
    return ldp_db_update_proto_field("lsr_id", (int64_t)lsr_id);
}

int ldp_db_set_hello_interval(uint32_t hello_ms)
{
    return ldp_db_update_proto_field("hello_interval_ms", (int64_t)hello_ms);
}

int ldp_db_set_hold_time(uint32_t hold_ms)
{
    return ldp_db_update_proto_field("hold_time_ms", (int64_t)hold_ms);
}

int ldp_db_set_keepalive(uint32_t keepalive_ms)
{
    return ldp_db_update_proto_field("keepalive_ms", (int64_t)keepalive_ms);
}

int ldp_db_query_proto_cfg(ldp_proto_cfg_t *cfg_out, gboolean *found_out)
{
    if (!cfg_out || !found_out)
    {
        return ERRCODE_FAIL;
    }
    memset(cfg_out, 0, sizeof(*cfg_out));
    *found_out = FALSE;
    cfg_out->hello_interval_ms = LDP_DEFAULT_HELLO_INTERVAL_MS;
    cfg_out->hold_time_ms = LDP_DEFAULT_HOLD_TIME_MS;
    cfg_out->keepalive_ms = LDP_DEFAULT_KEEPALIVE_INTERVAL_MS;

    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_filter_builder_t pk;
    ldp_db_proto_pk(&pk);

    db_result_t *result = NULL;
    int rc = db_rpc_query(ctx, LDP_TABLE_PROTOCOL, NULL, 0, &pk.filter, &result);
    db_filter_clear(&pk);
    if (rc != ERRCODE_SUCCESS)
    {
        if (result)
        {
            db_result_free(result);
        }
        return ERRCODE_FAIL;
    }
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
    cfg_out->lsr_id = (uint32_t)db_row_get_int(row, "lsr_id", 0);
    cfg_out->hello_interval_ms = (uint32_t)db_row_get_int(row, "hello_interval_ms", LDP_DEFAULT_HELLO_INTERVAL_MS);
    cfg_out->hold_time_ms = (uint32_t)db_row_get_int(row, "hold_time_ms", LDP_DEFAULT_HOLD_TIME_MS);
    cfg_out->keepalive_ms = (uint32_t)db_row_get_int(row, "keepalive_ms", LDP_DEFAULT_KEEPALIVE_INTERVAL_MS);
    db_result_free(result);
    *found_out = TRUE;
    return ERRCODE_SUCCESS;
}

int ldp_db_get_proto_cfg(ldp_proto_cfg_t *cfg_out)
{
    gboolean found = FALSE;
    if (ldp_db_query_proto_cfg(cfg_out, &found) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    return found ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

void ldp_db_restore_proto(void)
{
    ldp_proto_cfg_t cfg;
    if (ldp_db_get_proto_cfg(&cfg) != ERRCODE_SUCCESS)
    {
        return;
    }
    ldp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = LDP_APPLY_OP_PROTO_SET;
    apply.u.proto = cfg;
    (void)ldp_worker_dispatch_apply(&apply);
}
