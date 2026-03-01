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

static void db_send_cli_response(ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_DB, msg->src_module_id,
                                             msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        ipc_send_response(g_db_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }
}

// ============================================================================
// show db table-list
// ============================================================================

static int handle_db_show_table_list(ipc_message_t *msg)
{
    db_connection_t *conn = g_db_local->main_conn;
    if (!conn || !conn->handle)
    {
        db_send_cli_response(msg, "Error: Database not open.\r\n");
        return ERRCODE_FAIL;
    }

    db_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));
    int offset = 0;

    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset,
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
        return ERRCODE_FAIL;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  %s\r\n", name ? name : "");
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    db_send_cli_response(msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// show db table-field <table-name>
// ============================================================================

static int handle_db_show_table_field(ipc_message_t *msg, const char *table_name)
{
    db_connection_t *conn = g_db_local->main_conn;
    if (!conn || !conn->handle)
    {
        db_send_cli_response(msg, "Error: Database not open.\r\n");
        return ERRCODE_FAIL;
    }

    db_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));
    int offset = 0;

    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset,
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
        snprintf(resp_out.message, sizeof(resp_out.message), "Error: Table '%s' not found.\r\n", table_name);
        db_send_cli_response(msg, resp_out.message);
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
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset,
                           "  %-4d  %-24s  %-12s  %-8d  %d\r\n", cid, name ? name : "", type ? type : "", notnull, pk);
        row_count++;
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (row_count == 0)
    {
        snprintf(resp_out.message, sizeof(resp_out.message), "Error: Table '%s' not found.\r\n", table_name);
    }

    db_send_cli_response(msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// show db table-data <table-name>
// ============================================================================

static int handle_db_show_table_data(ipc_message_t *msg, const char *table_name)
{
    db_connection_t *conn = g_db_local->main_conn;
    if (!conn || !conn->handle)
    {
        db_send_cli_response(msg, "Error: Database not open.\r\n");
        return ERRCODE_FAIL;
    }

    db_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));
    int offset = 0;

    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT * FROM %s;", table_name);
    sqlite3_stmt *stmt;

    g_mutex_lock(&conn->db_mutex);
    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        g_mutex_unlock(&conn->db_mutex);
        snprintf(resp_out.message, sizeof(resp_out.message), "Error: Table '%s' not found.\r\n", table_name);
        db_send_cli_response(msg, resp_out.message);
        return ERRCODE_FAIL;
    }

    int col_count = sqlite3_column_count(stmt);

    /* 输出列头 */
    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "Table: %s\r\n  ", table_name);
    for (int c = 0; c < col_count; c++)
    {
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "%-20s",
                           sqlite3_column_name(stmt, c));
    }
    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "\r\n  ");
    for (int c = 0; c < col_count; c++)
    {
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "--------------------");
    }
    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "\r\n");

    /* 输出数据行 */
    uint32_t row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  ");
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
                    snprintf(val_buf, sizeof(val_buf), "%s", sqlite3_column_text(stmt, c));
                    break;
                default:
                    snprintf(val_buf, sizeof(val_buf), "NULL");
                    break;
            }
            offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "%-20s", val_buf);
        }
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "\r\n");
        row_count++;

        if ((size_t)offset >= sizeof(resp_out.message) - 128)
        {
            offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  ... (truncated)\r\n");
            break;
        }
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (row_count == 0)
    {
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  (no rows)\r\n");
    }
    else
    {
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  %u row(s)\r\n", row_count);
    }

    db_send_cli_response(msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 统一 Show 命令 Handler
// ============================================================================

static int handle_db_show_cmd(ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int action = 0; /* 1=table-list  2=table-field  3=table-data */
    char *table_name = NULL;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
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
// 主入口
// ============================================================================

int db_cli_handle_continue(ipc_message_t *msg)
{
    db_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

int db_cli_process_command(ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        db_send_cli_response(msg, "DB Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case 1:
            result = handle_db_show_cmd(msg, &parser);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            db_send_cli_response(msg, "DB Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
