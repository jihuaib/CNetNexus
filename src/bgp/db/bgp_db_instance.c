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

// ============================================================================
// 表 schema
// ============================================================================

static const db_column_def_t BGP_INSTANCE_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"afi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"safi", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"import_protos", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},        /* 已导入协议位掩码 */
    {"route_select_enabled", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"}, /* QP route-select 开关 */
};

const db_table_def_t BGP_INSTANCE_TABLE = {
    .table_name = BGP_TABLE_INSTANCE,
    .cols = BGP_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(BGP_INSTANCE_COLS),
};

// ============================================================================
// CRUD
// ============================================================================

int bgp_db_set_instance(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_id, afi, safi);

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
        LOG_INFO("BGP instance vrf=%u afi=%u safi=%u already exists", vrf_id, (unsigned)afi, (unsigned)safi);
        return 0;
    }

    db_col_t cols[] = {
        DB_COL_INT("vrf_id", vrf_id),
        DB_COL_INT("afi", afi),
        DB_COL_INT("safi", safi),
    };
    ret = db_rpc_insert_cols(ctx, BGP_TABLE_INSTANCE, cols, G_N_ELEMENTS(cols));
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to insert instance vrf=%u afi=%u safi=%u", vrf_id, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP instance vrf=%u afi=%u safi=%u written", vrf_id, (unsigned)afi, (unsigned)safi);
    return 0;
}

int bgp_db_del_instance(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    int rows_neighbor = bgp_db_del_neighbors_by_afi(vrf_id, afi, safi);
    if (rows_neighbor < 0)
    {
        LOG_ERROR("BGP failed to cascade delete neighbors for instance vrf=%u afi=%u safi=%u", vrf_id, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }

    int rows_qp_route = bgp_db_del_qp_routes_by_afi(vrf_id, afi, safi);
    if (rows_qp_route < 0)
    {
        LOG_ERROR("BGP failed to cascade delete QP routes for instance vrf=%u afi=%u safi=%u", vrf_id, (unsigned)afi,
                  (unsigned)safi);
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_id, afi, safi);

    int rows = db_rpc_delete(ctx, BGP_TABLE_INSTANCE, &pk.filter);
    db_filter_clear(&pk);

    if (rows < 0)
    {
        LOG_ERROR("BGP failed to delete instance");
        return -1;
    }

    LOG_INFO("BGP deleted instance vrf=%u afi=%u safi=%u, instance_rows=%d neighbor_rows=%d qp_route_rows=%d", vrf_id,
             (unsigned)afi, (unsigned)safi, rows, rows_neighbor, rows_qp_route);
    return rows + rows_neighbor + rows_qp_route;
}

int bgp_db_set_import_protos(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi, uint32_t import_protos)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_id, afi, safi);

    db_col_t cols[] = {
        DB_COL_INT("import_protos", import_protos),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP 写入 instance import_protos vrf=%u afi=%u safi=%u 失败", vrf_id, (unsigned)afi, (unsigned)safi);
        return -1;
    }

    LOG_INFO("BGP instance vrf=%u afi=%u safi=%u import_protos=0x%08X 已写入", vrf_id, (unsigned)afi, (unsigned)safi,
             import_protos);
    return 0;
}

int bgp_db_set_route_select(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi, bool enabled)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || safi != BGP_SAFI_QP)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_instance_pk(&pk, vrf_id, afi, safi);

    db_col_t cols[] = {
        DB_COL_INT("route_select_enabled", enabled ? 1 : 0),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_INSTANCE, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP 写入 route-select vrf=%u afi=%u safi=%u enabled=%d 失败", vrf_id, (unsigned)afi, (unsigned)safi,
                  enabled ? 1 : 0);
        return -1;
    }

    LOG_INFO("BGP route-select vrf=%u afi=%u safi=%u enabled=%d 已写入", vrf_id, (unsigned)afi, (unsigned)safi,
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
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);

        if (afi == 0 || safi == 0)
        {
            continue;
        }

        /* 恢复 AF 实例 */
        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_ADDR_FAMILY;
        apply.isNo = false;
        apply.vrf_id = vrf_id;
        apply.u.instance.afi = afi;
        apply.u.instance.safi = safi;
        if (bgp_worker_dispatch_apply(&apply) != 0 || apply.rc != BGP_APPLY_RC_OK)
        {
            LOG_WARN("BGP restore: AF instance vrf=%u afi=%u safi=%u failed", vrf_id, (unsigned)afi, (unsigned)safi);
            continue;
        }
        LOG_INFO("BGP restore: VRF %u AF instance afi=%u safi=%u", vrf_id, (unsigned)afi, (unsigned)safi);

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
            imp.vrf_id = vrf_id;
            imp.u.import_route.afi = afi;
            imp.u.import_route.safi = safi;
            imp.u.import_route.import_proto = proto;
            (void)bgp_worker_dispatch_apply(&imp);

            /* 重新订阅路由模块（fire-and-forget） */
            route_subscribe_req_t *req = g_malloc(sizeof(route_subscribe_req_t));
            req->protocol = proto;
            req->vrf_id = vrf_id;
            req->afi = (uint16_t)afi;
            req->_pad = 0;
            req->flags = ROUTE_SUBSCRIBE_FLAG_FULL;
            dev_ipc_message_t *sub_msg =
                dev_ipc_message_create(ROUTE_MSG_TYPE_SUBSCRIBE, DEV_MODULE_ID_BGP, DEV_MODULE_ID_ROUTE, 0, req,
                                       sizeof(route_subscribe_req_t), g_free);
            if (sub_msg)
            {
                dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, sub_msg);
                dev_ipc_message_free(sub_msg);
            }
            LOG_INFO("BGP restore: VRF %u afi=%u safi=%u import_protos=0x%08X proto=%u，已重新订阅路由模块", vrf_id,
                     (unsigned)afi, (unsigned)safi, import_protos, proto);
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
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);
        bgp_afi_t afi = (bgp_afi_t)db_row_get_int(row, "afi", 0);
        bgp_safi_t safi = (bgp_safi_t)db_row_get_int(row, "safi", 0);
        gboolean enabled = db_row_get_int(row, "route_select_enabled", 0) != 0;

        if (!enabled || safi != BGP_SAFI_QP || afi == 0)
        {
            continue;
        }

        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_ROUTE_SELECT;
        apply.isNo = false;
        apply.vrf_id = vrf_id;
        apply.u.route_select.afi = afi;
        apply.u.route_select.safi = safi;

        if (bgp_worker_dispatch_apply(&apply) != 0 || apply.rc != BGP_APPLY_RC_OK)
        {
            LOG_WARN("BGP restore: route-select vrf=%u afi=%u safi=%u apply failed", vrf_id, (unsigned)afi,
                     (unsigned)safi);
            continue;
        }

        LOG_INFO("BGP restore: route-select vrf=%u afi=%u safi=%u enabled", vrf_id, (unsigned)afi, (unsigned)safi);
    }

    db_result_free(result);
}
