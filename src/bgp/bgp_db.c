/**
 * @file   bgp_db.c
 * @brief  BGP 数据库协调层：建表、默认值、启动恢复
 *
 * 各表的 schema 与 CRUD/restore 实现位于 src/bgp/db/bgp_db_<table>.c。
 * 本文件只负责把建表与恢复入口串起来。
 *
 * @author jhb
 * @date   2026/02/23
 */
#include "bgp_db.h"

#include "bgp_bmp_db.h"
#include "bgp_main.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "db.h"
#include "db/bgp_db_internal.h"
#include "errcode.h"
#include "log.h"
#include "vrf.h"

// ============================================================================
// 启动恢复
// ============================================================================

uint32_t bgp_db_restore(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }

    uint32_t ret = bgp_db_restore_protocol();
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    if (!g_bgp_local || !g_bgp_work_local->protocol)
    {
        return ERRCODE_SUCCESS;
    }

    bgp_db_restore_vrf();
    bgp_db_restore_sessions();
    bgp_db_restore_instances();
    bgp_db_restore_neighbors();
    bgp_db_restore_qp_routes();
    bgp_db_restore_qp_route_select();
    bgp_bmp_db_restore();

    return ERRCODE_SUCCESS;
}

// ============================================================================
// 建表初始化
// ============================================================================

int bgp_db_init(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    static const db_table_def_t *const BGP_TABLES[] = {
        &BGP_PROTOCOL_TABLE, &BGP_VRF_TABLE,      &BGP_SESSION_TABLE,
        &BGP_INSTANCE_TABLE, &BGP_NEIGHBOR_TABLE, &BGP_QP_ROUTE_TABLE,
    };

    for (size_t i = 0; i < G_N_ELEMENTS(BGP_TABLES); i++)
    {
        int ret = db_rpc_create_table_from_def(ctx, BGP_TABLES[i]);
        if (ret != ERRCODE_SUCCESS)
        {
            LOG_ERROR("BGP table creation failed: %s", BGP_TABLES[i]->table_name);
            return -1;
        }
        LOG_INFO("BGP database table %s ready", BGP_TABLES[i]->table_name);
    }

    /* BMP 上报功能表 */
    int bmp_count = 0;
    const void **bmp_tables = bgp_bmp_db_get_tables(&bmp_count);
    for (int i = 0; i < bmp_count; i++)
    {
        const db_table_def_t *tbl = (const db_table_def_t *)bmp_tables[i];
        int ret = db_rpc_create_table_from_def(ctx, tbl);
        if (ret != ERRCODE_SUCCESS)
        {
            LOG_ERROR("BMP table creation failed: %s", tbl->table_name);
            return -1;
        }
        LOG_INFO("BMP database table %s ready", tbl->table_name);
    }

    /* 默认公网 VRF 行：保证后续 router-id/timers/connect-retry 走纯 UPDATE 路径都能命中 */
    db_filter_builder_t pk;
    bgp_db_vrf_pk(&pk, VRF_PUBLIC_VRF_NAME);
    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, BGP_TABLE_VRF, &pk.filter, &exists);
    db_filter_clear(&pk);
    if (rc == ERRCODE_SUCCESS && !exists)
    {
        db_col_t cols[] = {DB_COL_TEXT("vrf_name", VRF_PUBLIC_VRF_NAME)};
        if (db_rpc_insert_cols(ctx, BGP_TABLE_VRF, cols, G_N_ELEMENTS(cols)) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("BGP failed to seed default VRF row vrf=%s", VRF_PUBLIC_VRF_NAME);
            return -1;
        }
        LOG_INFO("BGP default VRF %s row seeded", VRF_PUBLIC_VRF_NAME);
    }

    return 0;
}

// ============================================================================
// 默认值写入
// ============================================================================

/**
 * @brief 检查指定表是否为空（查询失败按空表处理，保守写入默认值）
 */
static gboolean table_is_empty(const char *table_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return TRUE;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, table_name, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        return TRUE;
    }
    gboolean empty = (result->num_rows == 0);
    db_result_free(result);
    return empty;
}

/**
 * @brief 按表逐一检查并写入各自的默认值
 *
 * 每张表独立判断：为空则写入，非空则跳过，互不影响。
 */
static void write_defaults(void)
{
    if (table_is_empty(BGP_TABLE_PROTOCOL))
    {
        LOG_INFO("BGP %s table empty, writing default config", BGP_TABLE_PROTOCOL);
    }

    if (table_is_empty(BGP_TABLE_SESSION))
    {
        LOG_INFO("BGP %s table empty, writing default config", BGP_TABLE_SESSION);
    }

    if (table_is_empty(BGP_TABLE_NEIGHBOR))
    {
        LOG_INFO("BGP %s table empty, writing default config", BGP_TABLE_NEIGHBOR);
    }

    if (table_is_empty(BGP_TABLE_VRF))
    {
        LOG_INFO("BGP %s table empty, writing default VRF timers", BGP_TABLE_VRF);
        bgp_db_set_vrf_timers(VRF_PUBLIC_VRF_NAME, BGP_TIMER_DEFAULT_KEEPALIVE, BGP_TIMER_DEFAULT_HOLD);
        bgp_db_set_vrf_connect_retry(VRF_PUBLIC_VRF_NAME, BGP_TIMER_DEFAULT_CONNECT_RETRY);
    }
}

void bgp_db_ensure_defaults(void)
{
    if (!bgp_local_ipc_ctx())
    {
        return;
    }
    write_defaults();
}
