/**
 * @file   db_cli.c
 * @brief  数据库模块 CLI 命令处理（直接查询 SQLite，无需 registry）
 * @author jhb
 * @date   2026/01/22
 */
#include "db_cli.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "db_main.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void db_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_DB, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(db_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
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
                           "Tables in netnexus.db:\r\n"
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
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            db_send_cli_response(msg, "DB Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
