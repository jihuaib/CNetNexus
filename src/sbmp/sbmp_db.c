/**
 * @file   sbmp_db.c
 * @brief  SBMP 模块数据库操作实现（封装 db_rpc 调用）
 * @author jhb
 * @date   2026/03/08
 */
#include "sbmp_db.h"

#include <string.h>

#include "errcode.h"
#include "log.h"
#include "sbmp_main.h"

// ============================================================================
// 表定义
// ============================================================================

/* sbmp_server 表列定义 */
static const db_column_def_t SBMP_SERVER_COLS[] = {
    {"server_port", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
};

/* sbmp_server 表定义 */
static const db_table_def_t SBMP_SERVER_TABLE = {
    .table_name = SBMP_TABLE_SERVER,
    .cols = SBMP_SERVER_COLS,
    .num_cols = G_N_ELEMENTS(SBMP_SERVER_COLS),
};

// ============================================================================
// 公共 API
// ============================================================================

int sbmp_db_init(void)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    if (db_rpc_create_table_from_def(ctx, &SBMP_SERVER_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SBMP: Failed to create table %s", SBMP_TABLE_SERVER);
        return -1;
    }
    return 0;
}

int sbmp_db_restore(void)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    db_result_t *result = NULL;
    if (db_rpc_query(ctx, SBMP_TABLE_SERVER, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return ERRCODE_FAIL;
    }

    if (result->num_rows == 0)
    {
        db_result_free(result);
        LOG_INFO("SBMP: Database has no config, skipping restore");
        return ERRCODE_SUCCESS;
    }

    db_row_t *row = result->rows[0];
    uint16_t port = (uint16_t)db_row_get_int(row, "server_port", 0);
    db_result_free(result);

    if (port > 0)
    {
        LOG_INFO("SBMP: Restoring server port=%u", port);
        sbmp_listen_start(port);
    }

    return ERRCODE_SUCCESS;
}

int sbmp_db_set_server_port(uint16_t port)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    db_record_t *rec = db_record_new();
    db_record_set_int(rec, "server_port", port);

    db_condition_t cond = {.field_name = "server_port", .op = DB_CMP_EQ, .value = db_value_int(port)};
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};

    int ret = db_rpc_upsert(ctx, SBMP_TABLE_SERVER, rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SBMP: Failed to write server_port=%u", port);
        return -1;
    }
    return 0;
}

int sbmp_db_del_server_port(void)
{
    dev_ipc_context_t *ctx = sbmp_local_ipc_ctx();
    return db_rpc_delete(ctx, SBMP_TABLE_SERVER, NULL);
}
