/**
 * @file   db_cli.c
 * @brief  数据库模块 CLI 命令处理（直接查询 SQLite，无需 registry）
 * @author jhb
 * @date   2026/01/22
 */
#include "db_cli.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "config_capture.h"
#include "db.h"
#include "db_config.h"
#include "db_main.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

#define DB_CLI_REPLAY_FAILURES_FILE "startup-replay-failures.log"

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void db_send_cli_response_to(uint32_t dst_module_id, uint32_t request_id, const char *text);

static void db_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    if (!msg)
    {
        return;
    }
    db_send_cli_response_to(msg->src_module_id, msg->request_id, text);
}

static void db_send_cli_response_to(uint32_t dst_module_id, uint32_t request_id, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_DB, dst_module_id, request_id,
                                                     resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(db_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(resp_data);
    }
}

static void db_cli_set_err(char **err, const char *msg)
{
    if (err)
    {
        *err = g_strdup(msg);
    }
}

// ============================================================================
// show db table-list
// ============================================================================

static int handle_db_show_table_list(dev_ipc_message_t *msg)
{
    db_connection_t *conn = g_db_local->main_conn;
    if (!conn || !conn->handle)
    {
        db_send_cli_response(msg, "Error: Database not open.\r\n");
        return ERRCODE_FAIL;
    }

    GString *resp_out = g_string_new("");
    if (!resp_out)
    {
        db_send_cli_response(msg, "Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    g_string_append_printf(resp_out,
                           "Tables in running.db:\r\n"
                           "  %-40s\r\n"
                           "  ----------------------------------------\r\n",
                           "Name");

    const char *sql = "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;";
    sqlite3_stmt *stmt;

    g_mutex_lock(&conn->db_mutex);
    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        g_mutex_unlock(&conn->db_mutex);
        db_send_cli_response(msg, "Error: Failed to query table list.\r\n");
        g_string_free(resp_out, TRUE);
        return ERRCODE_FAIL;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        g_string_append_printf(resp_out, "  %s\r\n", name ? name : "");
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    return cli_chunk_stream_start(&g_db_local->show_stream, db_local_ipc_ctx(), DEV_MODULE_ID_DB, msg, resp_out);
}

// ============================================================================
// show db table-field <table-name>
// ============================================================================

static int handle_db_show_table_field(dev_ipc_message_t *msg, const char *table_name)
{
    db_connection_t *conn = g_db_local->main_conn;
    if (!conn || !conn->handle)
    {
        db_send_cli_response(msg, "Error: Database not open.\r\n");
        return ERRCODE_FAIL;
    }

    GString *resp_out = g_string_new("");
    if (!resp_out)
    {
        db_send_cli_response(msg, "Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    g_string_append_printf(resp_out,
                           "Fields of table '%s':\r\n"
                           "  %-4s  %-24s  %-12s  %-8s  %s\r\n"
                           "  -------------------------------------------------------\r\n",
                           table_name, "cid", "name", "type", "notnull", "pk");

    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table_name);
    sqlite3_stmt *stmt;

    g_mutex_lock(&conn->db_mutex);
    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        g_mutex_unlock(&conn->db_mutex);
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "Error: Table '%s' not found.\r\n", table_name);
        db_send_cli_response(msg, errbuf);
        g_string_free(resp_out, TRUE);
        return ERRCODE_FAIL;
    }

    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int cid = sqlite3_column_int(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *type = (const char *)sqlite3_column_text(stmt, 2);
        int notnull = sqlite3_column_int(stmt, 3);
        int pk = sqlite3_column_int(stmt, 5);
        g_string_append_printf(resp_out, "  %-4d  %-24s  %-12s  %-8d  %d\r\n", cid, name ? name : "", type ? type : "",
                               notnull, pk);
        row_count++;
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (row_count == 0)
    {
        g_string_printf(resp_out, "Error: Table '%s' not found.\r\n", table_name);
    }

    return cli_chunk_stream_start(&g_db_local->show_stream, db_local_ipc_ctx(), DEV_MODULE_ID_DB, msg, resp_out);
}

// ============================================================================
// show db table-data <table-name>
// ============================================================================

static int handle_db_show_table_data(dev_ipc_message_t *msg, const char *table_name)
{
    db_connection_t *conn = g_db_local->main_conn;
    if (!conn || !conn->handle)
    {
        db_send_cli_response(msg, "Error: Database not open.\r\n");
        return ERRCODE_FAIL;
    }

    GString *resp_out = g_string_new("");
    if (!resp_out)
    {
        db_send_cli_response(msg, "Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s;", table_name);
    sqlite3_stmt *stmt;

    g_mutex_lock(&conn->db_mutex);
    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        g_mutex_unlock(&conn->db_mutex);
        g_string_printf(resp_out, "Error: Table '%s' not found.\r\n", table_name);
        db_send_cli_response(msg, resp_out->str);
        g_string_free(resp_out, TRUE);
        return ERRCODE_FAIL;
    }

    int col_count = sqlite3_column_count(stmt);

    /* 输出列头 */
    g_string_append_printf(resp_out, "Table: %s\r\n  ", table_name);
    for (int c = 0; c < col_count; c++)
    {
        g_string_append_printf(resp_out, "%-20s", sqlite3_column_name(stmt, c));
    }
    g_string_append(resp_out, "\r\n  ");
    for (int c = 0; c < col_count; c++)
    {
        g_string_append(resp_out, "--------------------");
    }
    g_string_append(resp_out, "\r\n");

    /* 输出数据行 */
    uint32_t row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        g_string_append(resp_out, "  ");
        for (int c = 0; c < col_count; c++)
        {
            int col_type = sqlite3_column_type(stmt, c);
            char val_buf[64];
            switch (col_type)
            {
                case SQLITE_INTEGER:
                    snprintf(val_buf, sizeof(val_buf), "%lld", (long long)sqlite3_column_int64(stmt, c));
                    break;
                case SQLITE_FLOAT:
                    snprintf(val_buf, sizeof(val_buf), "%.6g", sqlite3_column_double(stmt, c));
                    break;
                case SQLITE_TEXT:
                {
                    const unsigned char *txt = sqlite3_column_text(stmt, c);
                    snprintf(val_buf, sizeof(val_buf), "%s", txt ? (const char *)txt : "");
                    break;
                }
                default:
                    snprintf(val_buf, sizeof(val_buf), "NULL");
                    break;
            }
            g_string_append_printf(resp_out, "%-20s", val_buf);
        }
        g_string_append(resp_out, "\r\n");
        row_count++;
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (row_count == 0)
    {
        g_string_append(resp_out, "  (no rows)\r\n");
    }
    else
    {
        g_string_append_printf(resp_out, "  %u row(s)\r\n", row_count);
    }

    return cli_chunk_stream_start(&g_db_local->show_stream, db_local_ipc_ctx(), DEV_MODULE_ID_DB, msg, resp_out);
}

// ============================================================================
// 统一 Show 命令 Handler
// ============================================================================

static int handle_db_show_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int action = 0; /* 1=table-list  2=table-field  3=table-data */
    char *table_name = NULL;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* "table-list" */
                action = 1;
                break;
            case 2: /* "table-field" */
                action = 2;
                break;
            case 3: /* "table-data" */
                action = 3;
                break;
            case 4: /* <table-name> 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_free(table_name);
                    table_name = g_strdup(text);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    int ret;
    switch (action)
    {
        case 1:
            ret = handle_db_show_table_list(msg);
            break;
        case 2:
            if (table_name)
            {
                ret = handle_db_show_table_field(msg, table_name);
            }
            else
            {
                db_send_cli_response(msg, "Error: Missing table name.\r\n");
                ret = ERRCODE_FAIL;
            }
            break;
        case 3:
            if (table_name)
            {
                ret = handle_db_show_table_data(msg, table_name);
            }
            else
            {
                db_send_cli_response(msg, "Error: Missing table name.\r\n");
                ret = ERRCODE_FAIL;
            }
            break;
        default:
            db_send_cli_response(msg, "Error: Unknown command.\r\n");
            ret = ERRCODE_FAIL;
            break;
    }

    g_free(table_name);
    return ret;
}

// ============================================================================
// save / startup / show configuration
// ============================================================================

/* config 组动作（与 commands.xml cfg-id 对齐） */
#define DB_CFG_ACT_SAVE 1             /**< save configuration [<name>] */
#define DB_CFG_ACT_STARTUP 2          /**< startup configuration <name> {db|cfg} */
#define DB_CFG_ACT_SHOW 3             /**< show startup configuration */
#define DB_CFG_PARAM_NAME 4           /**< <name> 参数 */
#define DB_CFG_STARTUP_DB 5           /**< startup db mode */
#define DB_CFG_STARTUP_CFG 6          /**< startup cfg mode */
#define DB_CFG_SHOW_REPLAY_FAILURES 7 /**< show configuration replay-failures */

typedef struct db_config_save_job
{
    uint32_t dst_module_id;
    uint32_t request_id;
    char *name;
} db_config_save_job_t;

static pthread_mutex_t g_db_config_save_mutex = PTHREAD_MUTEX_INITIALIZER;

static void db_cli_data_dir(char *buf, size_t size)
{
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        snprintf(buf, size, "%s/data", work_dir);
    }
    else
    {
        snprintf(buf, size, "./data");
    }
}

static void db_cli_replay_failures_path(char *path, size_t path_size)
{
    char data_dir[512];
    db_cli_data_dir(data_dir, sizeof(data_dir));
    snprintf(path, path_size, "%s/%s", data_dir, DB_CLI_REPLAY_FAILURES_FILE);
}

static gboolean db_cli_config_name_valid(const char *name)
{
    if (!name || name[0] == '\0')
    {
        return TRUE;
    }
    if (strlen(name) > DB_CONFIG_NAME_MAX)
    {
        return FALSE;
    }
    for (const char *p = name; *p; p++)
    {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static gint db_cmp_uint32_asc(gconstpointer a, gconstpointer b)
{
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    if (va < vb)
    {
        return -1;
    }
    if (va > vb)
    {
        return 1;
    }
    return 0;
}

static GArray *db_collect_connected_modules_for_bdr(void)
{
    GArray *modules = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    dev_ipc_context_t *ctx = db_local_ipc_ctx();
    if (!ctx)
    {
        return modules;
    }

    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_t *conn = ctx->connections[i];
        /* DEV DOWN 后旧 socket 会短暂保留以排空在途响应，但不能再承载
         * SHOW_CONFIG query；不要把这种 draining transport 当成可采集模块。 */
        if (!conn || conn->state != DEV_IPC_COCONNECTED || conn->draining)
        {
            continue;
        }

        uint32_t mod_id = conn->remote_module_id;
        if (mod_id == DEV_MODULE_ID_DB || mod_id == DEV_MODULE_ID_CLI)
        {
            continue;
        }

        gboolean exists = FALSE;
        for (guint j = 0; j < modules->len; j++)
        {
            if (g_array_index(modules, uint32_t, j) == mod_id)
            {
                exists = TRUE;
                break;
            }
        }
        if (!exists)
        {
            g_array_append_val(modules, mod_id);
        }
    }
    pthread_mutex_unlock(&ctx->comutex);

    g_array_sort(modules, db_cmp_uint32_asc);
    return modules;
}

static gboolean db_module_array_contains(const GArray *modules, uint32_t module_id)
{
    for (guint i = 0; modules && i < modules->len; i++)
    {
        if (g_array_index((GArray *)modules, uint32_t, i) == module_id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/** 在本地 running DB 中安全检查配置标识表是否存在且非空。 */
static int db_capture_table_has_rows(const char *table_name, gboolean *has_rows)
{
    if (!table_name || !has_rows || !g_db_local || !g_db_local->main_conn || !g_db_local->main_conn->handle)
    {
        return ERRCODE_FAIL;
    }

    *has_rows = FALSE;
    db_connection_t *conn = g_db_local->main_conn;
    sqlite3_stmt *stmt = NULL;
    int ret = ERRCODE_FAIL;

    g_mutex_lock(&conn->db_mutex);
    int rc = sqlite3_prepare_v2(conn->handle, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;", -1,
                                &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        goto out;
    }
    sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
    int step_rc = sqlite3_step(stmt);
    gboolean table_exists = FALSE;
    if (step_rc == SQLITE_ROW)
    {
        table_exists = TRUE;
    }
    else if (step_rc != SQLITE_DONE)
    {
        goto out;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (!table_exists)
    {
        ret = ERRCODE_SUCCESS;
        goto out;
    }

    char *sql = sqlite3_mprintf("SELECT 1 FROM \"%w\" LIMIT 1;", table_name);
    if (!sql)
    {
        goto out;
    }
    rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK)
    {
        goto out;
    }
    step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW)
    {
        *has_rows = TRUE;
    }
    else if (step_rc == SQLITE_DONE)
    {
        *has_rows = FALSE;
    }
    else
    {
        goto out;
    }
    ret = ERRCODE_SUCCESS;

out:
    if (stmt)
    {
        sqlite3_finalize(stmt);
    }
    g_mutex_unlock(&conn->db_mutex);
    return ret;
}

/** 按逗号分隔的 revive-table 清单检查；任一表非空即视为存在配置。 */
static int db_capture_tables_have_rows(const char *table_names, gboolean *has_rows)
{
    if (!table_names || !has_rows)
    {
        return ERRCODE_FAIL;
    }

    *has_rows = FALSE;
    gboolean saw_table = FALSE;
    int ret = ERRCODE_SUCCESS;
    gchar **tables = g_strsplit(table_names, ",", -1);
    for (guint i = 0; tables && tables[i]; ++i)
    {
        char *table = g_strstrip(tables[i]);
        if (table[0] == '\0')
        {
            continue;
        }
        saw_table = TRUE;

        gboolean table_has_rows = FALSE;
        if (db_capture_table_has_rows(table, &table_has_rows) != ERRCODE_SUCCESS)
        {
            ret = ERRCODE_FAIL;
            break;
        }
        if (table_has_rows)
        {
            *has_rows = TRUE;
            break;
        }
    }
    g_strfreev(tables);
    return saw_table ? ret : ERRCODE_FAIL;
}

static int db_validate_bdr_module_coverage(const GArray *modules, GHashTable *inactive_optional_modules, char **err)
{
    for (guint i = 0; i < CONFIG_CAPTURE_OWNER_COUNT; i++)
    {
        const config_capture_owner_t *owner = &CONFIG_CAPTURE_OWNERS[i];
        gboolean required = owner->always_required;
        if (!required && owner->revive_table)
        {
            if (db_capture_tables_have_rows(owner->revive_table, &required) != ERRCODE_SUCCESS)
            {
                if (err)
                {
                    *err = g_strdup_printf("Configuration capture could not inspect %s table '%s'", owner->module_name,
                                           owner->revive_table);
                }
                return ERRCODE_FAIL;
            }
        }

        if (!required && !owner->always_required && inactive_optional_modules)
        {
            /* revive_table 是“是否存在可回放配置”的权威 marker。最后一项
             * 配置已删除后，退出模块的被动 socket 可能尚未收到 EOF；此时
             * 不应因为物理连接仍在就继续请求 BDR。 */
            g_hash_table_add(inactive_optional_modules, GUINT_TO_POINTER(owner->module_id));
        }
        if (required && !db_module_array_contains(modules, owner->module_id))
        {
            if (err)
            {
                *err = g_strdup_printf("Configuration capture incomplete: required module %s (0x%08X) is not connected",
                                       owner->module_name, owner->module_id);
            }
            return ERRCODE_FAIL;
        }
    }
    return ERRCODE_SUCCESS;
}

static dev_ipc_message_t *db_create_show_config_request(uint32_t mod_id)
{
    return dev_ipc_message_create(CLI_MSG_TYPE_SHOW_CONFIG, DEV_MODULE_ID_DB, mod_id, 0, NULL, 0, NULL);
}

static int db_collect_module_bdr(uint32_t mod_id, GString *output)
{
    if (output)
    {
        g_string_truncate(output, 0);
    }

    dev_ipc_message_t *req = db_create_show_config_request(mod_id);
    if (!req)
    {
        return ERRCODE_FAIL;
    }

    const uint32_t max_chunks = 4096;
    uint32_t chunks = 0;
    int ret = ERRCODE_FAIL;
    while (req && chunks < max_chunks)
    {
        uint32_t timeout_ms = (chunks == 0) ? 1000 : 5000;
        dev_ipc_message_t *resp = dev_ipc_query(db_local_ipc_ctx(), mod_id, req, timeout_ms);
        dev_ipc_message_free(req);
        req = NULL;
        chunks++;

        if (!resp)
        {
            LOG_WARN("DB-CONFIG: module 0x%08X BDR query timed out or disconnected", mod_id);
            break;
        }

        if (resp->msg_type == CLI_MSG_TYPE_RESP || resp->msg_type == CLI_MSG_TYPE_RESP_MORE)
        {
            if (output && resp->payload && resp->payload_len > 1)
            {
                g_string_append(output, (const char *)resp->payload);
            }

            if (resp->msg_type == CLI_MSG_TYPE_RESP_MORE)
            {
                req = dev_ipc_message_create(CLI_MSG_TYPE_CONTINUE, DEV_MODULE_ID_DB, mod_id, 0, NULL, 0, NULL);
                if (!req)
                {
                    LOG_WARN("DB-CONFIG: create CONTINUE failed for module 0x%08X", mod_id);
                }
            }
            else
            {
                ret = ERRCODE_SUCCESS;
            }
        }
        else if (resp->msg_type == CLI_MSG_TYPE_RESP_ERROR)
        {
            const char *detail =
                (resp->payload && resp->payload_len > 1) ? (const char *)resp->payload : "unspecified module error";
            LOG_WARN("DB-CONFIG: module 0x%08X BDR failed: %s", mod_id, detail);
        }
        else
        {
            LOG_WARN("DB-CONFIG: module 0x%08X returned unexpected BDR msg_type=0x%08X", mod_id, resp->msg_type);
        }

        dev_ipc_message_free(resp);
    }

    if (req)
    {
        dev_ipc_message_free(req);
    }

    if (chunks >= max_chunks)
    {
        LOG_WARN("DB-CONFIG: module 0x%08X exceeded BDR chunk limit", mod_id);
        ret = ERRCODE_FAIL;
    }
    return ret;
}

static int db_collect_bdr_config(char **cfg_text, char **err)
{
    if (cfg_text)
    {
        *cfg_text = NULL;
    }
    if (err)
    {
        *err = NULL;
    }
    if (!cfg_text)
    {
        return ERRCODE_FAIL;
    }

    /* save configuration 本身来自 CLI worker；若 DB worker 同步反向 RPC 到 CLI，
     * CLI worker 正在等 DB 响应，会形成等待环。这里由后台 job 直接按 SHOW_CONFIG
     * 协议汇聚各模块 BDR，DB worker 保持空闲以服务模块 BDR 期间的 DB 查询。 */
    cli_cfg_anchor_aggregator_t *agg = cli_cfg_anchor_agg_new();
    GString *module_out = g_string_new("");
    GArray *modules = db_collect_connected_modules_for_bdr();
    GHashTable *inactive_optional_modules = g_hash_table_new(g_direct_hash, g_direct_equal);

    if (db_validate_bdr_module_coverage(modules, inactive_optional_modules, err) != ERRCODE_SUCCESS)
    {
        g_hash_table_destroy(inactive_optional_modules);
        g_array_free(modules, TRUE);
        g_string_free(module_out, TRUE);
        cli_cfg_anchor_agg_free(agg);
        return ERRCODE_FAIL;
    }

    for (guint i = 0; i < modules->len; i++)
    {
        uint32_t mod_id = g_array_index(modules, uint32_t, i);
        if (g_hash_table_contains(inactive_optional_modules, GUINT_TO_POINTER(mod_id)))
        {
            continue;
        }
        if (db_collect_module_bdr(mod_id, module_out) != ERRCODE_SUCCESS)
        {
            if (err)
            {
                *err = g_strdup_printf("Configuration capture failed for module 0x%08X", mod_id);
            }
            g_hash_table_destroy(inactive_optional_modules);
            g_array_free(modules, TRUE);
            g_string_free(module_out, TRUE);
            cli_cfg_anchor_agg_free(agg);
            return ERRCODE_FAIL;
        }
        if (module_out->len > 0)
        {
            cli_cfg_anchor_agg_feed(agg, module_out->str);
        }
    }

    GString *full = g_string_new("");
    cli_cfg_anchor_agg_render(agg, full);

    g_hash_table_destroy(inactive_optional_modules);
    g_array_free(modules, TRUE);
    g_string_free(module_out, TRUE);
    cli_cfg_anchor_agg_free(agg);

    *cfg_text = g_string_free(full, FALSE);
    return ERRCODE_SUCCESS;
}

static void *db_config_save_job_main(void *arg)
{
    db_config_save_job_t *job = (db_config_save_job_t *)arg;
    if (!job)
    {
        return NULL;
    }

    char *err = NULL;
    char *cfg_text = NULL;
    int ret = ERRCODE_FAIL;
    char saved[DB_CONFIG_NAME_MAX + 1] = "";

    pthread_mutex_lock(&g_db_config_save_mutex);
    ret = db_collect_bdr_config(&cfg_text, &err);
    if (ret == ERRCODE_SUCCESS)
    {
        ret = db_config_save(job->name, cfg_text, saved, sizeof(saved), &err);
    }
    pthread_mutex_unlock(&g_db_config_save_mutex);

    char outbuf[256];
    if (ret == ERRCODE_SUCCESS)
    {
        snprintf(outbuf, sizeof(outbuf), "Configuration saved as '%s'.\r\n", saved);
    }
    else
    {
        snprintf(outbuf, sizeof(outbuf), "Error: %s.\r\n", err ? err : "save failed");
    }
    db_send_cli_response_to(job->dst_module_id, job->request_id, outbuf);

    g_free(cfg_text);
    g_free(err);
    g_free(job->name);
    g_free(job);
    return NULL;
}

static int db_config_save_async(dev_ipc_message_t *msg, const char *name, char **err)
{
    if (err)
    {
        *err = NULL;
    }

    db_config_save_job_t *job = g_malloc0(sizeof(*job));
    job->dst_module_id = msg->src_module_id;
    job->request_id = msg->request_id;
    job->name = g_strdup(name ? name : "");

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, db_config_save_job_main, job);
    if (rc != 0)
    {
        g_free(job->name);
        g_free(job);
        db_cli_set_err(err, "Failed to start save job");
        return ERRCODE_FAIL;
    }
    pthread_detach(tid);
    return ERRCODE_SUCCESS;
}

static int db_cli_show_replay_failures(dev_ipc_message_t *msg)
{
    char path[700];
    db_cli_replay_failures_path(path, sizeof(path));

    gchar *content = NULL;
    gsize len = 0;
    GString *out = g_string_new("Configuration replay failures:\r\n");
    if (g_file_get_contents(path, &content, &len, NULL) && content && len > 0)
    {
        gchar **lines = g_strsplit(content, "\n", -1);
        for (guint i = 0; lines && lines[i]; i++)
        {
            if (lines[i][0] == '\0')
            {
                continue;
            }
            g_string_append_printf(out, "  %s\r\n", lines[i]);
        }
        g_strfreev(lines);
    }
    else
    {
        g_string_append(out, "  <none>\r\n");
    }

    g_free(content);
    return cli_chunk_stream_start(&g_db_local->show_stream, db_local_ipc_ctx(), DEV_MODULE_ID_DB, msg, out);
}

static int handle_db_config_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean has_save = FALSE;
    gboolean has_startup = FALSE;
    gboolean has_show = FALSE;
    gboolean has_replay_failures = FALSE;
    db_config_startup_mode_t startup_mode = DB_CONFIG_STARTUP_MODE_NONE;
    char *name = NULL;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case DB_CFG_ACT_SAVE:
                has_save = TRUE;
                break;
            case DB_CFG_ACT_STARTUP:
                has_startup = TRUE;
                break;
            case DB_CFG_ACT_SHOW:
                has_show = TRUE;
                break;
            case DB_CFG_STARTUP_DB:
                startup_mode = DB_CONFIG_STARTUP_MODE_DB;
                break;
            case DB_CFG_STARTUP_CFG:
                startup_mode = DB_CONFIG_STARTUP_MODE_CFG;
                break;
            case DB_CFG_SHOW_REPLAY_FAILURES:
                has_replay_failures = TRUE;
                break;
            case DB_CFG_PARAM_NAME:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_free(name);
                    name = g_strdup(text);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    int ret = ERRCODE_FAIL;
    char *err = NULL;
    char outbuf[256];

    if (has_show && has_replay_failures)
    {
        ret = db_cli_show_replay_failures(msg);
    }
    else if (has_show)
    {
        /* show startup configuration */
        char cur[DB_CONFIG_NAME_MAX + 1];
        db_config_startup_mode_t mode = DB_CONFIG_STARTUP_MODE_NONE;
        db_config_get_startup(cur, sizeof(cur), &mode);
        if (cur[0] != '\0')
        {
            snprintf(outbuf, sizeof(outbuf), "Startup configuration: %s (%s)\r\n", cur,
                     db_config_startup_mode_name(mode));
        }
        else
        {
            snprintf(outbuf, sizeof(outbuf), "Startup configuration: <none> (factory default)\r\n");
        }
        db_send_cli_response(msg, outbuf);
        ret = ERRCODE_SUCCESS;
    }
    else if (has_save)
    {
        /* save configuration [<name>] */
        if (!db_cli_config_name_valid(name))
        {
            db_send_cli_response(msg, "Error: Invalid configuration name (allowed: A-Z a-z 0-9 _ - , max 63).\r\n");
            ret = ERRCODE_FAIL;
        }
        else
        {
            ret = db_config_save_async(msg, name, &err);
        }
        if (ret != ERRCODE_SUCCESS)
        {
            if (err)
            {
                snprintf(outbuf, sizeof(outbuf), "Error: %s.\r\n", err);
                db_send_cli_response(msg, outbuf);
            }
        }
    }
    else if (has_startup)
    {
        /* startup configuration <name> {db|cfg} */
        if (!name || name[0] == '\0')
        {
            db_send_cli_response(msg, "Error: Missing configuration name.\r\n");
            ret = ERRCODE_FAIL;
        }
        else if (startup_mode == DB_CONFIG_STARTUP_MODE_NONE)
        {
            db_send_cli_response(msg, "Error: Missing startup mode (db|cfg).\r\n");
            ret = ERRCODE_FAIL;
        }
        else
        {
            ret = db_config_set_startup(name, startup_mode, &err);
            if (ret == ERRCODE_SUCCESS)
            {
                snprintf(outbuf, sizeof(outbuf), "Startup configuration set to '%s' (%s).\r\n", name,
                         db_config_startup_mode_name(startup_mode));
                db_send_cli_response(msg, outbuf);
            }
            else
            {
                snprintf(outbuf, sizeof(outbuf), "Error: %s.\r\n", err ? err : "startup failed");
                db_send_cli_response(msg, outbuf);
            }
        }
    }
    else
    {
        db_send_cli_response(msg, "Error: Unknown configuration command.\r\n");
        ret = ERRCODE_FAIL;
    }

    g_free(err);
    g_free(name);
    return ret;
}

// ============================================================================
// 动态候选值查询：返回所有用户表名
// ============================================================================

void db_cli_handle_query_candidates(dev_ipc_message_t *msg)
{
    db_connection_t *conn = g_db_local->main_conn;

    GByteArray *buf = g_byte_array_new();

    if (conn && conn->handle)
    {
        const char *sql =
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;";
        sqlite3_stmt *stmt;

        g_mutex_lock(&conn->db_mutex);
        int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK)
        {
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char *name = (const char *)sqlite3_column_text(stmt, 0);
                if (name && name[0] != '\0')
                {
                    g_byte_array_append(buf, (const guint8 *)name, (guint)(strlen(name) + 1));
                }
            }
            sqlite3_finalize(stmt);
        }
        g_mutex_unlock(&conn->db_mutex);
    }

    /* 追加结束空字节 */
    guint8 terminator = '\0';
    g_byte_array_append(buf, &terminator, 1);

    guint payload_len = buf->len;
    guint8 *payload = g_memdup2(buf->data, payload_len);
    g_byte_array_free(buf, TRUE);

    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_QUERY_CANDIDATES_RESP, DEV_MODULE_ID_DB,
                                                     msg->src_module_id, msg->request_id, payload, payload_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(db_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// 主入口
// ============================================================================

int db_cli_handle_show_config(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_start(&g_db_local->show_stream, db_local_ipc_ctx(), DEV_MODULE_ID_DB, msg, NULL);
}

int db_cli_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_db_local->show_stream, db_local_ipc_ctx(), DEV_MODULE_ID_DB, msg);
}

void db_cli_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_db_local->show_stream);
}

int db_cli_process_command(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_chunk_stream_reset(&g_db_local->show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("Payload parsing failed");
        db_send_cli_response(msg, "DB Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("Received TLV payload (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case DB_CLI_GROUP_ID_SHOW:
            result = handle_db_show_cmd(msg, &parser);
            break;
        case DB_CLI_GROUP_ID_CONFIG:
            result = handle_db_config_cmd(msg, &parser);
            break;
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            db_send_cli_response(msg, "DB Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
