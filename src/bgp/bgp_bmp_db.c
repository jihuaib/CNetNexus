/**
 * @file   bgp_bmp_db.c
 * @brief  BGP BMP 上报功能数据库操作实现
 * @author jhb
 * @date   2026/03/29
 */
#include "bgp_bmp_db.h"

#include <string.h>

#include "bgp_bmp_cli.h"
#include "bgp_bmp_thread.h"
#include "bgp_main.h"
#include "bgp_worker.h"
#include "db.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

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
// 辅助：按 instance_name 更新单列
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

    db_record_t *rec = db_record_new();
    db_record_set_int(rec, col_name, value);

    int rows = db_rpc_update_record(ctx, BGP_TABLE_BMP_INSTANCE, rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);
    return rows > 0 ? 0 : -1;
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

    db_record_t *rec = db_record_new();
    db_record_set_text(rec, col_name, value);

    int rows = db_rpc_update_record(ctx, BGP_TABLE_BMP_INSTANCE, rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);
    return rows > 0 ? 0 : -1;
}

// ============================================================================
// BMP 实例 CRUD
// ============================================================================

int bgp_bmp_db_set_instance(const char *instance_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();

    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "instance_name", instance_name);
    db_record_set_text(rec, "collector_ip", "");
    db_record_set_int(rec, "collector_port", 0);
    db_record_set_int(rec, "stats_interval", 0);
    db_record_set_int(rec, "reconnect_interval", 30);
    db_record_set_int(rec, "monitor_all", 1);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_BMP_INSTANCE, rec, NULL);
    db_record_free(rec);

    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BMP failed to write instance %s", instance_name);
        return -1;
    }
    return 0;
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
    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "instance_name", instance_name);
    db_record_set_text(rec, "neighbor_ip", neighbor_ip);

    int ret = db_rpc_upsert(ctx, BGP_TABLE_BMP_MONITOR, rec, NULL);
    db_record_free(rec);

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

// ============================================================================
// 启动恢复
// ============================================================================

/**
 * @brief 恢复单个 BMP 实例的监控邻居配置
 */
static void restore_bmp_monitors(const char *inst_name)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    db_condition_t cond = {
        .field_name = "instance_name",
        .op = DB_CMP_EQ,
        .value = db_value_text(inst_name),
    };
    db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
    db_result_t *result = NULL;

    if (db_rpc_query(ctx, BGP_TABLE_BMP_MONITOR, NULL, 0, &filter, &result) != ERRCODE_SUCCESS || !result)
    {
        db_value_free(&cond.value);
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        const char *ip = db_row_get_text(result->rows[i], "neighbor_ip", NULL);
        if (!ip)
        {
            continue;
        }

        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_BMP_MONITOR;
        apply.isNo = false;
        apply.u.bmp_monitor.is_all = FALSE;
        g_strlcpy(apply.u.bmp_monitor.inst_name, inst_name, sizeof(apply.u.bmp_monitor.inst_name));
        if (net_addr_from_str(ip, &apply.u.bmp_monitor.addr) != 0)
        {
            LOG_WARN("BMP restore: invalid monitor peer IP '%s', skipping", ip);
            continue;
        }

        if (bgp_bmp_dispatch_apply(&apply) != 0 || apply.rc == BGP_APPLY_RC_FAIL)
        {
            LOG_WARN("BMP restore: monitor peer %s apply failed for instance '%s'", ip, inst_name);
        }
    }

    db_result_free(result);
    db_value_free(&cond.value);
}

void bgp_bmp_db_restore(void)
{
    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, BGP_TABLE_BMP_INSTANCE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result ||
        result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *name = db_row_get_text(row, "instance_name", NULL);
        if (!name)
        {
            continue;
        }

        /* 1. 创建实例 */
        bgp_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.group_id = BGP_CLI_GROUP_ID_BMP_INSTANCE;
        apply.isNo = false;
        g_strlcpy(apply.u.bmp_instance.name, name, sizeof(apply.u.bmp_instance.name));

        if (bgp_bmp_dispatch_apply(&apply) != 0 || apply.rc == BGP_APPLY_RC_FAIL)
        {
            LOG_WARN("BMP restore: instance '%s' creation failed, skipping", name);
            continue;
        }

        /* 2. 恢复 collector */
        const char *collector_ip = db_row_get_text(row, "collector_ip", "");
        int64_t collector_port = db_row_get_int(row, "collector_port", 0);
        if (collector_ip[0] != '\0' && collector_port > 0)
        {
            bgp_apply_cmd_t col_apply;
            memset(&col_apply, 0, sizeof(col_apply));
            col_apply.group_id = BGP_CLI_GROUP_ID_BMP_COLLECTOR;
            col_apply.isNo = false;
            g_strlcpy(col_apply.u.bmp_collector.inst_name, name, sizeof(col_apply.u.bmp_collector.inst_name));
            col_apply.u.bmp_collector.port = (uint16_t)collector_port;
            if (net_addr_from_str(collector_ip, &col_apply.u.bmp_collector.addr) == 0)
            {
                (void)bgp_bmp_dispatch_apply(&col_apply);
            }
        }

        /* 3. 恢复 stats interval */
        int64_t stats_interval = db_row_get_int(row, "stats_interval", 0);
        if (stats_interval > 0)
        {
            bgp_apply_cmd_t stats_apply;
            memset(&stats_apply, 0, sizeof(stats_apply));
            stats_apply.group_id = BGP_CLI_GROUP_ID_BMP_STATS_INTERVAL;
            stats_apply.isNo = false;
            g_strlcpy(stats_apply.u.bmp_stats.inst_name, name, sizeof(stats_apply.u.bmp_stats.inst_name));
            stats_apply.u.bmp_stats.interval = (uint16_t)stats_interval;
            (void)bgp_bmp_dispatch_apply(&stats_apply);
        }

        /* 4. 恢复 reconnect interval */
        int64_t reconnect_interval = db_row_get_int(row, "reconnect_interval", 30);
        if (reconnect_interval != 30)
        {
            bgp_apply_cmd_t recon_apply;
            memset(&recon_apply, 0, sizeof(recon_apply));
            recon_apply.group_id = BGP_CLI_GROUP_ID_BMP_RECONNECT;
            recon_apply.isNo = false;
            g_strlcpy(recon_apply.u.bmp_reconnect.inst_name, name, sizeof(recon_apply.u.bmp_reconnect.inst_name));
            recon_apply.u.bmp_reconnect.interval = (uint16_t)reconnect_interval;
            (void)bgp_bmp_dispatch_apply(&recon_apply);
        }

        /* 5. 恢复 monitor 配置 */
        int64_t monitor_all = db_row_get_int(row, "monitor_all", 1);
        if (monitor_all)
        {
            /* 默认就是 all，无需额外操作 */
        }
        else
        {
            restore_bmp_monitors(name);
        }

        LOG_INFO("BMP instance '%s' restored from database", name);
    }

    db_result_free(result);
}
