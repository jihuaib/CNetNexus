/**
 * @file   bgp_db.c
 * @brief  BGP 模块数据库操作实现（封装 db_rpc 调用）
 * @author jhb
 * @date   2026/02/23
 */
#include "bgp_db.h"

#include <stdio.h>
#include <string.h>

#include "db_rpc.h"
#include "errcode.h"
#include "log.h"

/* BGP 协议配置表名 */
#define BGP_TABLE_PROTOCOL "bgp_protocol"

/* bgp_protocol 表列定义 */
static const db_column_def_t BGP_PROTOCOL_COLS[] = {
    {"as_number", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
};

/* bgp_protocol 表定义 */
static const db_table_def_t BGP_PROTOCOL_TABLE = {
    .table_name = BGP_TABLE_PROTOCOL,
    .cols = BGP_PROTOCOL_COLS,
    .num_cols = 1,
};

// ============================================================================
// 建表初始化
// ============================================================================

int bgp_db_init(ipc_context_t *ctx)
{
    if (!ctx)
    {
        return -1;
    }

    int ret = db_rpc_create_table_from_def(ctx, &BGP_PROTOCOL_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 建表失败: %s", BGP_TABLE_PROTOCOL);
        return -1;
    }

    LOG_INFO("BGP 数据库表 %s 已就绪", BGP_TABLE_PROTOCOL);
    return 0;
}

// ============================================================================
// 写入（插入或更新）AS 号
// ============================================================================

int bgp_db_set_as(ipc_context_t *ctx, uint32_t as_number)
{
    if (!ctx)
    {
        return -1;
    }

    /* 检查是否已存在 */
    gboolean exists = FALSE;
    int ret = db_rpc_exists(ctx, BGP_TABLE_PROTOCOL, NULL, &exists);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 查询 AS 存在性失败");
        return -1;
    }

    const char *field_names[] = {"as_number"};
    db_value_t values[] = {db_value_int((int64_t)as_number)};

    if (exists)
    {
        int rows = db_rpc_update(ctx, BGP_TABLE_PROTOCOL, field_names, values, 1, NULL);
        if (rows < 0)
        {
            LOG_ERROR("BGP 更新 AS 号 %u 失败", as_number);
            return -1;
        }
        LOG_INFO("BGP AS 号已更新为 %u", as_number);
    }
    else
    {
        ret = db_rpc_insert(ctx, BGP_TABLE_PROTOCOL, field_names, values, 1);
        if (ret != ERRCODE_SUCCESS)
        {
            LOG_ERROR("BGP 插入 AS 号 %u 失败", as_number);
            return -1;
        }
        LOG_INFO("BGP AS 号 %u 已写入", as_number);
    }

    return 0;
}

// ============================================================================
// 删除 AS 配置
// ============================================================================

int bgp_db_del_as(ipc_context_t *ctx, uint32_t as_number, int has_as)
{
    if (!ctx)
    {
        return -1;
    }

    int rows;
    if (has_as)
    {
        char where[64];
        snprintf(where, sizeof(where), "as_number = %u", as_number);
        rows = db_rpc_delete(ctx, BGP_TABLE_PROTOCOL, where);
    }
    else
    {
        rows = db_rpc_delete(ctx, BGP_TABLE_PROTOCOL, NULL);
    }

    if (rows < 0)
    {
        LOG_ERROR("BGP 删除 AS 配置失败");
        return -1;
    }

    LOG_INFO("BGP 删除 AS 配置，影响行数: %d", rows);
    return rows;
}

// ============================================================================
// 查询全部 BGP 配置
// ============================================================================

int bgp_db_query(ipc_context_t *ctx, db_result_t **result)
{
    if (!ctx || !result)
    {
        return -1;
    }

    int ret = db_rpc_query(ctx, BGP_TABLE_PROTOCOL, NULL, 0, NULL, result);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP 查询配置失败");
        return -1;
    }

    return 0;
}
