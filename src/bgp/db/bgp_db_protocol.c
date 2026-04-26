/**
 * @file   bgp_db_protocol.c
 * @brief  BGP protocol 表：schema、AS 写入/删除、协议恢复
 * @author jhb
 * @date   2026/04/26
 */

#include <string.h>

#include "bgp_bmp_db.h"
#include "bgp_db_internal.h"
#include "bgp_main.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 表 schema
// ============================================================================

static const db_column_def_t BGP_PROTOCOL_COLS[] = {
    {"as_number", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
};

const db_table_def_t BGP_PROTOCOL_TABLE = {
    .table_name = BGP_TABLE_PROTOCOL,
    .cols = BGP_PROTOCOL_COLS,
    .num_cols = G_N_ELEMENTS(BGP_PROTOCOL_COLS),
};

// ============================================================================
// 写入/删除
// ============================================================================

int bgp_db_set_as(uint32_t as_number)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_col_t cols[] = {DB_COL_INT("as_number", as_number)};
    int ret = db_rpc_insert_cols(ctx, BGP_TABLE_PROTOCOL, cols, G_N_ELEMENTS(cols));
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to write AS number %u", as_number);
        return -1;
    }

    /* 种子默认公网 VRF 行（schema DEFAULT 兜底其它字段），保证后续
     * router-id/timers/connect-retry 的纯 UPDATE 路径都能命中。
     * bgp_db_del_as 会清空 bgp_vrf 表，所以每次 set_as 都需要补一次。 */
    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, BGP_VRF_PUBLIC_ID);
    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, BGP_TABLE_VRF, &pk.filter, &exists);
    db_filter_clear(&pk);
    if (rc == ERRCODE_SUCCESS && !exists)
    {
        db_col_t vrf_cols[] = {DB_COL_INT("vrf_id", BGP_VRF_PUBLIC_ID)};
        if (db_rpc_insert_cols(ctx, BGP_TABLE_VRF, vrf_cols, G_N_ELEMENTS(vrf_cols)) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("BGP failed to seed default VRF row vrf_id=%u", BGP_VRF_PUBLIC_ID);
            return -1;
        }
    }

    LOG_INFO("BGP AS number %u written", as_number);
    return 0;
}

int bgp_db_del_as(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    int rows_neighbor = 0;
    int rows_instance = 0;
    int rows_qp_route = 0;
    int rows_session = 0;
    int rows_vrf = 0;
    int rows_protocol = 0;
    int rows_bmp_instance = 0;
    int rows_bmp_monitor = 0;

    if (bgp_db_delete_table_all(ctx, BGP_TABLE_NEIGHBOR, &rows_neighbor) != 0 ||
        bgp_db_delete_table_all(ctx, BGP_TABLE_INSTANCE, &rows_instance) != 0 ||
        bgp_db_delete_table_all(ctx, BGP_TABLE_QP_ROUTE, &rows_qp_route) != 0 ||
        bgp_db_delete_table_all(ctx, BGP_TABLE_SESSION, &rows_session) != 0 ||
        bgp_db_delete_table_all(ctx, BGP_TABLE_VRF, &rows_vrf) != 0 ||
        bgp_db_delete_table_all(ctx, BGP_TABLE_BMP_MONITOR, &rows_bmp_monitor) != 0 ||
        bgp_db_delete_table_all(ctx, BGP_TABLE_BMP_INSTANCE, &rows_bmp_instance) != 0 ||
        bgp_db_delete_table_all(ctx, BGP_TABLE_PROTOCOL, &rows_protocol) != 0)
    {
        return -1;
    }

    int total_rows = rows_neighbor + rows_instance + rows_qp_route + rows_session + rows_vrf + rows_protocol +
                     rows_bmp_instance + rows_bmp_monitor;
    LOG_INFO("BGP protocol cleanup complete: protocol=%d session=%d neighbor=%d instance=%d vrf=%d bmp_instance=%d "
             "bmp_monitor=%d qp_route=%d total=%d",
             rows_protocol, rows_session, rows_neighbor, rows_instance, rows_vrf, rows_bmp_instance, rows_bmp_monitor,
             rows_qp_route, total_rows);
    return total_rows;
}

// ============================================================================
// 启动恢复
// ============================================================================

uint32_t bgp_db_restore_protocol(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_PROTOCOL, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    if (!result || result->num_rows == 0)
    {
        db_result_free(result);
        LOG_INFO("BGP database has no config, skipping restore");
        return ERRCODE_SUCCESS;
    }

    db_row_t *row = result->rows[0];
    uint32_t as_number = (uint32_t)db_row_get_int(row, "as_number", 0);

    if (as_number != 0)
    {
        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_PROTOCOL;
        apply.isNo = false;
        apply.u.protocol.as_number = as_number;
        if (bgp_worker_dispatch_apply(&apply) == 0 && apply.rc == BGP_APPLY_RC_OK)
        {
            LOG_INFO("BGP protocol restored: AS %u", as_number);
        }
        else
        {
            LOG_ERROR("BGP restore: Protocol creation failed (as=%u)", as_number);
        }
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}
