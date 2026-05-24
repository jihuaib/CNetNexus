/**
 * @file   bgp_db_vrf.c
 * @brief  BGP vrf 表（router-id / timers / connect-retry）：schema、CRUD、启动恢复
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
#include "vrf.h"

// ============================================================================
// 表 schema
// ============================================================================

static const db_column_def_t BGP_VRF_COLS[] = {
    {"vrf_name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},      {"router_id", DB_TYPE_TEXT, DB_COL_NOT_NULL, "0.0.0.0"},
    {"keepalive", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "60"},     {"hold_time", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "180"},
    {"connect_retry", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "10"},
};

const db_table_def_t BGP_VRF_TABLE = {
    .table_name = BGP_TABLE_VRF,
    .cols = BGP_VRF_COLS,
    .num_cols = G_N_ELEMENTS(BGP_VRF_COLS),
};

// ============================================================================
// router-id
// ============================================================================

static int bgp_db_ensure_vrf_row(dev_ipc_context_t *ctx, const char *vrf_name)
{
    if (!ctx || !vrf_name)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_name);
    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, BGP_TABLE_VRF, &pk.filter, &exists);
    db_filter_clear(&pk);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to query VRF row existence vrf=%s", vrf_name);
        return -1;
    }
    if (exists)
    {
        return 0;
    }

    db_col_t cols[] = {
        DB_COL_TEXT("vrf_name", vrf_name),
    };
    ret = db_rpc_insert_cols(ctx, BGP_TABLE_VRF, cols, G_N_ELEMENTS(cols));
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP failed to insert VRF row vrf=%s", vrf_name);
        return -1;
    }
    return 0;
}

int bgp_db_ensure_vrf(const char *vrf_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }
    return bgp_db_ensure_vrf_row(ctx, vrf_name);
}

int bgp_db_del_vrf(const char *vrf_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name || vrf_name[0] == '\0' || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        return -1;
    }

    db_filter_builder_t filter;
    db_filter_init(&filter);
    db_filter_add_text(&filter, "vrf_name", vrf_name);

    int rows_neighbor = db_rpc_delete(ctx, BGP_TABLE_NEIGHBOR, &filter.filter);
    int rows_instance = db_rpc_delete(ctx, BGP_TABLE_INSTANCE, &filter.filter);
    int rows_qp_route = db_rpc_delete(ctx, BGP_TABLE_QP_ROUTE, &filter.filter);
    int rows_session = db_rpc_delete(ctx, BGP_TABLE_SESSION, &filter.filter);
    int rows_vrf = db_rpc_delete(ctx, BGP_TABLE_VRF, &filter.filter);
    db_filter_clear(&filter);

    if (rows_neighbor < 0 || rows_instance < 0 || rows_qp_route < 0 || rows_session < 0 || rows_vrf < 0)
    {
        LOG_ERROR("BGP failed to delete VRF %s from database", vrf_name);
        return -1;
    }

    int total = rows_neighbor + rows_instance + rows_qp_route + rows_session + rows_vrf;
    LOG_INFO("BGP deleted VRF %s: session=%d neighbor=%d instance=%d qp_route=%d vrf=%d total=%d", vrf_name,
             rows_session, rows_neighbor, rows_instance, rows_qp_route, rows_vrf, total);
    return total;
}

int bgp_db_set_vrf_router_id(const char *vrf_name, const char *router_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name || !router_id)
    {
        return -1;
    }
    if (bgp_db_ensure_vrf_row(ctx, vrf_name) != 0)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_name);

    db_col_t cols[] = {
        DB_COL_TEXT("router_id", router_id),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_VRF, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write VRF %s router-id=%s", vrf_name, router_id);
        return -1;
    }

    LOG_INFO("BGP VRF %s router-id=%s written", vrf_name, router_id);
    return 0;
}

int bgp_db_del_vrf_router_id(const char *vrf_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }
    if (bgp_db_ensure_vrf_row(ctx, vrf_name) != 0)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_name);

    db_col_t cols[] = {
        DB_COL_TEXT("router_id", "0.0.0.0"),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_VRF, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to delete VRF %s router-id", vrf_name);
        return -1;
    }

    LOG_INFO("BGP VRF %s router-id reset, affected rows: %d", vrf_name, rows);
    return rows;
}

// ============================================================================
// keepalive / hold_time
// ============================================================================

int bgp_db_set_vrf_timers(const char *vrf_name, uint16_t keepalive, uint16_t hold_time)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }
    if (bgp_db_ensure_vrf_row(ctx, vrf_name) != 0)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_name);

    db_col_t cols[] = {
        DB_COL_INT("keepalive", keepalive),
        DB_COL_INT("hold_time", hold_time),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_VRF, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write VRF %s timers keepalive=%u hold=%u", vrf_name, keepalive, hold_time);
        return -1;
    }

    LOG_INFO("BGP VRF %s timers keepalive=%u hold=%u written", vrf_name, keepalive, hold_time);
    return 0;
}

int bgp_db_del_vrf_timers(const char *vrf_name)
{
    /* 重置为默认值 */
    return bgp_db_set_vrf_timers(vrf_name, BGP_TIMER_DEFAULT_KEEPALIVE, BGP_TIMER_DEFAULT_HOLD);
}

// ============================================================================
// connect-retry
// ============================================================================

int bgp_db_set_vrf_connect_retry(const char *vrf_name, uint16_t connect_retry)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !vrf_name)
    {
        return -1;
    }
    if (bgp_db_ensure_vrf_row(ctx, vrf_name) != 0)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_name);

    db_col_t cols[] = {
        DB_COL_INT("connect_retry", connect_retry),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_VRF, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write VRF %s connect-retry=%u", vrf_name, connect_retry);
        return -1;
    }

    LOG_INFO("BGP VRF %s connect-retry=%u written", vrf_name, connect_retry);
    return 0;
}

int bgp_db_del_vrf_connect_retry(const char *vrf_name)
{
    /* 重置为默认值 */
    return bgp_db_set_vrf_connect_retry(vrf_name, BGP_TIMER_DEFAULT_CONNECT_RETRY);
}

// ============================================================================
// 启动恢复
// ============================================================================

void bgp_db_restore_vrf(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_VRF, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *vrf_name = db_row_get_text(row, "vrf_name", VRF_PUBLIC_VRF_NAME);

        if (g_bgp_db_resync_only_vrf_bound && (!vrf_name || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0))
        {
            continue; /* re-sync 跳过 public VRF 行 */
        }

        if (vrf_name && strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) != 0)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_VRF_VIEW;
            apply.isNo = false;
            snprintf(apply.vrf_name, sizeof(apply.vrf_name), "%s", vrf_name);
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %s", vrf_name);
        }

        const char *router_id = db_row_get_text(row, "router_id", NULL);
        if (router_id && strcmp(router_id, "0.0.0.0") != 0)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_ROUTER_ID;
            apply.isNo = false;
            snprintf(apply.vrf_name, sizeof(apply.vrf_name), "%s", vrf_name);
            snprintf(apply.u.router_id.id, sizeof(apply.u.router_id.id), "%s", router_id);
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %s router-id=%s", vrf_name, router_id);
        }

        uint16_t keepalive = (uint16_t)db_row_get_int(row, "keepalive", BGP_TIMER_DEFAULT_KEEPALIVE);
        uint16_t hold_time = (uint16_t)db_row_get_int(row, "hold_time", BGP_TIMER_DEFAULT_HOLD);
        if (keepalive > 0 && hold_time > keepalive)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_TIMERS;
            apply.isNo = false;
            snprintf(apply.vrf_name, sizeof(apply.vrf_name), "%s", vrf_name);
            apply.u.timers.keepalive = keepalive;
            apply.u.timers.hold_time = hold_time;
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %s keepalive=%u hold=%u", vrf_name, keepalive, hold_time);
        }

        uint16_t connect_retry = (uint16_t)db_row_get_int(row, "connect_retry", BGP_TIMER_DEFAULT_CONNECT_RETRY);
        if (connect_retry > 0)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_CONNECT_RETRY;
            apply.isNo = false;
            snprintf(apply.vrf_name, sizeof(apply.vrf_name), "%s", vrf_name);
            apply.u.connect_retry.interval = connect_retry;
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %s connect-retry=%u", vrf_name, connect_retry);
        }
    }

    db_result_free(result);
}
