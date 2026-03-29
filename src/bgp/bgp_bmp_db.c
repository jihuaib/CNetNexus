/**
 * @file   bgp_bmp_db.c
 * @brief  BGP BMP 上报功能数据库操作实现
 * @author jhb
 * @date   2026/03/29
 */
#include "bgp_bmp_db.h"

#include <string.h>

#include "bgp_main.h"
#include "db.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 表定义
// ============================================================================

/* bgp_bmp_instance 表列定义 */
static const db_column_def_t BMP_INSTANCE_COLS[] = {
    {"instance_name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"collector_ip", DB_TYPE_TEXT, DB_COL_NONE, ""},
    {"collector_port", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"stats_interval", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
    {"reconnect_interval", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "30"},
    {"monitor_all", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "1"},
};

static const db_table_def_t BMP_INSTANCE_TABLE = {
    .table_name = BGP_TABLE_BMP_INSTANCE,
    .cols = BMP_INSTANCE_COLS,
    .num_cols = G_N_ELEMENTS(BMP_INSTANCE_COLS),
};

/* bgp_bmp_monitor 表列定义 */
static const db_column_def_t BMP_MONITOR_COLS[] = {
    {"instance_name", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"neighbor_ip", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
};

static const db_table_def_t BMP_MONITOR_TABLE = {
    .table_name = BGP_TABLE_BMP_MONITOR,
    .cols = BMP_MONITOR_COLS,
    .num_cols = G_N_ELEMENTS(BMP_MONITOR_COLS),
};

/* 供 bgp_db_init() 批量建表使用的指针数组 */
static const db_table_def_t *BMP_TABLES[] = {
    &BMP_INSTANCE_TABLE,
    &BMP_MONITOR_TABLE,
};

const void **bgp_bmp_db_get_tables(int *count)
{
    if (count)
    {
        *count = (int)G_N_ELEMENTS(BMP_TABLES);
    }
    return (const void **)BMP_TABLES;
}

// ============================================================================
// 辅助：更新单列
// ============================================================================

/**
 * @brief 按 instance_name 更新单列（整数值）
 */
static int update_int_col(const char *instance_name, const char *col_name, int64_t value)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {
        .field_name = "instance_name",
        .op = DB_CMP_EQ,
        .value = db_value_text(instance_name),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};

    db_row_data_t upd = {.field_name = col_name, .value = db_value_int(value)};
    int ret = db_rpc_update(ctx, BGP_TABLE_BMP_INSTANCE, &upd, 1, &filter);
    db_value_free(&cond.value);
    db_value_free(&upd.value);
    return ret >= 0 ? 0 : -1;
}

/**
 * @brief 按 instance_name 更新单列（文本值）
 */
static int update_text_col(const char *instance_name, const char *col_name, const char *value)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {
        .field_name = "instance_name",
        .op = DB_CMP_EQ,
        .value = db_value_text(instance_name),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};

    db_row_data_t upd = {.field_name = col_name, .value = db_value_text(value)};
    int ret = db_rpc_update(ctx, BGP_TABLE_BMP_INSTANCE, &upd, 1, &filter);
    db_value_free(&cond.value);
    db_value_free(&upd.value);
    return ret >= 0 ? 0 : -1;
}

// ============================================================================
// BMP 实例 CRUD
// ============================================================================

int bgp_bmp_db_set_instance(const char *instance_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_row_data_t row[] = {
        {.field_name = "instance_name", .value = db_value_text(instance_name)},
        {.field_name = "collector_ip", .value = db_value_text("")},
        {.field_name = "collector_port", .value = db_value_int(0)},
        {.field_name = "stats_interval", .value = db_value_int(0)},
        {.field_name = "reconnect_interval", .value = db_value_int(30)},
        {.field_name = "monitor_all", .value = db_value_int(1)},
    };
    int ret = db_rpc_upsert(ctx, BGP_TABLE_BMP_INSTANCE, row, G_N_ELEMENTS(row));
    for (size_t i = 0; i < G_N_ELEMENTS(row); i++)
    {
        db_value_free(&row[i].value);
    }
    return ret == ERRCODE_SUCCESS ? 0 : -1;
}

int bgp_bmp_db_del_instance(const char *instance_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();

    /* 先删监控邻居 */
    db_condition_t cond = {
        .field_name = "instance_name",
        .op = DB_CMP_EQ,
        .value = db_value_text(instance_name),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    int rows_mon = db_rpc_delete(ctx, BGP_TABLE_BMP_MONITOR, &filter);
    db_value_free(&cond.value);

    /* 再删实例 */
    db_condition_t cond2 = {
        .field_name = "instance_name",
        .op = DB_CMP_EQ,
        .value = db_value_text(instance_name),
    };
    db_filter_t filter2 = {.conditions = &cond2, .num_conditions = 1};
    int rows_inst = db_rpc_delete(ctx, BGP_TABLE_BMP_INSTANCE, &filter2);
    db_value_free(&cond2.value);

    if (rows_inst < 0)
    {
        return -1;
    }
    return rows_inst + (rows_mon > 0 ? rows_mon : 0);
}

// ============================================================================
// Collector 配置
// ============================================================================

int bgp_bmp_db_set_collector(const char *instance_name, const char *ip, uint16_t port)
{
    if (update_text_col(instance_name, "collector_ip", ip) != 0)
    {
        return -1;
    }
    return update_int_col(instance_name, "collector_port", port);
}

int bgp_bmp_db_del_collector(const char *instance_name)
{
    if (update_text_col(instance_name, "collector_ip", "") != 0)
    {
        return -1;
    }
    return update_int_col(instance_name, "collector_port", 0);
}

// ============================================================================
// Stats Report 间隔
// ============================================================================

int bgp_bmp_db_set_stats_interval(const char *instance_name, uint16_t interval)
{
    return update_int_col(instance_name, "stats_interval", interval);
}

int bgp_bmp_db_del_stats_interval(const char *instance_name)
{
    return update_int_col(instance_name, "stats_interval", 0);
}

// ============================================================================
// 重连间隔
// ============================================================================

int bgp_bmp_db_set_reconnect_interval(const char *instance_name, uint16_t interval)
{
    return update_int_col(instance_name, "reconnect_interval", interval);
}

int bgp_bmp_db_del_reconnect_interval(const char *instance_name)
{
    return update_int_col(instance_name, "reconnect_interval", 30);
}

// ============================================================================
// 监控邻居
// ============================================================================

int bgp_bmp_db_set_monitor_all(const char *instance_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();

    /* 清除特定邻居列表 */
    db_condition_t cond = {
        .field_name = "instance_name",
        .op = DB_CMP_EQ,
        .value = db_value_text(instance_name),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_rpc_delete(ctx, BGP_TABLE_BMP_MONITOR, &filter);
    db_value_free(&cond.value);

    /* 设置 monitor_all=1 */
    return update_int_col(instance_name, "monitor_all", 1);
}

int bgp_bmp_db_add_monitor_peer(const char *instance_name, const char *neighbor_ip)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();

    /* 先将 monitor_all 设为 0 */
    if (update_int_col(instance_name, "monitor_all", 0) != 0)
    {
        return -1;
    }

    /* 插入监控邻居（幂等） */
    db_row_data_t row[] = {
        {.field_name = "instance_name", .value = db_value_text(instance_name)},
        {.field_name = "neighbor_ip", .value = db_value_text(neighbor_ip)},
    };
    int ret = db_rpc_upsert(ctx, BGP_TABLE_BMP_MONITOR, row, G_N_ELEMENTS(row));
    for (size_t i = 0; i < G_N_ELEMENTS(row); i++)
    {
        db_value_free(&row[i].value);
    }
    return ret == ERRCODE_SUCCESS ? 0 : -1;
}

int bgp_bmp_db_del_monitor_peer(const char *instance_name, const char *neighbor_ip)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t conds[] = {
        {.field_name = "instance_name", .op = DB_CMP_EQ, .value = db_value_text(instance_name)},
        {.field_name = "neighbor_ip", .op = DB_CMP_EQ, .value = db_value_text(neighbor_ip)},
    };
    db_filter_t filter = {.conditions = conds, .num_conditions = G_N_ELEMENTS(conds)};
    int rows = db_rpc_delete(ctx, BGP_TABLE_BMP_MONITOR, &filter);
    for (size_t i = 0; i < G_N_ELEMENTS(conds); i++)
    {
        db_value_free(&conds[i].value);
    }
    return rows;
}
