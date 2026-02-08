/**
 * @file   db_cli.c
 * @brief  数据库模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "db_cli.h"

#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "db.h"
#include "db_main.h"
#include "db_registry.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

// ============================================================================
// Group Dispatch Table
// ============================================================================
typedef int (*db_group_handler_t)(cfg_tlv_parser_t parser, db_cli_out_t *cfg_out,
                                     db_cli_resp_out_t *resp_out);

typedef struct db_group_dispatch
{
    uint32_t group_id;
    db_group_handler_t handler;
} db_group_dispatch_t;

static int handle_show_db(cfg_tlv_parser_t parser, db_cli_out_t *cfg_out, db_cli_resp_out_t *resp_out);

// ============================================================================
// Show 输出写入临时 DB
// ============================================================================

#define DB_SHOW_DB "db_show_db"
#define DB_SHOW_META "db_show_meta"
#define DB_SHOW_ROW "db_show_row"

static int db_insert_show_meta(int has_rows, uint32_t total)
{
    const char *fields[] = {"has_rows", "total"};
    db_value_t values[] = {db_value_int(has_rows ? 1 : 0), db_value_int(total)};
    return db_insert(DB_SHOW_DB, DB_SHOW_META, fields, values, 2);
}

static int db_insert_show_row(const char *name, const char *module, uint32_t tables)
{
    const char *fields[] = {"name", "module", "tables"};
    db_value_t values[] = {db_value_text(name), db_value_text(module), db_value_int(tables)};
    int ret = db_insert(DB_SHOW_DB, DB_SHOW_ROW, fields, values, 3);
    db_value_free(&values[0]);
    db_value_free(&values[1]);
    return ret;
}

static const db_group_dispatch_t g_db_group_dispatch[] = {
    {DB_CLI_GROUP_ID_SHOW_DB, handle_show_db},
};

#define DB_GROUP_DISPATCH_COUNT (sizeof(g_db_group_dispatch) / sizeof(g_db_group_dispatch[0]))

typedef int (*db_cfg_resp_t)(ipc_message_t *msg, const db_cli_out_t *cfg_out,
                                const db_cli_resp_out_t *resp_out);

typedef struct db_cli_resp_dispatch
{
    uint32_t group_id;
    db_cfg_resp_t handler;
} db_cli_resp_dispatch_t;

static int handle_default_resp(ipc_message_t *msg, const db_cli_out_t *cfg_out,
                               const db_cli_resp_out_t *resp_out);

static const db_cli_resp_dispatch_t g_db_cfg_resp_dispatch[] = {
    {DB_CLI_GROUP_ID_SHOW_DB, handle_default_resp},
};

#define DB_CFG_RESP_DISPATCH_COUNT (sizeof(g_db_cfg_resp_dispatch) / sizeof(g_db_cfg_resp_dispatch[0]))

// ============================================================================
// Show DB Command Handler
// ============================================================================

/**
 * @brief Handle "show db" command
 * Displays all registered databases, or tables in a db, or structure of a table
 */
static int handle_show_db(cfg_tlv_parser_t parser, db_cli_out_t *cfg_out, db_cli_resp_out_t *resp_out)
{
    CFG_TLV_FOREACH(parser, cfg_id, value, len)
    {
        switch (cfg_id)
        {
            case DB_CLI_SHOW_DB_CFG_ID_LIST:
            {
                cfg_out->data.show_db.is_db_list = TRUE;
                break;
            }
            case DB_CLI_SHOW_DB_CFG_ID_DB_NAME:
            {
                CFG_TLV_GET_STRING(value, len, cfg_out->data.show_db.db_name, sizeof(cfg_out->data.show_db.db_name));
                break;
            }
            case DB_CLI_SHOW_DB_CFG_ID_TABLE_LIST:
            {
                cfg_out->data.show_db.is_table_list = TRUE;
                break;
            }
            case DB_CLI_SHOW_DB_CFG_ID_TABLE_FIELD:
            {
                cfg_out->data.show_db.is_table_field = TRUE;
                break;
            }
            case DB_CLI_SHOW_DB_CFG_ID_TABLE_DATA:
            {
                cfg_out->data.show_db.is_table_data = TRUE;
                break;
            }
            case DB_CLI_SHOW_DB_CFG_ID_TABLE_NAME:
            {
                CFG_TLV_GET_STRING(value, len, cfg_out->data.show_db.table_name,
                                      sizeof(cfg_out->data.show_db.table_name));
                break;
            }
        }
    }

    db_registry_t *registry = db_registry_get_instance();
    if (!registry || !registry->databases)
    {
        snprintf(resp_out->message, sizeof(resp_out->message), "No databases registered\r\n");
        resp_out->success = 1;
        return ERRCODE_SUCCESS;
    }

    int offset = 0;

    if (cfg_out->data.show_db.is_table_data)
    {
        // show db <db-name> table-data <table-name>
        db_table_t *table_def =
            db_registry_find_table(cfg_out->data.show_db.db_name, cfg_out->data.show_db.table_name);
        if (!table_def)
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Table '%s' not found in database '%s'\r\n",
                     cfg_out->data.show_db.table_name, cfg_out->data.show_db.db_name);
            resp_out->success = 1;
            return ERRCODE_SUCCESS;
        }

        // 查询所有行
        db_result_t *result = NULL;
        int ret = db_query(cfg_out->data.show_db.db_name, cfg_out->data.show_db.table_name, NULL, 0, NULL, &result);
        if (ret != ERRCODE_SUCCESS || !result)
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Failed to query table '%s'\r\n",
                     cfg_out->data.show_db.table_name);
            resp_out->success = 1;
            return ERRCODE_SUCCESS;
        }

        // 计算每列最大宽度
        uint32_t num_cols = table_def->num_fields;
        int *col_widths = g_malloc0(sizeof(int) * num_cols);

        // 初始化为列名宽度
        for (uint32_t c = 0; c < num_cols; c++)
        {
            col_widths[c] = (int)strlen(table_def->fields[c]->field_name);
        }

        // 遍历数据行计算最大宽度
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
                        val_len = 4; // "NULL"
                        break;
                }
                if (val_len > col_widths[c])
                {
                    col_widths[c] = val_len;
                }
            }
        }

        // 输出标题
        offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                           "Database: %s, Table: %s (%u rows)\r\n", cfg_out->data.show_db.db_name,
                           cfg_out->data.show_db.table_name, result->num_rows);

        // 输出列头
        offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  ");
        for (uint32_t c = 0; c < num_cols; c++)
        {
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "%-*s",
                               col_widths[c] + 2, table_def->fields[c]->field_name);
        }
        offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "\r\n  ");

        // 输出分隔线
        for (uint32_t c = 0; c < num_cols; c++)
        {
            for (int k = 0; k < col_widths[c] + 2; k++)
            {
                if ((size_t)(offset + 1) < sizeof(resp_out->message))
                {
                    resp_out->message[offset++] = '-';
                }
            }
        }
        offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "\r\n");

        // 输出数据行
        for (uint32_t r = 0; r < result->num_rows; r++)
        {
            db_row_t *row = result->rows[r];
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  ");
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
                        snprintf(val_buf, sizeof(val_buf), "%s", row->values[c].data.text ? row->values[c].data.text : "NULL");
                        break;
                    default:
                        snprintf(val_buf, sizeof(val_buf), "NULL");
                        break;
                }
                offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "%-*s",
                                   col_widths[c] + 2, val_buf);
            }
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "\r\n");

            // 防止缓冲区溢出
            if ((size_t)offset >= sizeof(resp_out->message) - 128)
            {
                offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                                   "  ... (truncated)\r\n");
                break;
            }
        }

        g_free(col_widths);
        db_result_free(result);
    }
    else if (cfg_out->data.show_db.is_table_field)
    {
        // show db <db-name> table-field <table-name>
        db_table_t *table =
            db_registry_find_table(cfg_out->data.show_db.db_name, cfg_out->data.show_db.table_name);
        if (table)
        {
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "Database: %s, Table: %s\r\n", cfg_out->data.show_db.db_name, table->table_name);
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Fields:\r\n");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "  %-20s | %-20s | %-10s\r\n", "Field Name", "Type", "SQL Type");
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                               "  ------------------------------------------------------------\r\n");
            for (uint32_t j = 0; j < table->num_fields; j++)
            {
                db_field_t *field = table->fields[j];
                offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset,
                                   "  %-20s | %-20s | %-10s\r\n", field->field_name, field->type_str, field->sql_type);
            }
        }
        else
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Table '%s' not found in database '%s'\r\n",
                     cfg_out->data.show_db.table_name, cfg_out->data.show_db.db_name);
        }
    }
    else if (cfg_out->data.show_db.is_table_list)
    {
        // show db <db-name> table
        db_definition_t *db_def = db_registry_find(cfg_out->data.show_db.db_name);
        if (db_def)
        {
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Database: %s\r\n",
                               db_def->db_name);
            offset += snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "Tables:\r\n");
            for (uint32_t i = 0; i < db_def->num_tables; i++)
            {
                offset +=
                    snprintf(resp_out->message + offset, sizeof(resp_out->message) - offset, "  - %s (%u fields)\r\n",
                             db_def->tables[i]->table_name, db_def->tables[i]->num_fields);
            }
        }
        else
        {
            snprintf(resp_out->message, sizeof(resp_out->message), "Error: Database '%s' not found\r\n",
                     cfg_out->data.show_db.db_name);
        }
    }
    else if (cfg_out->data.show_db.is_db_list)
    {
        // show db list -> write structured rows, let CLI render template
        uint32_t total = 0;
        g_mutex_lock(&registry->registry_mutex);
        total = g_hash_table_size(registry->databases);
        db_insert_show_meta(total > 0, total);

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
                db_insert_show_row(db_def->db_name, module_name, db_def->num_tables);
            }
        }
        g_mutex_unlock(&registry->registry_mutex);

        resp_out->message[0] = '\0';
    }

    resp_out->success = 1;
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Dispatch logic
// ============================================================================

static int dispatch_by_group_id(uint32_t group_id, cfg_tlv_parser_t parser, db_cli_out_t *cfg_out,
                                db_cli_resp_out_t *resp_out)
{
    for (size_t i = 0; i < DB_GROUP_DISPATCH_COUNT; i++)
    {
        if (g_db_group_dispatch[i].group_id == group_id)
        {
            printf("[db_cli] Dispatching to group (group_id=%u)\n", group_id);
            return g_db_group_dispatch[i].handler(parser, cfg_out, resp_out);
        }
    }

    printf("[db_cli] Error: Unknown group_id: %u\n", group_id);
    snprintf(resp_out->message, sizeof(resp_out->message), "DB Error: Unknown command group %u.\r\n", group_id);
    resp_out->success = 0;
    return ERRCODE_FAIL;
}

static int handle_default_resp(ipc_message_t *msg, const db_cli_out_t *cfg_out,
                               const db_cli_resp_out_t *resp_out)
{
    (void)cfg_out;

    char *resp_data = g_strdup(resp_out->message);
    ipc_message_t *resp_msg = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_DB, msg->src_module_id,
                                                       msg->request_id, resp_data, strlen(resp_data) + 1, g_free);

    if (resp_msg)
    {
        ipc_send_response(g_db_local->ipc_ctx, resp_msg);
        ipc_message_free(resp_msg);
    }

    return ERRCODE_SUCCESS;
}

static void db_cli_send_response(ipc_message_t *msg, const db_cli_out_t *cfg_out,
                                    const db_cli_resp_out_t *resp_out)
{
    if (msg->src_module_id == 0)
    {
        return; // No sender to respond to
    }

    for (size_t i = 0; i < DB_CFG_RESP_DISPATCH_COUNT; i++)
    {
        if (g_db_cfg_resp_dispatch[i].group_id == cfg_out->group_id)
        {
            printf("[db_cli] Dispatching resp to group (group_id=%u)\n", cfg_out->group_id);
            (void)g_db_cfg_resp_dispatch[i].handler(msg, cfg_out, resp_out);
        }
    }
}

int db_cli_handle_continue(ipc_message_t *msg)
{
    // No batch output pending - send empty final response
    char *resp_data = g_strdup("");
    ipc_message_t *resp_msg = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_DB, msg->src_module_id,
                                                       msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp_msg)
    {
        ipc_send_response(g_db_local->ipc_ctx, resp_msg);
        ipc_message_free(resp_msg);
    }
    return ERRCODE_SUCCESS;
}

int db_cli_process_command(ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    db_cli_out_t cfg_out;
    db_cli_resp_out_t resp_out;
    memset(&cfg_out, 0, sizeof(cfg_out));
    memset(&resp_out, 0, sizeof(resp_out));

    int result = ERRCODE_FAIL;

    CFG_TLV_PARSE_BEGIN(msg->payload, msg->payload_len, parser, group_id)
    {
        printf("[db_cli] Received CLI command (group_id=%u)\n", group_id);
        cfg_out.group_id = group_id;
        result = dispatch_by_group_id(group_id, parser, &cfg_out, &resp_out);
    }
    CFG_TLV_PARSE_END();

    // Send response back
    db_cli_send_response(msg, &cfg_out, &resp_out);

    return result;
}
