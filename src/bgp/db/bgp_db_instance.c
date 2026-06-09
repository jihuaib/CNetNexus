/**
 * @file   bgp_db_instance.c
 * @brief  BGP instance 表（地址族实例）：schema、CRUD、字段级 setter、启动恢复
 * @author jhb
 * @date   2026/04/26
 */

#include <stdbool.h>
#include <string.h>

#include "bgp_db_internal.h"
#include "bgp_main.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "route.h"
#include "vrf.h"

// ============================================================================
// 表 schema
// ============================================================================

static const db_column_def_t BGP_INSTANCE_COLS[] = {
    {"vrf_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, VRF_PUBLIC_VRF_NAME},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"safi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"import_protos", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},        /* 已导入协议位掩码 */
    {"route_select_enabled", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"}, /* QP route-select 开关 */
    {"cluster_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"}, /* 主机序 32 位 cluster-id（0=用 router-id） */
    {"import_rib_sources", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"}, /* import-rib 源位掩码（bgp_import_src_t） */
    {"vpn_target_policy", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"}, /* VPN AF 入向 vpn-target 过滤（默认 1=启用） */
};

const db_table_def_t BGP_INSTANCE_TABLE = {
    .table_name = BGP_TABLE_INSTANCE,
    .cols = BGP_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(BGP_INSTANCE_COLS),
};

// ============================================================================
// CRUD
// ============================================================================

int bgp_db_set_instance(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_name, afi, safi);

    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, BGP_TABLE_INSTANCE, &pk.filter, &exists);
    db_filter_clear(&pk);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to query instance existence");
        return -1;
    }
    if (exists)
    {
        LOG_INFO("BGP instance vrf=%s afi=%u safi=%u already exists", vrf_name, (unsigned)afi, (unsigned)safi);
        return 0;
    }

    db_col_t cols[] = {
        DB_COL_TEXT("vrf_name", vrf_name),
        DB_COL_INT("afi", afi),
        DB_COL_INT("safi", safi),
    };
    ret = db_rpc_insert_cols(ctx, BGP_TABLE_INSTANCE, cols, G_N_ELEMENTS(cols));
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to insert instance vrf=%s afi=%u safi=%u", vrf_name, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP instance vrf=%s afi=%u safi=%u written", vrf_name, (unsigned)afi, (unsigned)safi);
    return 0;
}

int bgp_db_del_instance(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }

    int rows_neighbor = bgp_db_del_neighbors_by_afi(vrf_name, afi, safi);
    if (rows_neighbor < 0)
    {
        LOG_ERROR("BGP failed to cascade delete neighbors for instance vrf=%s afi=%u safi=%u", vrf_name, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }

    int rows_qp_route = bgp_db_del_qp_routes_by_afi(vrf_name, afi, safi);
    if (rows_qp_route < 0)
    {
        LOG_ERROR("BGP failed to cascade delete QP routes for instance vrf=%s afi=%u safi=%u", vrf_name, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_name, afi, safi);

    int rows = db_rpc_delete(ctx, BGP_TABLE_INSTANCE, &pk.filter);
    db_filter_clear(&pk);

    if (rows < 0)
    {
        LOG_ERROR("BGP failed to delete instance");
        return -1;
    }

    LOG_INFO("BGP deleted instance vrf=%s afi=%u safi=%u, instance_rows=%d neighbor_rows=%d qp_route_rows=%d", vrf_name,
             (unsigned)afi, (unsigned)safi, rows, rows_neighbor, rows_qp_route);
    return rows + rows_neighbor + rows_qp_route;
}

int bgp_db_set_import_protos(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, uint32_t import_protos)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_name, afi, safi);

    db_col_t cols[] = {
        DB_COL_INT("import_protos", import_protos),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP 写入 instance import_protos vrf=%s afi=%u safi=%u 失败", vrf_name, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP instance vrf=%s afi=%u safi=%u import_protos=0x%08X 已写入", vrf_name, (unsigned)afi, (unsigned)safi,
             import_protos);
    return 0;
}

int bgp_db_set_import_rib_sources(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, uint32_t sources)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }
    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_name, afi, safi);
    db_col_t cols[] = {
        DB_COL_INT("import_rib_sources", (int64_t)sources),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);
    if (rows <= 0)
    {
        LOG_ERROR("BGP 写入 instance import_rib_sources vrf=%s afi=%u safi=%u 失败", vrf_name, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }
    LOG_INFO("BGP instance vrf=%s afi=%u safi=%u import_rib_sources=0x%08X 已写入", vrf_name, (unsigned)afi,
             (unsigned)safi, sources);
    return 0;
}

int bgp_db_set_inst_cluster_id(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, uint32_t cluster_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }
    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_name, afi, safi);
    db_col_t cols[] = {
        DB_COL_INT("cluster_id", (int64_t)cluster_id),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);
    if (rows <= 0)
    {
        LOG_ERROR("BGP 写入 instance cluster-id vrf=%s afi=%u safi=%u 失败", vrf_name, (unsigned)afi, (unsigned)safi);
        return -1;
    }
    LOG_INFO("BGP instance vrf=%s afi=%u safi=%u cluster-id=%u 已写入", vrf_name, (unsigned)afi, (unsigned)safi,
             cluster_id);
    return 0;
}

int bgp_db_set_route_select(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, bool enabled)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name || safi != BGP_SAFI_QP)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_name, afi, safi);

    db_col_t cols[] = {
        DB_COL_INT("route_select_enabled", enabled ? 1 : 0),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP 写入 route-select vrf=%s afi=%u safi=%u enabled=%d 失败", vrf_name, (unsigned)afi,
                  (unsigned)safi, enabled ? 1 : 0);
        return -1;
    }

    LOG_INFO("BGP route-select vrf=%s afi=%u safi=%u enabled=%d 已写入", vrf_name, (unsigned)afi, (unsigned)safi,
             enabled ? 1 : 0);
    return 0;
}

int bgp_db_set_vpn_target_policy(const char *vrf_name, bgp_afi_t afi, bgp_safi_t safi, bool enabled)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_name, afi, safi);

    db_col_t cols[] = {
        DB_COL_INT("vpn_target_policy", enabled ? 1 : 0),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP 写入 vpn-target policy vrf=%s afi=%u safi=%u enabled=%d 失败", vrf_name, (unsigned)afi,
                  (unsigned)safi, enabled ? 1 : 0);
        return -1;
    }

    LOG_INFO("BGP vpn-target policy vrf=%s afi=%u safi=%u enabled=%d 已写入", vrf_name, (unsigned)afi, (unsigned)safi,
             enabled ? 1 : 0);
    return 0;
}

// ============================================================================
// 启动恢复
// ============================================================================

void bgp_db_restore_instances(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *vrf_name = db_row_get_text(row, "vrf_name", VRF_PUBLIC_VRF_NAME);
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);

        if (afi == 0 || safi == 0)
        {
            continue;
        }
        if (g_bgp_db_resync_only_vrf_bound && (!vrf_name || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0))
        {
            continue;
        }

        /* 恢复 AF 实例 */
        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_ADDR_FAMILY;
        apply.isNo = false;
        snprintf(apply.vrf_name, sizeof(apply.vrf_name), "%s", vrf_name);
        apply.u.instance.afi = afi;
        apply.u.instance.safi = safi;
        if (bgp_worker_dispatch_apply(&apply) != 0 || apply.rc != BGP_APPLY_RC_OK)
        {
            LOG_WARN("BGP restore: AF instance vrf=%s afi=%u safi=%u failed", vrf_name, (unsigned)afi, (unsigned)safi);
            continue;
        }
        LOG_INFO("BGP restore: VRF %s AF instance afi=%u safi=%u", vrf_name, (unsigned)afi, (unsigned)safi);

        /* 恢复 import-route（支持 static / connected） */
        uint32_t import_protos = (uint32_t)db_row_get_int(row, "import_protos", 0);
        static const uint32_t k_supported_import_protos[] = {ROUTE_PROTOCOL_STATIC, ROUTE_PROTOCOL_CONNECTED};
        for (size_t pi = 0; pi < G_N_ELEMENTS(k_supported_import_protos); ++pi)
        {
            uint32_t proto = k_supported_import_protos[pi];
            if ((import_protos & (1u << proto)) == 0u)
            {
                continue;
            }

            bgp_apply_cmd_t imp;
            memset(&imp, 0, sizeof(imp));
            imp.group_id = BGP_CLI_GROUP_ID_IMPORT_ROUTE;
            imp.isNo = false;
            snprintf(imp.vrf_name, sizeof(imp.vrf_name), "%s", vrf_name);
            imp.u.import_route.afi = afi;
            imp.u.import_route.safi = safi;
            imp.u.import_route.import_proto = proto;
            (void)bgp_worker_dispatch_apply(&imp);
            LOG_INFO("BGP restore: VRF %s afi=%u safi=%u import_protos=0x%08X proto=%u", vrf_name, (unsigned)afi,
                     (unsigned)safi, import_protos, proto);
        }

        /* 恢复 import-rib 跨 AF 路由互导 */
        uint32_t import_rib_sources = (uint32_t)db_row_get_int(row, "import_rib_sources", 0);
        for (uint32_t bit = 0; bit < 32 && import_rib_sources != 0; ++bit)
        {
            if ((import_rib_sources & (1U << bit)) == 0U)
            {
                continue;
            }
            bgp_apply_cmd_t imp;
            memset(&imp, 0, sizeof(imp));
            imp.group_id = BGP_CLI_GROUP_ID_IMPORT_RIB;
            imp.isNo = false;
            snprintf(imp.vrf_name, sizeof(imp.vrf_name), "%s", vrf_name);
            imp.u.import_rib.afi = afi;
            imp.u.import_rib.safi = safi;
            imp.u.import_rib.src = bit;
            (void)bgp_worker_dispatch_apply(&imp);
            LOG_INFO("BGP restore: VRF %s afi=%u safi=%u import-rib src=%u", vrf_name, (unsigned)afi, (unsigned)safi,
                     bit);
        }

        /* 恢复 cluster-id（RFC 4456） */
        uint32_t cluster_id = (uint32_t)db_row_get_int(row, "cluster_id", 0);
        if (cluster_id != 0)
        {
            bgp_apply_cmd_t cid;
            memset(&cid, 0, sizeof(cid));
            cid.group_id = BGP_CLI_GROUP_ID_CLUSTER_ID;
            cid.isNo = false;
            snprintf(cid.vrf_name, sizeof(cid.vrf_name), "%s", vrf_name);
            cid.u.cluster_id.afi = afi;
            cid.u.cluster_id.safi = safi;
            cid.u.cluster_id.cluster_id = cluster_id;
            (void)bgp_worker_dispatch_apply(&cid);
            LOG_INFO("BGP restore: VRF %s afi=%u safi=%u cluster-id=%u", vrf_name, (unsigned)afi, (unsigned)safi,
                     cluster_id);
        }

        /* 恢复 VPN AF 入向 vpn-target 过滤策略（默认启用；仅当显式关闭即列值为 0 时下发 no） */
        if (safi == BGP_SAFI_VPN_UNICAST && db_row_get_int(row, "vpn_target_policy", 1) == 0)
        {
            bgp_apply_cmd_t vt;
            memset(&vt, 0, sizeof(vt));
            vt.group_id = BGP_CLI_GROUP_ID_VPN_TARGET_POLICY;
            vt.isNo = true;
            snprintf(vt.vrf_name, sizeof(vt.vrf_name), "%s", vrf_name);
            vt.u.vpn_target.afi = afi;
            vt.u.vpn_target.safi = safi;
            (void)bgp_worker_dispatch_apply(&vt);
            LOG_INFO("BGP restore: VRF %s afi=%u safi=%u no policy vpn-target", vrf_name, (unsigned)afi,
                     (unsigned)safi);
        }
    }

    db_result_free(result);
}

void bgp_db_restore_qp_route_select(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *vrf_name = db_row_get_text(row, "vrf_name", VRF_PUBLIC_VRF_NAME);
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);
        gboolean enabled = db_row_get_int(row, "route_select_enabled", 0) != 0;

        if (!enabled || safi != BGP_SAFI_QP || afi == 0)
        {
            continue;
        }
        if (g_bgp_db_resync_only_vrf_bound && (!vrf_name || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0))
        {
            continue;
        }

        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_ROUTE_SELECT;
        apply.isNo = false;
        snprintf(apply.vrf_name, sizeof(apply.vrf_name), "%s", vrf_name);
        apply.u.route_select.afi = afi;
        apply.u.route_select.safi = safi;

        if (bgp_worker_dispatch_apply(&apply) != 0 || apply.rc != BGP_APPLY_RC_OK)
        {
            LOG_WARN("BGP restore: route-select vrf=%s afi=%u safi=%u apply failed", vrf_name, (unsigned)afi,
                     (unsigned)safi);
            continue;
        }

        LOG_INFO("BGP restore: route-select vrf=%s afi=%u safi=%u enabled", vrf_name, (unsigned)afi, (unsigned)safi);
    }

    db_result_free(result);
}
