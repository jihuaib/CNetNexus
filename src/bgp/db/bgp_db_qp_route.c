/**
 * @file   bgp_db_qp_route.c
 * @brief  BGP qp_route 表（QP 自产生路由配置）：schema、CRUD、启动恢复
 * @author jhb
 * @date   2026/04/26
 */

#include <string.h>

#include "bgp_db_internal.h"
#include "bgp_main.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 表 schema
// ============================================================================

static const db_column_def_t BGP_QP_ROUTE_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},       {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"safi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},        {"start_dqpn", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"route_count", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL}, {"prefix_addr", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"mask_len", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},    {"bid", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
};

const db_table_def_t BGP_QP_ROUTE_TABLE = {
    .table_name = BGP_TABLE_QP_ROUTE,
    .cols = BGP_QP_ROUTE_COLS,
    .num_cols = G_N_ELEMENTS(BGP_QP_ROUTE_COLS),
};

// ============================================================================
// CRUD
// ============================================================================

int bgp_db_set_qp_route(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi, uint32_t start_dqpn, uint32_t count,
                        const char *prefix_addr, uint8_t mask_len, const char *bid)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !prefix_addr || !bid || safi != BGP_SAFI_QP)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_qp_route_pk(&pk, vrf_id, afi, safi, start_dqpn, count, prefix_addr, mask_len, bid);

    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, BGP_TABLE_QP_ROUTE, &pk.filter, &exists);
    db_filter_clear(&pk);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 QP route vrf=%u afi=%u start_dqpn=%u 失败", vrf_id, (unsigned)afi, start_dqpn);
        return -1;
    }
    if (exists)
    {
        return 0;
    }

    db_col_t cols[] = {
        DB_COL_INT("vrf_id", vrf_id),         DB_COL_INT("afi", afi),           DB_COL_INT("safi", safi),
        DB_COL_INT("start_dqpn", start_dqpn), DB_COL_INT("route_count", count), DB_COL_TEXT("prefix_addr", prefix_addr),
        DB_COL_INT("mask_len", mask_len),     DB_COL_TEXT("bid", bid),
    };
    ret = db_rpc_insert_cols(ctx, BGP_TABLE_QP_ROUTE, cols, G_N_ELEMENTS(cols));
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 写入 QP route vrf=%u afi=%u start_dqpn=%u 失败", vrf_id, (unsigned)afi, start_dqpn);
        return -1;
    }

    LOG_INFO("BGP QP route vrf=%u afi=%u start_dqpn=%u count=%u 已写入", vrf_id, (unsigned)afi, start_dqpn, count);
    return 0;
}

int bgp_db_del_qp_route(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi, uint32_t start_dqpn, uint32_t count,
                        const char *prefix_addr, uint8_t mask_len, const char *bid)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !prefix_addr || !bid || safi != BGP_SAFI_QP)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_qp_route_pk(&pk, vrf_id, afi, safi, start_dqpn, count, prefix_addr, mask_len, bid);

    int rows = db_rpc_delete(ctx, BGP_TABLE_QP_ROUTE, &pk.filter);
    db_filter_clear(&pk);

    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 QP route vrf=%u afi=%u start_dqpn=%u 失败", vrf_id, (unsigned)afi, start_dqpn);
        return -1;
    }

    LOG_INFO("BGP 删除 QP route vrf=%u afi=%u start_dqpn=%u，影响行数: %d", vrf_id, (unsigned)afi, start_dqpn, rows);
    return rows;
}

int bgp_db_del_qp_routes_by_afi(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_id, afi, safi);

    int rows = db_rpc_delete(ctx, BGP_TABLE_QP_ROUTE, &pk.filter);
    db_filter_clear(&pk);

    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 AF 下 QP routes vrf=%u afi=%u safi=%u 失败", vrf_id, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP 删除 AF 下 QP routes vrf=%u afi=%u safi=%u，影响行数: %d", vrf_id, (unsigned)afi, (unsigned)safi,
             rows);
    return rows;
}

// ============================================================================
// 启动恢复
// ============================================================================

void bgp_db_restore_qp_routes(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_QP_ROUTE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);
        uint32_t start_dqpn = (uint32_t)db_row_get_int(row, "start_dqpn", 0);
        uint32_t count = (uint32_t)db_row_get_int(row, "route_count", 0);
        uint8_t mask_len = (uint8_t)db_row_get_int(row, "mask_len", 0);
        const char *prefix_addr = db_row_get_text(row, "prefix_addr", NULL);
        const char *bid = db_row_get_text(row, "bid", NULL);

        if (!prefix_addr || !bid || afi == 0 || safi != BGP_SAFI_QP)
        {
            continue;
        }

        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_QP_ROUTE;
        apply.isNo = false;
        apply.vrf_id = vrf_id;
        apply.u.qp_route.afi = afi;
        apply.u.qp_route.safi = safi;
        apply.u.qp_route.start_dqpn = start_dqpn;
        apply.u.qp_route.count = count;
        apply.u.qp_route.mask_len = mask_len;

        if (net_addr_from_str(prefix_addr, &apply.u.qp_route.ip) != 0)
        {
            LOG_WARN("BGP restore: QP route prefix %s parse failed, skipping", prefix_addr);
            continue;
        }
        if (net_addr_from_str(bid, &apply.u.qp_route.bid) != 0)
        {
            LOG_WARN("BGP restore: QP route BID %s parse failed, skipping", bid);
            continue;
        }

        if (bgp_worker_dispatch_apply(&apply) != 0 || apply.rc != BGP_APPLY_RC_OK)
        {
            LOG_WARN("BGP restore: QP route vrf=%u afi=%u start_dqpn=%u apply failed", vrf_id, (unsigned)afi,
                     start_dqpn);
            continue;
        }

        LOG_INFO("BGP restore: QP route vrf=%u afi=%u start_dqpn=%u count=%u restored", vrf_id, (unsigned)afi,
                 start_dqpn, count);
    }

    db_result_free(result);
}
