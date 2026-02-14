/**
 * @file   db_cli.c
 * @brief  数据库模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "db_cli.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "db_api.h"
#include "db_main.h"
#include "db_registry.h"
#include "dev.h"
#include "errcode.h"

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
// Show 命令处理函数
// ============================================================================

static int handle_db_show_list(ipc_message_t *msg)
{
    db_registry_t *registry = db_registry_get_instance();
    if (!registry || !registry->databases)
    {
        db_send_cli_response(msg, "No databases registered.\r\n");
        return ERRCODE_SUCCESS;
    }

    db_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));
    int offset = 0;

    g_mutex_lock(&registry->registry_mutex);
    uint32_t total = g_hash_table_size(registry->databases);

    /* 输出标题 */
    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset,
                       "Registered Databases:\r\n"
                       "%-16s %-10s %s\r\n"
                       "---------------- ---------- ------\r\n",
                       "Name", "Module", "Tables");

    if (total > 0)
    {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, registry->databases);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            db_definition_t *db_def = (db_definition_t *)value;
            char module_name[64];
            if (dev_get_module_name(db_def->module_id, module_name) != ERRCODE_SUCCESS)
            {
                snprintf(module_name, sizeof(module_name), "0x%08X", db_def->module_id);
            }
            offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "%-16s %-10s %u\r\n",
                               db_def->db_name, module_name, db_def->num_tables);
        }
    }
    g_mutex_unlock(&registry->registry_mutex);

    db_send_cli_response(msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

static int handle_db_show_tables(ipc_message_t *msg, const char *db_name)
{
    db_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));
    int offset = 0;

    db_definition_t *db_def = db_registry_find(db_name);
    if (db_def)
    {
        offset +=
            snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "Database: %s\r\n", db_def->db_name);
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "Tables:\r\n");
        for (uint32_t i = 0; i < db_def->num_tables; i++)
        {
            offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  - %s (%u fields)\r\n",
                               db_def->tables[i]->table_name, db_def->tables[i]->num_fields);
        }
    }
    else
    {
        snprintf(resp_out.message, sizeof(resp_out.message), "Error: Database '%s' not found\r\n", db_name);
    }

    db_send_cli_response(msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

static int handle_db_show_fields(ipc_message_t *msg, const char *db_name, const char *table_name)
{
    db_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));
    int offset = 0;

    db_table_t *table = db_registry_find_table(db_name, table_name);
    if (table)
    {
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "Database: %s, Table: %s\r\n",
                           db_name, table->table_name);
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "Fields:\r\n");
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  %-20s | %-20s | %-10s\r\n",
                           "Field Name", "Type", "SQL Type");
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset,
                           "  ------------------------------------------------------------\r\n");
        for (uint32_t j = 0; j < table->num_fields; j++)
        {
            db_field_t *field = table->fields[j];
            offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset,
                               "  %-20s | %-20s | %-10s\r\n", field->field_name, field->type_str, field->sql_type);
        }
    }
    else
    {
        snprintf(resp_out.message, sizeof(resp_out.message), "Error: Table '%s' not found in database '%s'\r\n",
                 table_name, db_name);
    }

    db_send_cli_response(msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

static int handle_db_show_data(ipc_message_t *msg, const char *db_name, const char *table_name)
{
    db_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));
    int offset = 0;

    db_table_t *table_def = db_registry_find_table(db_name, table_name);
    if (!table_def)
    {
        snprintf(resp_out.message, sizeof(resp_out.message), "Error: Table '%s' not found in database '%s'\r\n",
                 table_name, db_name);
        db_send_cli_response(msg, resp_out.message);
        return ERRCODE_SUCCESS;
    }

    /* 查询所有行 */
    db_result_t *result = NULL;
    int ret = db_query(db_name, table_name, NULL, 0, NULL, &result);
    if (ret != ERRCODE_SUCCESS || !result)
    {
        snprintf(resp_out.message, sizeof(resp_out.message), "Error: Failed to query table '%s'\r\n", table_name);
        db_send_cli_response(msg, resp_out.message);
        return ERRCODE_SUCCESS;
    }

    /* 计算每列最大宽度 */
    uint32_t num_cols = table_def->num_fields;
    int *col_widths = g_malloc0(sizeof(int) * num_cols);

    for (uint32_t c = 0; c < num_cols; c++)
    {
        col_widths[c] = (int)strlen(table_def->fields[c]->field_name);
    }

    for (uint32_t r = 0; r < result->num_rows; r++)
    {
        db_row_t *row = result->rows[r];
        for (uint32_t c = 0; c < row->num_fields && c < num_cols; c++)
        {
            int val_len = 0;
            switch (row->values[c].type)
            {
                case DB_TYPE_INTEGER:
                {
                    char tmp[32];
                    val_len = snprintf(tmp, sizeof(tmp), "%ld", (long)row->values[c].data.i64);
                    break;
                }
                case DB_TYPE_REAL:
                {
                    char tmp[32];
                    val_len = snprintf(tmp, sizeof(tmp), "%.6g", row->values[c].data.real);
                    break;
                }
                case DB_TYPE_TEXT:
                    val_len = row->values[c].data.text ? (int)strlen(row->values[c].data.text) : 4;
                    break;
                default:
                    val_len = 4; /* "NULL" */
                    break;
            }
            if (val_len > col_widths[c])
            {
                col_widths[c] = val_len;
            }
        }
    }

    /* 输出标题 */
    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset,
                       "Database: %s, Table: %s (%u rows)\r\n", db_name, table_name, result->num_rows);

    /* 输出列头 */
    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  ");
    for (uint32_t c = 0; c < num_cols; c++)
    {
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "%-*s", col_widths[c] + 2,
                           table_def->fields[c]->field_name);
    }
    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "\r\n  ");

    /* 输出分隔线 */
    for (uint32_t c = 0; c < num_cols; c++)
    {
        for (int k = 0; k < col_widths[c] + 2; k++)
        {
            if ((size_t)(offset + 1) < sizeof(resp_out.message))
            {
                resp_out.message[offset++] = '-';
            }
        }
    }
    offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "\r\n");

    /* 输出数据行 */
    for (uint32_t r = 0; r < result->num_rows; r++)
    {
        db_row_t *row = result->rows[r];
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  ");
        for (uint32_t c = 0; c < row->num_fields && c < num_cols; c++)
        {
            char val_buf[256];
            switch (row->values[c].type)
            {
                case DB_TYPE_INTEGER:
                    snprintf(val_buf, sizeof(val_buf), "%ld", (long)row->values[c].data.i64);
                    break;
                case DB_TYPE_REAL:
                    snprintf(val_buf, sizeof(val_buf), "%.6g", row->values[c].data.real);
                    break;
                case DB_TYPE_TEXT:
                    snprintf(val_buf, sizeof(val_buf), "%s",
                             row->values[c].data.text ? row->values[c].data.text : "NULL");
                    break;
                default:
                    snprintf(val_buf, sizeof(val_buf), "NULL");
                    break;
            }
            offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "%-*s", col_widths[c] + 2,
                               val_buf);
        }
        offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "\r\n");

        if ((size_t)offset >= sizeof(resp_out.message) - 128)
        {
            offset += snprintf(resp_out.message + offset, sizeof(resp_out.message) - offset, "  ... (truncated)\r\n");
            break;
        }
    }

    g_free(col_widths);
    db_result_free(result);

    db_send_cli_response(msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 统一 Show 命令 Handler（按 cfg_id 解析 TLV 条目）
// ============================================================================

static int handle_db_show_cmd(ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int action = 0; /* 由关键字 cfg_id 决定动作 */
    char *db_name = NULL;
    char *table_name = NULL;

    /* 遍历 TLV 条目，按 cfg_id 解析 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        /* 跳过上下文条目 */
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* "list" 关键字 */
                action = 1;
                break;
            case 2: /* db_name 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_free(db_name);
                    db_name = g_strdup(text);
                }
                break;
            }
            case 3: /* "table-list" 关键字 */
                action = 3;
                break;
            case 4: /* "table-field" 关键字 */
                action = 4;
                break;
            case 5: /* table_name 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_free(table_name);
                    table_name = g_strdup(text);
                }
                break;
            }
            case 6: /* "table-data" 关键字 */
                action = 6;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    int ret = ERRCODE_FAIL;

    switch (action)
    {
        case 1: /* show db list */
            ret = handle_db_show_list(msg);
            break;
        case 3: /* show db <name> table-list */
            if (db_name)
            {
                ret = handle_db_show_tables(msg, db_name);
            }
            else
            {
                db_send_cli_response(msg, "Error: Missing database name.\r\n");
            }
            break;
        case 4: /* show db <name> table-field <table> */
            if (db_name && table_name)
            {
                ret = handle_db_show_fields(msg, db_name, table_name);
            }
            else
            {
                db_send_cli_response(msg, "Error: Missing database or table name.\r\n");
            }
            break;
        case 6: /* show db <name> table-data <table> */
            if (db_name && table_name)
            {
                ret = handle_db_show_data(msg, db_name, table_name);
            }
            else
            {
                db_send_cli_response(msg, "Error: Missing database or table name.\r\n");
            }
            break;
        default:
            db_send_cli_response(msg, "Error: Unknown or missing action in command payload.\r\n");
            break;
    }

    g_free(db_name);
    g_free(table_name);
    return ret;
}

// ============================================================================
// 主入口
// ============================================================================

int db_cli_handle_continue(ipc_message_t *msg)
{
    /* 无待发送的批量数据 - 发送空的最终响应 */
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
        printf("[db_cli] 载荷解析失败\n");
        db_send_cli_response(msg, "DB Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    printf("[db_cli] 收到 TLV 载荷 (group_id=%u)\n", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case 1: /* DB show 命令组 */
            result = handle_db_show_cmd(msg, &parser);
            break;
        default:
            printf("[db_cli] 未知 group_id: %u\n", parser.group_id);
            db_send_cli_response(msg, "DB Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
