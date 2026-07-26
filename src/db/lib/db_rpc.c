/**
 * @file   db_rpc.c
 * @brief  数据库 RPC 接口实现，供其他模块通过 IPC 调用 DB 模块
 * @author jhb
 * @date   2026/02/11
 *
 * 统一 SQL 传输模型：
 *   - 客户端（本文件）负责将参数组装为完整 SQL 字符串并打印
 *   - 服务端（db_ipc_handler.c）直接执行收到的 SQL 字符串
 *
 * 全局只有一个 SQLite 数据库，db_name 在此固定，调用方只需传入表名。
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "db_serialize.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

/** 全局唯一数据库名（与 db_schema.c 中路径对应） */
#define DB_GLOBAL_NAME "netnexus"

// ============================================================================
// SQL 构建辅助函数
// ============================================================================

/**
 * @brief 将 db_value_t 格式化为 SQL 字面量，写入 buf，返回写入字节数
 *
 * - INTEGER → 十进制整数
 * - REAL    → %.17g 浮点数
 * - TEXT    → 单引号包围，内部单引号转义为 ''
 * - BLOB    → X'HEXSTRING' 格式
 * - NULL    → NULL
 */
static int value_to_sql_literal(const db_value_t *val, char *buf, size_t buf_size)
{
    if (!val || buf_size == 0)
    {
        return 0;
    }

    switch (val->type)
    {
        case DB_TYPE_NULL:
            return snprintf(buf, buf_size, "NULL");

        case DB_TYPE_INTEGER:
            return snprintf(buf, buf_size, "%" PRId64, val->data.i64);

        case DB_TYPE_REAL:
            return snprintf(buf, buf_size, "%.17g", val->data.real);

        case DB_TYPE_TEXT:
        {
            int offset = snprintf(buf, buf_size, "'");
            const char *s = val->data.text ? val->data.text : "";
            while (*s && (size_t)offset < buf_size - 2)
            {
                if (*s == '\'')
                {
                    if ((size_t)offset + 2 >= buf_size)
                    {
                        break;
                    }
                    buf[offset++] = '\'';
                    buf[offset++] = '\'';
                    buf[offset] = '\0';
                }
                else
                {
                    buf[offset++] = *s;
                    buf[offset] = '\0';
                }
                s++;
            }
            offset += snprintf(buf + offset, buf_size - offset, "'");
            return offset;
        }

        case DB_TYPE_BLOB:
        {
            int offset = snprintf(buf, buf_size, "X'");
            const uint8_t *data = (const uint8_t *)val->data.blob.data;
            for (size_t i = 0; i < val->data.blob.len; i++)
            {
                if ((size_t)offset + 3 >= buf_size)
                {
                    break;
                }
                offset += snprintf(buf + offset, buf_size - offset, "%02X", data[i]);
            }
            offset += snprintf(buf + offset, buf_size - offset, "'");
            return offset;
        }
    }
    return 0;
}

/**
 * @brief 将比较操作符映射为 SQL 操作符
 */
static const char *compare_op_to_sql(db_compare_op_t op)
{
    switch (op)
    {
        case DB_CMP_EQ:
            return "=";
        case DB_CMP_NE:
            return "!=";
        case DB_CMP_GT:
            return ">";
        case DB_CMP_GTE:
            return ">=";
        case DB_CMP_LT:
            return "<";
        case DB_CMP_LTE:
            return "<=";
        case DB_CMP_LIKE:
            return "LIKE";
        default:
            return "=";
    }
}

/**
 * @brief 根据结构化过滤条件构建 WHERE 子句内容（不含 WHERE 关键字）
 */
static int build_where_clause(const db_filter_t *filter, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0)
    {
        return -1;
    }

    buf[0] = '\0';
    if (!filter || !filter->conditions || filter->num_conditions == 0)
    {
        return 0;
    }

    int offset = 0;
    for (uint32_t i = 0; i < filter->num_conditions; i++)
    {
        const db_condition_t *cond = &filter->conditions[i];
        if (!cond->field_name || cond->field_name[0] == '\0')
        {
            return -1;
        }

        if (i > 0)
        {
            offset += snprintf(buf + offset, buf_size - offset, " AND ");
        }

        offset += snprintf(buf + offset, buf_size - offset, "%s %s ", cond->field_name, compare_op_to_sql(cond->op));
        offset += value_to_sql_literal(&cond->value, buf + offset, buf_size - offset);
    }

    return offset;
}

/**
 * @brief 构建 INSERT SQL：INSERT INTO table (col1, col2) VALUES (val1, val2);
 */
static int build_insert_sql(const char *table_name, const char **field_names, const db_value_t *values,
                            uint32_t num_fields, char *buf, size_t buf_size)
{
    int offset = snprintf(buf, buf_size, "INSERT INTO %s (", table_name);

    for (uint32_t i = 0; i < num_fields; i++)
    {
        if (i > 0)
        {
            offset += snprintf(buf + offset, buf_size - offset, ", ");
        }
        offset += snprintf(buf + offset, buf_size - offset, "%s", field_names[i]);
    }

    offset += snprintf(buf + offset, buf_size - offset, ") VALUES (");

    for (uint32_t i = 0; i < num_fields; i++)
    {
        if (i > 0)
        {
            offset += snprintf(buf + offset, buf_size - offset, ", ");
        }
        offset += value_to_sql_literal(&values[i], buf + offset, buf_size - offset);
    }

    offset += snprintf(buf + offset, buf_size - offset, ");");
    return offset;
}

/**
 * @brief 构建 UPDATE SQL：UPDATE table SET col1=val1, col2=val2 [WHERE ...];
 */
static int build_update_sql(const char *table_name, const char **field_names, const db_value_t *values,
                            uint32_t num_fields, const db_filter_t *filter, char *buf, size_t buf_size)
{
    int offset = snprintf(buf, buf_size, "UPDATE %s SET ", table_name);

    for (uint32_t i = 0; i < num_fields; i++)
    {
        if (i > 0)
        {
            offset += snprintf(buf + offset, buf_size - offset, ", ");
        }
        offset += snprintf(buf + offset, buf_size - offset, "%s = ", field_names[i]);
        offset += value_to_sql_literal(&values[i], buf + offset, buf_size - offset);
    }

    char where_clause[2048];
    int where_len = build_where_clause(filter, where_clause, sizeof(where_clause));
    if (where_len < 0)
    {
        return -1;
    }
    if (where_len > 0)
    {
        offset += snprintf(buf + offset, buf_size - offset, " WHERE %s", where_clause);
    }

    offset += snprintf(buf + offset, buf_size - offset, ";");
    return offset;
}

/**
 * @brief 构建 DELETE SQL：DELETE FROM table [WHERE ...];
 */
static int build_delete_sql(const char *table_name, const db_filter_t *filter, char *buf, size_t buf_size)
{
    int offset = snprintf(buf, buf_size, "DELETE FROM %s", table_name);

    char where_clause[2048];
    int where_len = build_where_clause(filter, where_clause, sizeof(where_clause));
    if (where_len < 0)
    {
        return -1;
    }
    if (where_len > 0)
    {
        offset += snprintf(buf + offset, buf_size - offset, " WHERE %s", where_clause);
    }

    offset += snprintf(buf + offset, buf_size - offset, ";");
    return offset;
}

/**
 * @brief 将 db_value_type_t 映射为 SQLite 类型字符串
 */
static const char *rpc_col_type_to_sql(db_value_type_t type)
{
    switch (type)
    {
        case DB_TYPE_INTEGER:
            return "INTEGER";
        case DB_TYPE_REAL:
            return "REAL";
        case DB_TYPE_TEXT:
            return "TEXT";
        case DB_TYPE_BLOB:
            return "BLOB";
        default:
            return "TEXT";
    }
}

/**
 * @brief 根据 db_table_def_t 生成 CREATE TABLE IF NOT EXISTS SQL
 */
static int rpc_build_create_table_sql(const db_table_def_t *def, char *buf, size_t buf_size)
{
    int offset = snprintf(buf, buf_size, "CREATE TABLE IF NOT EXISTS %s (\n", def->table_name);

    for (uint32_t i = 0; i < def->num_cols; i++)
    {
        const db_column_def_t *col = &def->cols[i];
        offset += snprintf(buf + offset, buf_size - offset, "  %s %s", col->name, rpc_col_type_to_sql(col->type));

        if (col->constraints & DB_COL_PRIMARY_KEY)
        {
            offset += snprintf(buf + offset, buf_size - offset, " PRIMARY KEY");
        }
        if (col->constraints & DB_COL_AUTOINCREMENT)
        {
            offset += snprintf(buf + offset, buf_size - offset, " AUTOINCREMENT");
        }
        if (col->constraints & DB_COL_NOT_NULL)
        {
            offset += snprintf(buf + offset, buf_size - offset, " NOT NULL");
        }
        if (col->constraints & DB_COL_UNIQUE)
        {
            offset += snprintf(buf + offset, buf_size - offset, " UNIQUE");
        }
        if (col->default_val)
        {
            if (col->type == DB_TYPE_TEXT)
            {
                offset += snprintf(buf + offset, buf_size - offset, " DEFAULT '%s'", col->default_val);
            }
            else
            {
                offset += snprintf(buf + offset, buf_size - offset, " DEFAULT %s", col->default_val);
            }
        }

        offset += snprintf(buf + offset, buf_size - offset, (i < def->num_cols - 1) ? ",\n" : "\n");
    }

    offset += snprintf(buf + offset, buf_size - offset, ");");
    return offset;
}

static gboolean rpc_is_valid_identifier(const char *identifier)
{
    if (!identifier || identifier[0] == '\0' || (!g_ascii_isalpha(identifier[0]) && identifier[0] != '_'))
    {
        return FALSE;
    }

    for (const char *cursor = identifier + 1; *cursor != '\0'; ++cursor)
    {
        if (!g_ascii_isalnum(*cursor) && *cursor != '_')
        {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * @brief 检查 PRAGMA table_info 结果中是否包含指定列名
 */
static gboolean rpc_schema_has_column(const db_result_t *schema, const char *column_name)
{
    if (!schema || !column_name)
    {
        return FALSE;
    }

    for (uint32_t i = 0; i < schema->num_rows; i++)
    {
        const db_row_t *row = schema->rows[i];
        if (!row)
        {
            continue;
        }

        for (uint32_t j = 0; j < row->num_fields; j++)
        {
            if (strcmp(row->field_names[j], "name") != 0)
            {
                continue;
            }

            if (row->values[j].type == DB_TYPE_TEXT && row->values[j].data.text &&
                strcmp(row->values[j].data.text, column_name) == 0)
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/**
 * @brief 为新增列构建 ALTER TABLE ... ADD COLUMN SQL
 */
static int rpc_build_add_column_sql(const char *table_name, const db_column_def_t *col, char *buf, size_t buf_size)
{
    if (!table_name || !col || !col->name || !buf || buf_size == 0)
    {
        return -1;
    }

    if ((col->constraints & DB_COL_PRIMARY_KEY) || (col->constraints & DB_COL_AUTOINCREMENT) ||
        (col->constraints & DB_COL_UNIQUE))
    {
        LOG_ERROR("New column %s.%s contains constraints not supported by SQLite ADD COLUMN (PRIMARY "
                  "KEY/UNIQUE/AUTOINCREMENT)",
                  table_name, col->name);
        return -1;
    }

    if ((col->constraints & DB_COL_NOT_NULL) && (!col->default_val || col->default_val[0] == '\0'))
    {
        LOG_ERROR("New NOT NULL column must have a default value: %s.%s", table_name, col->name);
        return -1;
    }

    int offset = snprintf(buf, buf_size, "ALTER TABLE %s ADD COLUMN %s %s", table_name, col->name,
                          rpc_col_type_to_sql(col->type));

    if (col->constraints & DB_COL_NOT_NULL)
    {
        offset += snprintf(buf + offset, buf_size - offset, " NOT NULL");
    }

    if (col->default_val && col->default_val[0] != '\0')
    {
        offset += snprintf(buf + offset, buf_size - offset, " DEFAULT %s", col->default_val);
    }

    offset += snprintf(buf + offset, buf_size - offset, ";");
    return offset;
}

/**
 * @brief 构建 SELECT SQL：SELECT col1, col2 FROM table [WHERE ...];
 */
static int build_select_sql(const char *table_name, const char **field_names, uint32_t num_fields,
                            const db_filter_t *filter, char *buf, size_t buf_size)
{
    int offset = snprintf(buf, buf_size, "SELECT ");

    if (num_fields == 0 || !field_names)
    {
        offset += snprintf(buf + offset, buf_size - offset, "*");
    }
    else
    {
        for (uint32_t i = 0; i < num_fields; i++)
        {
            if (i > 0)
            {
                offset += snprintf(buf + offset, buf_size - offset, ", ");
            }
            offset += snprintf(buf + offset, buf_size - offset, "%s", field_names[i]);
        }
    }

    offset += snprintf(buf + offset, buf_size - offset, " FROM %s", table_name);

    char where_clause[2048];
    int where_len = build_where_clause(filter, where_clause, sizeof(where_clause));
    if (where_len < 0)
    {
        return -1;
    }
    if (where_len > 0)
    {
        offset += snprintf(buf + offset, buf_size - offset, " WHERE %s", where_clause);
    }

    offset += snprintf(buf + offset, buf_size - offset, ";");
    return offset;
}

// ============================================================================
// 通用 RPC 发送辅助
// ============================================================================

/**
 * @brief 发送 EXEC_SQL 请求（DML/DDL），返回影响行数或错误码
 *
 * 入口先做一次 O(1) 的 IPC 连接状态检查：DB 离线时直接返回 -1，避免阻塞
 * 5 秒 IPC 超时（DB crash / 重启过程中这一点很关键）。
 */
static int send_exec_sql(dev_ipc_context_t *ctx, const char *sql)
{
    if (!dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        LOG_ERROR("RPC exec_sql rejected: DB module not connected");
        return -1;
    }

    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_sql(DB_GLOBAL_NAME, sql, &payload, &payload_len);

    dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DB_EXEC_SQL, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_DB, 0, payload, payload_len, g_free);

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    dev_ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC exec_sql failed: no response");
        return -1;
    }

    int32_t retval = -1;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
    dev_ipc_message_free(resp);
    return retval;
}

/**
 * @brief 发送 QUERY_SQL 请求（SELECT），返回结果集
 *
 * 入口连接预检逻辑与 send_exec_sql 一致：DB 离线时快速失败。
 */
static int send_query_sql(dev_ipc_context_t *ctx, const char *sql, db_result_t **result)
{
    if (!dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        LOG_ERROR("RPC query_sql rejected: DB module not connected");
        return ERRCODE_FAIL;
    }

    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_sql(DB_GLOBAL_NAME, sql, &payload, &payload_len);

    dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DB_QUERY_SQL, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_DB, 0, payload, payload_len, g_free);

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    dev_ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC query_sql failed: no response");
        return ERRCODE_FAIL;
    }

    int32_t retval = ERRCODE_FAIL;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, result);
    dev_ipc_message_free(resp);
    return retval;
}

// ============================================================================
// DB 可用性检查 / 配置 Guard（业务模块在 CLI config 入口调用）
// ============================================================================

int db_rpc_is_available(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return 0;
    }
    return dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB);
}

int db_rpc_guard_reject(dev_ipc_context_t *ctx, dev_ipc_message_t *cli_msg, const char *module_tag)
{
    if (!ctx || !cli_msg)
    {
        return 0;
    }
    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        return 0;
    }

    const char *tag = module_tag ? module_tag : "DB";
    char text[160];
    int n =
        snprintf(text, sizeof(text), "%s Error: configuration rejected because DB module is not available\r\n", tag);
    if (n < 0)
    {
        n = 0;
    }
    size_t text_len = (size_t)n + 1;

    char *payload = g_strdup(text);
    dev_ipc_message_t *resp =
        dev_ipc_message_create(CLI_MSG_TYPE_RESP, dev_ipc_get_module_id(ctx), cli_msg->src_module_id,
                               cli_msg->request_id, payload, text_len, g_free);
    if (resp)
    {
        if (dev_ipc_send_response(ctx, resp) != 0)
        {
            LOG_WARN("db_rpc_guard_reject: failed to send error response to module 0x%08X", cli_msg->src_module_id);
        }
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(payload);
    }

    LOG_WARN("%s: config command rejected (DB module not available)", tag);
    return 1;
}

/**
 * @brief 根据表定义自动补齐缺失列（仅新增列，不改动已有列）
 */
static int rpc_sync_table_columns(dev_ipc_context_t *ctx, const db_table_def_t *def)
{
    char pragma_sql[512];
    snprintf(pragma_sql, sizeof(pragma_sql), "PRAGMA table_info(%s);", def->table_name);
    LOG_INFO("[SQL] %s", pragma_sql);

    db_result_t *schema = NULL;
    int ret = send_query_sql(ctx, pragma_sql, &schema);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Failed to read table structure: %s", def->table_name);
        return ERRCODE_FAIL;
    }

    for (uint32_t i = 0; i < def->num_cols; i++)
    {
        const db_column_def_t *col = &def->cols[i];
        if (rpc_schema_has_column(schema, col->name))
        {
            continue;
        }

        char alter_sql[1024];
        if (rpc_build_add_column_sql(def->table_name, col, alter_sql, sizeof(alter_sql)) < 0)
        {
            db_result_free(schema);
            return ERRCODE_FAIL;
        }

        LOG_INFO("[SQL] %s", alter_sql);
        int rows = send_exec_sql(ctx, alter_sql);
        if (rows < 0)
        {
            LOG_ERROR("Failed to add missing column: %s.%s", def->table_name, col->name);
            db_result_free(schema);
            return ERRCODE_FAIL;
        }

        LOG_INFO("Auto-added missing column: %s.%s", def->table_name, col->name);
    }

    db_result_free(schema);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 公共 RPC 接口实现
// ============================================================================

int db_rpc_delete(dev_ipc_context_t *ctx, const char *table_name, const db_filter_t *filter)
{
    if (!ctx || !table_name)
    {
        return -1;
    }

    char sql[2048];
    if (build_delete_sql(table_name, filter, sql, sizeof(sql)) < 0)
    {
        LOG_ERROR("Failed to build DELETE SQL: invalid filter condition");
        return -1;
    }
    LOG_INFO("[SQL] %s", sql);

    return send_exec_sql(ctx, sql);
}

int db_rpc_query(dev_ipc_context_t *ctx, const char *table_name, const char **field_names, uint32_t num_fields,
                 const db_filter_t *filter, db_result_t **result)
{
    if (!ctx || !table_name || !result)
    {
        return ERRCODE_FAIL;
    }

    char sql[4096];
    if (build_select_sql(table_name, field_names, num_fields, filter, sql, sizeof(sql)) < 0)
    {
        LOG_ERROR("Failed to build SELECT SQL: invalid filter condition");
        return ERRCODE_FAIL;
    }
    LOG_INFO("[SQL] %s", sql);

    return send_query_sql(ctx, sql, result);
}

int db_rpc_exists(dev_ipc_context_t *ctx, const char *table_name, const db_filter_t *filter, gboolean *exists)
{
    if (!ctx || !table_name || !exists)
    {
        return ERRCODE_FAIL;
    }

    /* 用 SELECT 1 检查是否存在，避免传输完整行数据 */
    const char *fields[] = {"1"};
    char sql[2048];
    if (build_select_sql(table_name, fields, 1, filter, sql, sizeof(sql)) < 0)
    {
        LOG_ERROR("Failed to build EXISTS query SQL: invalid filter condition");
        return ERRCODE_FAIL;
    }
    LOG_INFO("[SQL] %s", sql);

    db_result_t *result = NULL;
    int ret = send_query_sql(ctx, sql, &result);

    if (ret == ERRCODE_SUCCESS)
    {
        *exists = (result != NULL && result->num_rows > 0);
        db_result_free(result);
        return ERRCODE_SUCCESS;
    }

    return ERRCODE_FAIL;
}

int db_rpc_create_table_from_def(dev_ipc_context_t *ctx, const db_table_def_t *def)
{
    if (!ctx || !def || !def->table_name || !def->cols || def->num_cols == 0)
    {
        return ERRCODE_FAIL;
    }

    char sql[4096];
    rpc_build_create_table_sql(def, sql, sizeof(sql));
    LOG_INFO("[SQL] %s", sql);

    int rows = send_exec_sql(ctx, sql);
    if (rows < 0)
    {
        return ERRCODE_FAIL;
    }

    return rpc_sync_table_columns(ctx, def);
}

int db_rpc_create_delete_cascade(dev_ipc_context_t *ctx, const char *trigger_name, const char *parent_table,
                                 const char *parent_column, const char *child_table, const char *child_column)
{
    if (!ctx || !rpc_is_valid_identifier(trigger_name) || !rpc_is_valid_identifier(parent_table) ||
        !rpc_is_valid_identifier(parent_column) || !rpc_is_valid_identifier(child_table) ||
        !rpc_is_valid_identifier(child_column))
    {
        LOG_ERROR("Failed to create delete cascade: invalid SQL identifier");
        return ERRCODE_FAIL;
    }

    char sql[1024];
    int length = snprintf(sql, sizeof(sql),
                          "CREATE TRIGGER IF NOT EXISTS %s AFTER DELETE ON %s FOR EACH ROW BEGIN "
                          "DELETE FROM %s WHERE %s = OLD.%s; END;",
                          trigger_name, parent_table, child_table, child_column, parent_column);
    if (length < 0 || (size_t)length >= sizeof(sql))
    {
        LOG_ERROR("Failed to create delete cascade: SQL buffer is too small");
        return ERRCODE_FAIL;
    }

    LOG_INFO("[SQL] %s", sql);
    return send_exec_sql(ctx, sql) >= 0 ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

// ============================================================================
// db_record_t — 写操作键值构建器实现
// ============================================================================

/** 单个字段-值对 */
typedef struct
{
    char *field;      /**< 字段名（已分配） */
    db_value_t value; /**< 值 */
} db_record_entry_t;

/** db_record_t 内部结构 */
struct db_record
{
    GArray *entries; /**< db_record_entry_t 数组 */
};

db_record_t *db_record_new(void)
{
    db_record_t *rec = g_new0(db_record_t, 1);
    rec->entries = g_array_new(FALSE, FALSE, sizeof(db_record_entry_t));
    return rec;
}

void db_record_free(db_record_t *rec)
{
    if (!rec)
    {
        return;
    }

    for (guint i = 0; i < rec->entries->len; i++)
    {
        db_record_entry_t *e = &g_array_index(rec->entries, db_record_entry_t, i);
        g_free(e->field);
        db_value_free(&e->value);
    }
    g_array_free(rec->entries, TRUE);
    g_free(rec);
}

/**
 * @brief 在 record 中查找已有字段，返回其索引；不存在返回 -1
 */
static int record_find_field(db_record_t *rec, const char *field)
{
    for (guint i = 0; i < rec->entries->len; i++)
    {
        db_record_entry_t *e = &g_array_index(rec->entries, db_record_entry_t, i);
        if (strcmp(e->field, field) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief 设置或覆盖字段值（已存在则替换）
 */
static void record_set(db_record_t *rec, const char *field, db_value_t value)
{
    if (!rec || !field)
    {
        db_value_free(&value);
        return;
    }

    int idx = record_find_field(rec, field);
    if (idx >= 0)
    {
        /* 覆盖已有字段 */
        db_record_entry_t *e = &g_array_index(rec->entries, db_record_entry_t, idx);
        db_value_free(&e->value);
        e->value = value;
    }
    else
    {
        db_record_entry_t entry;
        entry.field = g_strdup(field);
        entry.value = value;
        g_array_append_val(rec->entries, entry);
    }
}

void db_record_set_int(db_record_t *rec, const char *field, int64_t value)
{
    record_set(rec, field, db_value_int(value));
}

void db_record_set_text(db_record_t *rec, const char *field, const char *value)
{
    record_set(rec, field, db_value_text(value));
}

void db_record_set_real(db_record_t *rec, const char *field, double value)
{
    record_set(rec, field, db_value_real(value));
}

// ============================================================================
// db_row_t 读取辅助实现
// ============================================================================

int64_t db_row_get_int(const db_row_t *row, const char *field, int64_t default_val)
{
    if (!row || !field)
    {
        return default_val;
    }

    for (uint32_t i = 0; i < row->num_fields; i++)
    {
        if (strcmp(row->field_names[i], field) == 0)
        {
            if (row->values[i].type == DB_TYPE_INTEGER)
            {
                return row->values[i].data.i64;
            }
            return default_val;
        }
    }
    return default_val;
}

const char *db_row_get_text(const db_row_t *row, const char *field, const char *default_val)
{
    if (!row || !field)
    {
        return default_val;
    }

    for (uint32_t i = 0; i < row->num_fields; i++)
    {
        if (strcmp(row->field_names[i], field) == 0)
        {
            if (row->values[i].type == DB_TYPE_TEXT)
            {
                return row->values[i].data.text;
            }
            return default_val;
        }
    }
    return default_val;
}

double db_row_get_real(const db_row_t *row, const char *field, double default_val)
{
    if (!row || !field)
    {
        return default_val;
    }

    for (uint32_t i = 0; i < row->num_fields; i++)
    {
        if (strcmp(row->field_names[i], field) == 0)
        {
            if (row->values[i].type == DB_TYPE_REAL)
            {
                return row->values[i].data.real;
            }
            return default_val;
        }
    }
    return default_val;
}

// ============================================================================
// 基于 db_record_t 的新 RPC 函数实现
// ============================================================================

int db_rpc_insert_record(dev_ipc_context_t *ctx, const char *table, const db_record_t *rec)
{
    if (!ctx || !table || !rec || rec->entries->len == 0)
    {
        return ERRCODE_FAIL;
    }

    uint32_t n = rec->entries->len;
    const char **field_names = g_new(const char *, n);
    db_value_t *values = g_new(db_value_t, n);

    for (uint32_t i = 0; i < n; i++)
    {
        db_record_entry_t *e = &g_array_index(rec->entries, db_record_entry_t, i);
        field_names[i] = e->field;
        values[i] = e->value;
    }

    char sql[4096];
    build_insert_sql(table, field_names, values, n, sql, sizeof(sql));
    g_free(field_names);
    g_free(values);
    LOG_INFO("[SQL] %s", sql);

    int rows = send_exec_sql(ctx, sql);
    return (rows >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int db_rpc_update_record(dev_ipc_context_t *ctx, const char *table, const db_record_t *rec, const db_filter_t *filter)
{
    if (!ctx || !table || !rec || rec->entries->len == 0)
    {
        return -1;
    }

    uint32_t n = rec->entries->len;
    const char **field_names = g_new(const char *, n);
    db_value_t *values = g_new(db_value_t, n);

    for (uint32_t i = 0; i < n; i++)
    {
        db_record_entry_t *e = &g_array_index(rec->entries, db_record_entry_t, i);
        field_names[i] = e->field;
        values[i] = e->value;
    }

    char sql[4096];
    int sql_len = build_update_sql(table, field_names, values, n, filter, sql, sizeof(sql));
    g_free(field_names);
    g_free(values);
    if (sql_len < 0)
    {
        LOG_ERROR("Failed to build UPDATE SQL: invalid filter condition");
        return -1;
    }
    LOG_INFO("[SQL] %s", sql);

    return send_exec_sql(ctx, sql);
}

// ============================================================================
// 通用便捷 API：(列名,值) 数组直传，免去 db_record_t 模板代码
// ============================================================================

void db_filter_init(db_filter_builder_t *b)
{
    if (!b)
    {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->filter.conditions = b->conds;
    b->filter.num_conditions = 0;
}

static void db_filter_add(db_filter_builder_t *b, const char *name, db_value_t v)
{
    if (!b || !name)
    {
        db_value_free(&v);
        return;
    }
    if (b->n >= G_N_ELEMENTS(b->conds))
    {
        LOG_ERROR("db_filter_builder overflow: cap=%zu, dropping field=%s", G_N_ELEMENTS(b->conds), name);
        db_value_free(&v);
        return;
    }
    b->conds[b->n].field_name = name;
    b->conds[b->n].op = DB_CMP_EQ;
    b->conds[b->n].value = v;
    b->n++;
    b->filter.num_conditions = b->n;
}

void db_filter_add_int(db_filter_builder_t *b, const char *name, int64_t v)
{
    db_filter_add(b, name, db_value_int(v));
}

void db_filter_add_text(db_filter_builder_t *b, const char *name, const char *v)
{
    db_filter_add(b, name, db_value_text(v));
}

void db_filter_clear(db_filter_builder_t *b)
{
    if (!b)
    {
        return;
    }
    for (uint32_t i = 0; i < b->n; i++)
    {
        db_value_free(&b->conds[i].value);
    }
    b->n = 0;
    b->filter.num_conditions = 0;
}

/** 把 (cols,n) 折叠成临时 db_record_t */
static db_record_t *cols_to_record(const db_col_t *cols, size_t n_cols)
{
    db_record_t *rec = db_record_new();
    if (!rec)
    {
        return NULL;
    }
    for (size_t i = 0; i < n_cols; i++)
    {
        const db_col_t *c = &cols[i];
        if (!c->name)
        {
            continue;
        }
        switch (c->value.type)
        {
            case DB_TYPE_INTEGER:
                db_record_set_int(rec, c->name, c->value.data.i64);
                break;
            case DB_TYPE_TEXT:
                db_record_set_text(rec, c->name, c->value.data.text);
                break;
            case DB_TYPE_REAL:
                db_record_set_real(rec, c->name, c->value.data.real);
                break;
            case DB_TYPE_NULL:
            case DB_TYPE_BLOB:
            default:
                LOG_WARN("db_rpc_*_cols: unsupported value type=%d for field=%s, skipped", (int)c->value.type, c->name);
                break;
        }
    }
    return rec;
}

int db_rpc_update_cols(dev_ipc_context_t *ctx, const char *table, const db_filter_t *where, const db_col_t *cols,
                       size_t n_cols)
{
    if (!ctx || !table || !where || !cols || n_cols == 0)
    {
        return -1;
    }
    db_record_t *rec = cols_to_record(cols, n_cols);
    if (!rec)
    {
        return -1;
    }
    int rows = db_rpc_update_record(ctx, table, rec, where);
    db_record_free(rec);
    return rows;
}

int db_rpc_insert_cols(dev_ipc_context_t *ctx, const char *table, const db_col_t *cols, size_t n_cols)
{
    if (!ctx || !table || !cols || n_cols == 0)
    {
        return ERRCODE_FAIL;
    }
    db_record_t *rec = cols_to_record(cols, n_cols);
    if (!rec)
    {
        return ERRCODE_FAIL;
    }
    int rc = db_rpc_insert_record(ctx, table, rec);
    db_record_free(rec);
    return rc;
}
