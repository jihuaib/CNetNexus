/**
 * @file   bgp_db_neighbor.c
 * @brief  BGP neighbor 表（地址族邻居使能）：schema、CRUD、启动恢复
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
#include "vrf.h"

// ============================================================================
// 表 schema
// ============================================================================

static const db_column_def_t BGP_NEIGHBOR_COLS[] = {
    {"vrf_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, VRF_PUBLIC_VRF_NAME},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"safi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"neighbor_ip", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"is_rr_client", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"}, /* RFC 4456: 1=RR 客户端 */
};

const db_table_def_t BGP_NEIGHBOR_TABLE = {
    .table_name = BGP_TABLE_NEIGHBOR,
    .cols = BGP_NEIGHBOR_COLS,
    .num_cols = G_N_ELEMENTS(BGP_NEIGHBOR_COLS),
};

// ============================================================================
// CRUD
// ============================================================================

int bgp_db_set_neighbor(const char *vrf_name, const char *neighbor_ip, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name || !neighbor_ip)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_neighbor_pk(&pk, vrf_name, afi, safi, neighbor_ip);

    /* neighbor 记录 4 列联合为键，存在即幂等 */
    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, BGP_TABLE_NEIGHBOR, &pk.filter, &exists);
    db_filter_clear(&pk);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to query neighbor existence");
        return -1;
    }
    if (exists)
    {
        LOG_INFO("BGP neighbor vrf=%s %s afi=%u safi=%u already exists", vrf_name, neighbor_ip, (unsigned)afi,
                 (unsigned)safi);
        return 0;
    }

    db_col_t cols[] = {
        DB_COL_TEXT("vrf_name", vrf_name),
        DB_COL_INT("afi", afi),
        DB_COL_INT("safi", safi),
        DB_COL_TEXT("neighbor_ip", neighbor_ip),
    };
    ret = db_rpc_insert_cols(ctx, BGP_TABLE_NEIGHBOR, cols, G_N_ELEMENTS(cols));
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to insert neighbor vrf=%s %s afi=%u safi=%u", vrf_name, neighbor_ip, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP neighbor vrf=%s %s afi=%u safi=%u enabled", vrf_name, neighbor_ip, (unsigned)afi, (unsigned)safi);
    return 0;
}

int bgp_db_del_neighbor(const char *vrf_name, const char *neighbor_ip, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name || !neighbor_ip)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_neighbor_pk(&pk, vrf_name, afi, safi, neighbor_ip);

    int rows = db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &pk.filter);
    db_filter_clear(&pk);

    if (rows < 0)
    {
        LOG_ERROR("BGP failed to delete neighbor");
        return -1;
    }

    LOG_INFO("BGP deleted neighbor vrf=%s %s afi=%u safi=%u, affected rows: %d", vrf_name, neighbor_ip, (unsigned)afi,
             (unsigned)safi, rows);
    return rows;
}

int bgp_db_set_neighbor_rr_client(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, const char *neighbor_ip,
                                  bool is_client)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name || !neighbor_ip)
    {
        return -1;
    }
    db_filter_builder_t pk;
    bgp_db_neighbor_pk(&pk, vrf_name, afi, safi, neighbor_ip);
    db_col_t cols[] = {
        DB_COL_INT("is_rr_client", is_client ? 1 : 0),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_NEIGHBOR, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);
    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write neighbor rr-client vrf=%s %s afi=%u safi=%u", vrf_name, neighbor_ip,
                  (unsigned)afi, (unsigned)safi);
        return -1;
    }
    LOG_INFO("BGP neighbor vrf=%s %s afi=%u safi=%u rr-client=%d written", vrf_name, neighbor_ip, (unsigned)afi,
             (unsigned)safi, is_client ? 1 : 0);
    return 0;
}

int bgp_db_del_neighbors_by_afi(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_name, afi, safi);

    int rows = db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &pk.filter);
    db_filter_clear(&pk);

    if (rows < 0)
    {
        LOG_ERROR("BGP failed to batch delete neighbor vrf=%s afi=%u safi=%u", vrf_name, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP batch deleted neighbor vrf=%s afi=%u safi=%u, affected rows: %d", vrf_name, (unsigned)afi,
             (unsigned)safi, rows);
    return rows;
}

// ============================================================================
// 启动恢复
// ============================================================================

void bgp_db_restore_neighbors(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_NEIGHBOR, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *nb_ip = db_row_get_text(row, "neighbor_ip", NULL);
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);
        const char *vrf_name = db_row_get_text(row, "vrf_name", VRF_PUBLIC_VRF_NAME);

        if (!nb_ip)
        {
            continue;
        }
        if (g_bgp_db_resync_only_vrf_bound && (!vrf_name || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0))
        {
            continue;
        }

        net_addr_t nb_addr;
        if (net_addr_from_str(nb_ip, &nb_addr) != 0)
        {
            LOG_WARN("BGP restore: Peer neighbor address %s parse failed, skipping", nb_ip);
            continue;
        }

        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_AF_NEIGHBOR;
        apply.isNo = false;
        snprintf(apply.vrf_name, sizeof(apply.vrf_name), "%s", vrf_name);
        apply.u.af_neighbor.afi = afi;
        apply.u.af_neighbor.safi = safi;
        apply.u.af_neighbor.addr = nb_addr;
        (void)bgp_worker_dispatch_apply(&apply);

        /* 恢复 reflect-client 标记（RFC 4456） */
        if (db_row_get_int(row, "is_rr_client", 0) != 0)
        {
            bgp_apply_cmd_t rr;
            memset(&rr, 0, sizeof(rr));
            rr.group_id = BGP_CLI_GROUP_ID_REFLECT_CLIENT;
            rr.isNo = false;
            snprintf(rr.vrf_name, sizeof(rr.vrf_name), "%s", vrf_name);
            rr.u.reflect_client.afi = afi;
            rr.u.reflect_client.safi = safi;
            rr.u.reflect_client.addr = nb_addr;
            (void)bgp_worker_dispatch_apply(&rr);
            LOG_INFO("BGP restore: VRF %s %s afi=%u safi=%u reflect-client", vrf_name, nb_ip, (unsigned)afi,
                     (unsigned)safi);
        }
    }

    db_result_free(result);
}
