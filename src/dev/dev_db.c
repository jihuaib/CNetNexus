/**
 * @file   dev_db.c
 * @brief  DEV 模块配置持久化（log-level 等）
 * @author jhb
 * @date   2026/04/27
 */
#include "dev_db.h"

#include <errno.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
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
    {"sysname", DB_TYPE_TEXT, 0, NULL},
    {"syslog_server", DB_TYPE_TEXT, 0, NULL},
    {"syslog_port", DB_TYPE_INTEGER, 0, NULL},
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

int dev_db_get_sysname(char *out, size_t cap)
{
    if (!out || cap == 0)
    {
        return -1;
    }
    out[0] = '\0';

    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    db_condition_t cond;
    db_filter_t filter = make_pk_filter(&cond);

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, DEV_TABLE_CONFIG, NULL, 0, &filter, &result) != ERRCODE_SUCCESS)
    {
        return -1;
    }
    if (!result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return 1; /* 不存在 */
    }
    const char *v = db_row_get_text(result->rows[0], "sysname", NULL);
    if (v && v[0] != '\0')
    {
        g_strlcpy(out, v, cap);
    }
    db_result_free(result);
    return 0;
}

int dev_db_set_sysname(const char *sysname)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_condition_t cond;
    db_filter_t filter = make_pk_filter(&cond);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, DEV_TABLE_CONFIG, &filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        return -1;
    }

    const char *v = sysname ? sysname : "";
    if (exists)
    {
        db_col_t cols[] = {DB_COL_TEXT("sysname", v)};
        rc = db_rpc_update_cols(ctx, DEV_TABLE_CONFIG, &filter, cols, G_N_ELEMENTS(cols));
        return (rc < 0) ? -1 : 0;
    }
    db_col_t cols[] = {
        DB_COL_INT("id", DEV_CONFIG_PK_VALUE),
        DB_COL_TEXT("sysname", v),
    };
    rc = db_rpc_insert_cols(ctx, DEV_TABLE_CONFIG, cols, G_N_ELEMENTS(cols));
    return (rc != ERRCODE_SUCCESS) ? -1 : 0;
}

int dev_db_get_syslog_remote(syslog_report_remote_config_t *out)
{
    if (!out)
    {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    db_condition_t cond;
    db_filter_t filter = make_pk_filter(&cond);

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, DEV_TABLE_CONFIG, NULL, 0, &filter, &result) != ERRCODE_SUCCESS)
    {
        return -1;
    }
    if (!result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return 1;
    }

    const char *server = db_row_get_text(result->rows[0], "syslog_server", NULL);
    int64_t port = db_row_get_int(result->rows[0], "syslog_port", 0);
    if (server && server[0] != '\0' && port > 0 && port <= 65535)
    {
        out->enabled = 1u;
        out->port = (uint32_t)port;
        g_strlcpy(out->server, server, sizeof(out->server));
    }
    db_result_free(result);
    return 0;
}

int dev_db_set_syslog_remote(const char *server, uint16_t port)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (!ctx)
    {
        return -1;
    }

    db_condition_t cond;
    db_filter_t filter = make_pk_filter(&cond);

    gboolean exists = FALSE;
    int rc = db_rpc_exists(ctx, DEV_TABLE_CONFIG, &filter, &exists);
    if (rc != ERRCODE_SUCCESS)
    {
        return -1;
    }

    const char *stored_server = (server && server[0] != '\0' && port != 0) ? server : "";
    int64_t stored_port = (stored_server[0] != '\0') ? (int64_t)port : 0;
    if (exists)
    {
        db_col_t cols[] = {
            DB_COL_TEXT("syslog_server", stored_server),
            DB_COL_INT("syslog_port", stored_port),
        };
        rc = db_rpc_update_cols(ctx, DEV_TABLE_CONFIG, &filter, cols, G_N_ELEMENTS(cols));
        return (rc < 0) ? -1 : 0;
    }

    db_col_t cols[] = {
        DB_COL_INT("id", DEV_CONFIG_PK_VALUE),
        DB_COL_TEXT("syslog_server", stored_server),
        DB_COL_INT("syslog_port", stored_port),
    };
    rc = db_rpc_insert_cols(ctx, DEV_TABLE_CONFIG, cols, G_N_ELEMENTS(cols));
    return (rc != ERRCODE_SUCCESS) ? -1 : 0;
}

/**
 * @brief 将 sysname 推送给 CLI 模块（让其覆盖默认 "NetNexus"）
 */
static void push_sysname_to_cli(const char *sysname)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    if (!ctx)
    {
        return;
    }
    const char *v = (sysname && sysname[0] != '\0') ? sysname : "";
    char *payload = g_strdup(v);
    if (!payload)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(CLI_MSG_TYPE_SYSNAME_UPDATE, DEV_MODULE_ID_DEV, DEV_MODULE_ID_CLI, 0,
                                                  payload, (uint32_t)(strlen(v) + 1), g_free);
    if (!m)
    {
        g_free(payload);
        return;
    }
    if (dev_ipc_send(ctx, DEV_MODULE_ID_CLI, m) != ERRCODE_SUCCESS)
    {
        LOG_WARN("DEV: failed to push sysname to CLI from restore");
    }
    dev_ipc_message_free(m);
}

static void restore_kernel_hostname(const char *sysname)
{
    const char *v = (sysname && sysname[0] != '\0') ? sysname : CLI_SYSNAME_DEFAULT;
    if (sethostname(v, strlen(v)) != 0)
    {
        LOG_WARN("DEV: failed to restore kernel hostname to %s: %s", v, strerror(errno));
    }
}

int dev_db_restore(void)
{
    syslog_report_remote_config_t syslog_cfg;
    int syslog_rc = dev_db_get_syslog_remote(&syslog_cfg);
    if (syslog_rc == 0 && syslog_cfg.enabled)
    {
        syslog_report_set_remote(syslog_cfg.server, (uint16_t)syslog_cfg.port);
        LOG_INFO("DEV: Restored syslog remote=%s:%u from DB", syslog_cfg.server, (unsigned)syslog_cfg.port);
    }
    else
    {
        syslog_report_disable_remote();
    }

    char sysname[64] = {0};
    int sysname_rc = dev_db_get_sysname(sysname, sizeof(sysname));
    if (sysname_rc == 0 && sysname[0] != '\0')
    {
        push_sysname_to_cli(sysname);
        LOG_INFO("DEV: Restored sysname=%s from DB", sysname);
    }
    restore_kernel_hostname((sysname_rc == 0) ? sysname : "");

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
