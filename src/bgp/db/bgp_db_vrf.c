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

// ============================================================================
// 表 schema
// ============================================================================

static const db_column_def_t BGP_VRF_COLS[] = {
    {"vrf_id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},     {"router_id", DB_TYPE_TEXT, DB_COL_NOT_NULL, "0.0.0.0"},
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

int bgp_db_set_vrf_router_id(uint32_t vrf_id, const char *router_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !router_id)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_id);

    db_col_t cols[] = {
        DB_COL_TEXT("router_id", router_id),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_VRF, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write VRF %u router-id=%s", vrf_id, router_id);
        return -1;
    }

    LOG_INFO("BGP VRF %u router-id=%s written", vrf_id, router_id);
    return 0;
}

int bgp_db_del_vrf_router_id(uint32_t vrf_id)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_id);

    int rows = db_rpc_delete(ctx, BGP_TABLE_VRF, &pk.filter);
    db_filter_clear(&pk);

    if (rows < 0)
    {
        LOG_ERROR("BGP failed to delete VRF %u router-id", vrf_id);
        return -1;
    }

    LOG_INFO("BGP VRF %u router-id deleted, affected rows: %d", vrf_id, rows);
    return rows;
}

// ============================================================================
// keepalive / hold_time
// ============================================================================

int bgp_db_set_vrf_timers(uint32_t vrf_id, uint16_t keepalive, uint16_t hold_time)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_id);

    db_col_t cols[] = {
        DB_COL_INT("keepalive", keepalive),
        DB_COL_INT("hold_time", hold_time),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_VRF, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write VRF %u timers keepalive=%u hold=%u", vrf_id, keepalive, hold_time);
        return -1;
    }

    LOG_INFO("BGP VRF %u timers keepalive=%u hold=%u written", vrf_id, keepalive, hold_time);
    return 0;
}

int bgp_db_del_vrf_timers(uint32_t vrf_id)
{
    /* 重置为默认值 */
    return bgp_db_set_vrf_timers(vrf_id, BGP_TIMER_DEFAULT_KEEPALIVE, BGP_TIMER_DEFAULT_HOLD);
}

// ============================================================================
// connect-retry
// ============================================================================

int bgp_db_set_vrf_connect_retry(uint32_t vrf_id, uint16_t connect_retry)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, vrf_id);

    db_col_t cols[] = {
        DB_COL_INT("connect_retry", connect_retry),
    };
    int rows = db_rpc_update_cols(ctx, BGP_TABLE_VRF, &pk.filter, cols, G_N_ELEMENTS(cols));
    db_filter_clear(&pk);

    if (rows <= 0)
    {
        LOG_ERROR("BGP failed to write VRF %u connect-retry=%u", vrf_id, connect_retry);
        return -1;
    }

    LOG_INFO("BGP VRF %u connect-retry=%u written", vrf_id, connect_retry);
    return 0;
}

int bgp_db_del_vrf_connect_retry(uint32_t vrf_id)
{
    /* 重置为默认值 */
    return bgp_db_set_vrf_connect_retry(vrf_id, BGP_TIMER_DEFAULT_CONNECT_RETRY);
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
        uint32_t vrf_id = (uint32_t)db_row_get_int(row, "vrf_id", BGP_VRF_PUBLIC_ID);

        const char *router_id = db_row_get_text(row, "router_id", NULL);
        if (router_id && strcmp(router_id, "0.0.0.0") != 0)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_ROUTER_ID;
            apply.isNo = false;
            apply.vrf_id = vrf_id;
            snprintf(apply.u.router_id.id, sizeof(apply.u.router_id.id), "%s", router_id);
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %u router-id=%s", vrf_id, router_id);
        }

        uint16_t keepalive = (uint16_t)db_row_get_int(row, "keepalive", BGP_TIMER_DEFAULT_KEEPALIVE);
        uint16_t hold_time = (uint16_t)db_row_get_int(row, "hold_time", BGP_TIMER_DEFAULT_HOLD);
        if (keepalive > 0 && hold_time > keepalive)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_TIMERS;
            apply.isNo = false;
            apply.vrf_id = vrf_id;
            apply.u.timers.keepalive = keepalive;
            apply.u.timers.hold_time = hold_time;
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %u keepalive=%u hold=%u", vrf_id, keepalive, hold_time);
        }

        uint16_t connect_retry = (uint16_t)db_row_get_int(row, "connect_retry", BGP_TIMER_DEFAULT_CONNECT_RETRY);
        if (connect_retry > 0)
        {
            bgp_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.group_id = BGP_CLI_GROUP_ID_CONNECT_RETRY;
            apply.isNo = false;
            apply.vrf_id = vrf_id;
            apply.u.connect_retry.interval = connect_retry;
            (void)bgp_worker_dispatch_apply(&apply);
            LOG_INFO("BGP restore: VRF %u connect-retry=%u", vrf_id, connect_retry);
        }
    }

    db_result_free(result);
}
