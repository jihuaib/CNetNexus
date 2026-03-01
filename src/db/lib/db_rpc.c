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
#include "db_rpc.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_serialize.h"
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
                            uint32_t num_fields, const char *where_clause, char *buf, size_t buf_size)
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

    if (where_clause && where_clause[0] != '\0')
    {
        offset += snprintf(buf + offset, buf_size - offset, " WHERE %s", where_clause);
    }

    offset += snprintf(buf + offset, buf_size - offset, ";");
    return offset;
}

/**
 * @brief 构建 DELETE SQL：DELETE FROM table [WHERE ...];
 */
static int build_delete_sql(const char *table_name, const char *where_clause, char *buf, size_t buf_size)
{
    int offset = snprintf(buf, buf_size, "DELETE FROM %s", table_name);

    if (where_clause && where_clause[0] != '\0')
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
            offset += snprintf(buf + offset, buf_size - offset, " DEFAULT %s", col->default_val);
        }

        offset += snprintf(buf + offset, buf_size - offset, (i < def->num_cols - 1) ? ",\n" : "\n");
    }

    offset += snprintf(buf + offset, buf_size - offset, ");");
    return offset;
}

/**
 * @brief 构建 SELECT SQL：SELECT col1, col2 FROM table [WHERE ...];
 */
static int build_select_sql(const char *table_name, const char **field_names, uint32_t num_fields,
                            const char *where_clause, char *buf, size_t buf_size)
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

    if (where_clause && where_clause[0] != '\0')
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
 */
static int send_exec_sql(ipc_context_t *ctx, const char *sql)
{
    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_sql(DB_GLOBAL_NAME, sql, &payload, &payload_len);

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_EXEC_SQL, ipc_get_module_id(ctx), DEV_MODULE_ID_DB, 0,
                                            payload, payload_len, g_free);

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC exec_sql 失败: 无响应");
        return -1;
    }

    int32_t retval = -1;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
    ipc_message_free(resp);
    return retval;
}

/**
 * @brief 发送 QUERY_SQL 请求（SELECT），返回结果集
 */
static int send_query_sql(ipc_context_t *ctx, const char *sql, db_result_t **result)
{
    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_sql(DB_GLOBAL_NAME, sql, &payload, &payload_len);

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_QUERY_SQL, ipc_get_module_id(ctx), DEV_MODULE_ID_DB, 0,
                                            payload, payload_len, g_free);

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC query_sql 失败: 无响应");
        return ERRCODE_FAIL;
    }

    int32_t retval = ERRCODE_FAIL;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, result);
    ipc_message_free(resp);
    return retval;
}

// ============================================================================
// 公共 RPC 接口实现
// ============================================================================

int db_rpc_insert(ipc_context_t *ctx, const char *table_name, const char **field_names, const db_value_t *values,
                  uint32_t num_fields)
{
    if (!ctx || !table_name || !field_names || !values || num_fields == 0)
    {
        return ERRCODE_FAIL;
    }

    char sql[4096];
    build_insert_sql(table_name, field_names, values, num_fields, sql, sizeof(sql));
    LOG_INFO("[SQL] %s", sql);

    int rows = send_exec_sql(ctx, sql);
    return (rows >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int db_rpc_update(ipc_context_t *ctx, const char *table_name, const char **field_names, const db_value_t *values,
                  uint32_t num_fields, const char *where_clause)
{
    if (!ctx || !table_name || !field_names || !values || num_fields == 0)
    {
        return -1;
    }

    char sql[4096];
    build_update_sql(table_name, field_names, values, num_fields, where_clause, sql, sizeof(sql));
    LOG_INFO("[SQL] %s", sql);

    return send_exec_sql(ctx, sql);
}

int db_rpc_delete(ipc_context_t *ctx, const char *table_name, const char *where_clause)
{
    if (!ctx || !table_name)
    {
        return -1;
    }

    char sql[2048];
    build_delete_sql(table_name, where_clause, sql, sizeof(sql));
    LOG_INFO("[SQL] %s", sql);

    return send_exec_sql(ctx, sql);
}

int db_rpc_query(ipc_context_t *ctx, const char *table_name, const char **field_names, uint32_t num_fields,
                 const char *where_clause, db_result_t **result)
{
    if (!ctx || !table_name || !result)
    {
        return ERRCODE_FAIL;
    }

    char sql[4096];
    build_select_sql(table_name, field_names, num_fields, where_clause, sql, sizeof(sql));
    LOG_INFO("[SQL] %s", sql);

    return send_query_sql(ctx, sql, result);
}

int db_rpc_exists(ipc_context_t *ctx, const char *table_name, const char *where_clause, gboolean *exists)
{
    if (!ctx || !table_name || !exists)
    {
        return ERRCODE_FAIL;
    }

    /* 用 SELECT 1 检查是否存在，避免传输完整行数据 */
    const char *fields[] = {"1"};
    char sql[2048];
    build_select_sql(table_name, fields, 1, where_clause, sql, sizeof(sql));
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

int db_rpc_create_table(ipc_context_t *ctx, const char *ddl)
{
    if (!ctx || !ddl || ddl[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    LOG_INFO("[SQL] %s", ddl);

    int rows = send_exec_sql(ctx, ddl);
    return (rows >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int db_rpc_create_table_from_def(ipc_context_t *ctx, const db_table_def_t *def)
{
    if (!ctx || !def || !def->table_name || !def->cols || def->num_cols == 0)
    {
        return ERRCODE_FAIL;
    }

    char sql[4096];
    rpc_build_create_table_sql(def, sql, sizeof(sql));
    LOG_INFO("[SQL] %s", sql);

    int rows = send_exec_sql(ctx, sql);
    return (rows >= 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}
