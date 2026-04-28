/**
 * @file   dev_db.c
 * @brief  DEV 模块配置持久化（log-level 等）
 * @author jhb
 * @date   2026/04/27
 */
#include "dev_db.h"

#include <glib.h>

#include "db.h"
#include "dev.h"
#include "dev_main.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 表定义：单行配置（id=1 行存放所有标量配置项）
// ============================================================================

static const db_column_def_t DEV_CONFIG_COLS[] = {
    {"id", DB_TYPE_INTEGER, DB_COL_PRIMARY_KEY, NULL},
    {"log_level", DB_TYPE_INTEGER, 0, NULL},
};

static const db_table_def_t DEV_CONFIG_TABLE = {
    .table_name = DEV_TABLE_CONFIG,
    .cols = DEV_CONFIG_COLS,
    .num_cols = G_N_ELEMENTS(DEV_CONFIG_COLS),
};

/** 主键过滤条件（id = 1） */
static db_filter_t make_pk_filter(db_condition_t *cond)
{
    cond->field_name = "id";
    cond->op = DB_CMP_EQ;
    cond->value = db_value_int(DEV_CONFIG_PK_VALUE);

    db_filter_t f = {.conditions = cond, .num_conditions = 1};
    return f;
}

// ============================================================================
// 公共 API
// ============================================================================

log_level_t dev_db_default_log_level(void)
{
#ifdef NDEBUG
    return LOG_LEVEL_WARN;
#else
    return LOG_LEVEL_DEBUG;
#endif
}

int dev_db_init(void)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (db_rpc_create_table_from_def(ctx, &DEV_CONFIG_TABLE) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("DEV: Failed to create table %s", DEV_TABLE_CONFIG);
        return -1;
    }
    return 0;
}

int dev_db_get_log_level(log_level_t *level)
{
    if (!level)
    {
        return -1;
    }

    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    db_condition_t cond;
    db_filter_t filter = make_pk_filter(&cond);

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, DEV_TABLE_CONFIG, NULL, 0, &filter, &result) != ERRCODE_SUCCESS)
    {
        return -1;
    }

    /* 空表：db 层会返回 result=NULL + ERRCODE_SUCCESS */
    if (!result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return 1;
    }

    int64_t v = db_row_get_int(result->rows[0], "log_level", -1);
    db_result_free(result);

    if (v < LOG_LEVEL_DEBUG || v > LOG_LEVEL_ERROR)
    {
        return -1;
    }
    *level = (log_level_t)v;
    return 0;
}

int dev_db_set_log_level(log_level_t level)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (!ctx)
    {
        LOG_ERROR("DEV: dev_db_set_log_level: ipc ctx is NULL");
        return -1;
    }

    db_condition_t cond;
    db_filter_t filter = make_pk_filter(&cond);

    /* 先查存在性：插入与更新走不同 SQL（db 层不支持 INSERT OR REPLACE） */
    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, DEV_TABLE_CONFIG, &filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        LOG_ERROR("DEV: dev_db_set_log_level: db_rpc_exists failed rc=%d", rc);
        return -1;
    }

    if (exists)
    {
        db_col_t cols[] = {DB_COL_INT("log_level", (int64_t)level)};
        /* db_rpc_update_cols 返回受影响行数，<0 才是失败 */
        rc = db_rpc_update_cols(ctx, DEV_TABLE_CONFIG, &filter, cols, G_N_ELEMENTS(cols));
        if (rc < 0)
        {
            LOG_ERROR("DEV: dev_db_set_log_level: update_cols failed rc=%d level=%d", rc, (int)level);
            return -1;
        }
    }
    else
    {
        db_col_t cols[] = {
            DB_COL_INT("id", DEV_CONFIG_PK_VALUE),
            DB_COL_INT("log_level", (int64_t)level),
        };
        rc = db_rpc_insert_cols(ctx, DEV_TABLE_CONFIG, cols, G_N_ELEMENTS(cols));
        if (rc != ERRCODE_SUCCESS)
        {
            LOG_ERROR("DEV: dev_db_set_log_level: insert_cols failed rc=%d level=%d", rc, (int)level);
            return -1;
        }
    }
    return 0;
}

int dev_db_restore(void)
{
    log_level_t level;
    int rc = dev_db_get_log_level(&level);

    if (rc == 0)
    {
        log_set_level(level);
        LOG_INFO("DEV: Restored log_level=%d from DB", (int)level);
        return 0;
    }

    if (rc == 1)
    {
        log_level_t def = dev_db_default_log_level();
        log_set_level(def);
        if (dev_db_set_log_level(def) != 0)
        {
            LOG_WARN("DEV: Failed to persist default log_level=%d", (int)def);
        }
        else
        {
            LOG_INFO("DEV: Initialized DB with default log_level=%d", (int)def);
        }
        return 0;
    }

    LOG_ERROR("DEV: Failed to query log_level from DB");
    return -1;
}
