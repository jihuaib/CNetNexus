/**
 * @file   db_api.c
 * @brief  数据库 CRUD 操作 API 实现（本地 SQLite 操作）
 * @author jhb
 * @date   2026/01/22
 */
#include "db_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "db_main.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// Value Helper Functions
// ============================================================================

db_value_t db_value_int(int64_t value)
{
    db_value_t v;
    v.type = DB_TYPE_INTEGER;
    v.data.i64 = value;
    return v;
}

db_value_t db_value_text(const char *value)
{
    db_value_t v;
    v.type = DB_TYPE_TEXT;
    v.data.text = value ? g_strdup(value) : NULL;
    return v;
}

db_value_t db_value_real(double value)
{
    db_value_t v;
    v.type = DB_TYPE_REAL;
    v.data.real = value;
    return v;
}

db_value_t db_value_null(void)
{
    db_value_t v;
    v.type = DB_TYPE_NULL;
    return v;
}

void db_value_free(db_value_t *value)
{
    if (!value)
    {
        return;
    }

    if (value->type == DB_TYPE_TEXT && value->data.text)
    {
        g_free(value->data.text);
        value->data.text = NULL;
    }
    else if (value->type == DB_TYPE_BLOB && value->data.blob.data)
    {
        g_free(value->data.blob.data);
        value->data.blob.data = NULL;
    }
}

// ============================================================================
// Result Management
// ============================================================================

void db_result_free(db_result_t *result)
{
    if (!result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        if (row)
        {
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                g_free(row->field_names[j]);
                db_value_free(&row->values[j]);
            }
            g_free(row->field_names);
            g_free(row->values);
            g_free(row);
        }
    }

    g_free(result->rows);
    g_free(result);
}

// ============================================================================
// CRUD Operations
// ============================================================================

int db_insert(const char *db_name, const char *table_name, const char **field_names, const db_value_t *values,
              uint32_t num_fields)
{
    if (!db_name || !table_name || !field_names || !values || num_fields == 0)
    {
        return ERRCODE_FAIL;
    }

    db_connection_t *conn = db_get_connection(db_name);
    if (!conn)
    {
        LOG_ERROR("错误: 找不到数据库连接 (db=%s)", db_name);
        return ERRCODE_FAIL;
    }

    if (!conn->handle)
    {
        LOG_ERROR("错误: 数据库未连接 (db=%s)", db_name);
        return ERRCODE_DB_NOT_OPEN;
    }

    // Build INSERT SQL
    char sql[4096];
    int offset = 0;

    offset += snprintf(sql + offset, sizeof(sql) - offset, "INSERT INTO %s (", table_name);

    for (uint32_t i = 0; i < num_fields; i++)
    {
        if (i > 0)
        {
            offset += snprintf(sql + offset, sizeof(sql) - offset, ", ");
        }
        offset += snprintf(sql + offset, sizeof(sql) - offset, "%s", field_names[i]);
    }

    offset += snprintf(sql + offset, sizeof(sql) - offset, ") VALUES (");

    for (uint32_t i = 0; i < num_fields; i++)
    {
        if (i > 0)
        {
            offset += snprintf(sql + offset, sizeof(sql) - offset, ", ");
        }
        offset += snprintf(sql + offset, sizeof(sql) - offset, "?");
    }

    offset += snprintf(sql + offset, sizeof(sql) - offset, ");");

    LOG_INFO("[SQL] %s", sql);

    // Prepare statement
    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR("Failed to prepare INSERT: %s", sqlite3_errmsg(conn->handle));
        g_mutex_unlock(&conn->db_mutex);
        return ERRCODE_FAIL;
    }

    // Bind values
    for (uint32_t i = 0; i < num_fields; i++)
    {
        const db_value_t *val = &values[i];
        int bind_idx = i + 1;

        switch (val->type)
        {
            case DB_TYPE_NULL:
                sqlite3_bind_null(stmt, bind_idx);
                break;
            case DB_TYPE_INTEGER:
                sqlite3_bind_int64(stmt, bind_idx, val->data.i64);
                break;
            case DB_TYPE_REAL:
                sqlite3_bind_double(stmt, bind_idx, val->data.real);
                break;
            case DB_TYPE_TEXT:
                sqlite3_bind_text(stmt, bind_idx, val->data.text, -1, SQLITE_TRANSIENT);
                break;
            case DB_TYPE_BLOB:
                sqlite3_bind_blob(stmt, bind_idx, val->data.blob.data, val->data.blob.len, SQLITE_TRANSIENT);
                break;
        }
    }

    // Execute
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_DONE)
    {
        LOG_ERROR("INSERT failed: %s", sqlite3_errmsg(conn->handle));
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

int db_update(const char *db_name, const char *table_name, const char **field_names, const db_value_t *values,
              uint32_t num_fields, const char *where_clause)
{
    if (!db_name || !table_name || !field_names || !values || num_fields == 0)
    {
        return -1;
    }

    db_connection_t *conn = db_get_connection(db_name);
    if (!conn)
    {
        LOG_ERROR("错误: 找不到数据库连接 (db=%s)", db_name);
        return -1;
    }

    if (!conn->handle)
    {
        LOG_ERROR("错误: 数据库未连接 (db=%s)", db_name);
        return -1;
    }

    // Build UPDATE SQL
    char sql[4096];
    int offset = 0;

    offset += snprintf(sql + offset, sizeof(sql) - offset, "UPDATE %s SET ", table_name);

    for (uint32_t i = 0; i < num_fields; i++)
    {
        if (i > 0)
        {
            offset += snprintf(sql + offset, sizeof(sql) - offset, ", ");
        }
        offset += snprintf(sql + offset, sizeof(sql) - offset, "%s = ?", field_names[i]);
    }

    if (where_clause && where_clause[0] != '\0')
    {
        offset += snprintf(sql + offset, sizeof(sql) - offset, " WHERE %s", where_clause);
    }

    offset += snprintf(sql + offset, sizeof(sql) - offset, ";");

    LOG_INFO("[SQL] %s", sql);

    // Prepare statement
    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR("Failed to prepare UPDATE: %s", sqlite3_errmsg(conn->handle));
        g_mutex_unlock(&conn->db_mutex);
        return -1;
    }

    // Bind values
    for (uint32_t i = 0; i < num_fields; i++)
    {
        const db_value_t *val = &values[i];
        int bind_idx = i + 1;

        switch (val->type)
        {
            case DB_TYPE_NULL:
                sqlite3_bind_null(stmt, bind_idx);
                break;
            case DB_TYPE_INTEGER:
                sqlite3_bind_int64(stmt, bind_idx, val->data.i64);
                break;
            case DB_TYPE_REAL:
                sqlite3_bind_double(stmt, bind_idx, val->data.real);
                break;
            case DB_TYPE_TEXT:
                sqlite3_bind_text(stmt, bind_idx, val->data.text, -1, SQLITE_TRANSIENT);
                break;
            case DB_TYPE_BLOB:
                sqlite3_bind_blob(stmt, bind_idx, val->data.blob.data, val->data.blob.len, SQLITE_TRANSIENT);
                break;
        }
    }

    // Execute
    rc = sqlite3_step(stmt);
    int rows_changed = sqlite3_changes(conn->handle);
    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_DONE)
    {
        LOG_ERROR("UPDATE failed: %s", sqlite3_errmsg(conn->handle));
        return -1;
    }

    return rows_changed;
}

int db_delete(const char *db_name, const char *table_name, const char *where_clause)
{
    if (!db_name || !table_name)
    {
        return -1;
    }

    db_connection_t *conn = db_get_connection(db_name);
    if (!conn)
    {
        LOG_ERROR("错误: 找不到数据库连接 (db=%s)", db_name);
        return -1;
    }

    if (!conn->handle)
    {
        LOG_ERROR("错误: 数据库未连接 (db=%s)", db_name);
        return -1;
    }

    // Build DELETE SQL
    char sql[2048];
    int offset = 0;

    offset += snprintf(sql + offset, sizeof(sql) - offset, "DELETE FROM %s", table_name);

    if (where_clause && where_clause[0] != '\0')
    {
        offset += snprintf(sql + offset, sizeof(sql) - offset, " WHERE %s", where_clause);
    }

    offset += snprintf(sql + offset, sizeof(sql) - offset, ";");

    LOG_INFO("[SQL] %s", sql);

    // Execute
    g_mutex_lock(&conn->db_mutex);

    char *err_msg = NULL;
    int rc = sqlite3_exec(conn->handle, sql, NULL, NULL, &err_msg);
    int rows_changed = sqlite3_changes(conn->handle);

    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_OK)
    {
        LOG_ERROR("DELETE failed: %s", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return rows_changed;
}

int db_query(const char *db_name, const char *table_name, const char **field_names, uint32_t num_fields,
             const char *where_clause, db_result_t **result)
{
    if (!db_name || !table_name || !result)
    {
        return ERRCODE_FAIL;
    }

    db_connection_t *conn = db_get_connection(db_name);
    if (!conn)
    {
        LOG_ERROR("错误: 找不到数据库连接 (db=%s)", db_name);
        return ERRCODE_FAIL;
    }

    if (!conn->handle)
    {
        LOG_ERROR("错误: 数据库未连接 (db=%s)", db_name);
        return ERRCODE_FAIL;
    }

    // Build SELECT SQL
    char sql[4096];
    int offset = 0;

    offset += snprintf(sql + offset, sizeof(sql) - offset, "SELECT ");

    if (num_fields == 0 || field_names == NULL)
    {
        offset += snprintf(sql + offset, sizeof(sql) - offset, "*");
    }
    else
    {
        for (uint32_t i = 0; i < num_fields; i++)
        {
            if (i > 0)
            {
                offset += snprintf(sql + offset, sizeof(sql) - offset, ", ");
            }
            offset += snprintf(sql + offset, sizeof(sql) - offset, "%s", field_names[i]);
        }
    }

    offset += snprintf(sql + offset, sizeof(sql) - offset, " FROM %s", table_name);

    if (where_clause && where_clause[0] != '\0')
    {
        offset += snprintf(sql + offset, sizeof(sql) - offset, " WHERE %s", where_clause);
    }

    offset += snprintf(sql + offset, sizeof(sql) - offset, ";");

    LOG_INFO("[SQL] %s", sql);

    // Prepare statement
    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR("Failed to prepare SELECT: %s", sqlite3_errmsg(conn->handle));
        g_mutex_unlock(&conn->db_mutex);
        return ERRCODE_FAIL;
    }

    // Create result set
    db_result_t *res = g_malloc0(sizeof(db_result_t));
    res->rows = NULL;
    res->num_rows = 0;
    res->rows_capacity = 0;

    int col_count = sqlite3_column_count(stmt);

    // Fetch rows
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        // Resize array if needed
        if (res->num_rows >= res->rows_capacity)
        {
            res->rows_capacity = (res->rows_capacity == 0) ? 8 : res->rows_capacity * 2;
            res->rows = g_realloc(res->rows, res->rows_capacity * sizeof(db_row_t *));
        }

        // Create row
        db_row_t *row = g_malloc0(sizeof(db_row_t));
        row->num_fields = col_count;
        row->field_names = g_malloc0(col_count * sizeof(char *));
        row->values = g_malloc0(col_count * sizeof(db_value_t));

        for (int i = 0; i < col_count; i++)
        {
            row->field_names[i] = g_strdup((const char *)sqlite3_column_name(stmt, i));

            int col_type = sqlite3_column_type(stmt, i);
            switch (col_type)
            {
                case SQLITE_INTEGER:
                    row->values[i] = db_value_int(sqlite3_column_int64(stmt, i));
                    break;
                case SQLITE_FLOAT:
                    row->values[i] = db_value_real(sqlite3_column_double(stmt, i));
                    break;
                case SQLITE_TEXT:
                    row->values[i] = db_value_text((const char *)sqlite3_column_text(stmt, i));
                    break;
                case SQLITE_NULL:
                    row->values[i] = db_value_null();
                    break;
                default:
                    row->values[i] = db_value_null();
                    break;
            }
        }

        res->rows[res->num_rows++] = row;
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_DONE)
    {
        LOG_ERROR("SELECT failed: %s", sqlite3_errmsg(conn->handle));
        db_result_free(res);
        return ERRCODE_FAIL;
    }

    *result = res;
    return ERRCODE_SUCCESS;
}

int db_create_table(const char *db_name, const char *ddl)
{
    if (!db_name || !ddl || ddl[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    db_connection_t *conn = db_get_connection(db_name);
    if (!conn)
    {
        LOG_ERROR("错误: 找不到数据库连接 (db=%s)", db_name);
        return ERRCODE_FAIL;
    }

    if (!conn->handle)
    {
        LOG_ERROR("错误: 数据库未连接 (db=%s)", db_name);
        return ERRCODE_FAIL;
    }

    LOG_INFO("[SQL] %s", ddl);

    g_mutex_lock(&conn->db_mutex);
    char *err_msg = NULL;
    int rc = sqlite3_exec(conn->handle, ddl, NULL, NULL, &err_msg);
    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_OK)
    {
        LOG_ERROR("CREATE TABLE 失败: %s", err_msg);
        sqlite3_free(err_msg);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

// ============================================================================
// 建表定义辅助函数
// ============================================================================

/**
 * @brief 将 db_value_type_t 映射为 SQLite 类型字符串
 */
static const char *col_type_to_sql(db_value_type_t type)
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
 * @brief 根据 db_table_def_t 生成 CREATE TABLE IF NOT EXISTS SQL 语句
 * @param def      表定义
 * @param buf      输出缓冲区
 * @param buf_size 缓冲区大小
 * @return 写入的字节数，不含终止符
 */
static int build_create_table_sql(const db_table_def_t *def, char *buf, size_t buf_size)
{
    int offset = 0;
    offset += snprintf(buf + offset, buf_size - offset, "CREATE TABLE IF NOT EXISTS %s (\n", def->table_name);

    for (uint32_t i = 0; i < def->num_cols; i++)
    {
        const db_column_def_t *col = &def->cols[i];
        offset += snprintf(buf + offset, buf_size - offset, "  %s %s", col->name, col_type_to_sql(col->type));

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

        /* 末列不加逗号 */
        if (i < def->num_cols - 1)
        {
            offset += snprintf(buf + offset, buf_size - offset, ",\n");
        }
        else
        {
            offset += snprintf(buf + offset, buf_size - offset, "\n");
        }
    }

    offset += snprintf(buf + offset, buf_size - offset, ");");
    return offset;
}

int db_create_table_from_def(const char *db_name, const db_table_def_t *def)
{
    if (!db_name || !def || !def->table_name || !def->cols || def->num_cols == 0)
    {
        return ERRCODE_FAIL;
    }

    char sql[4096];
    build_create_table_sql(def, sql, sizeof(sql));

    LOG_INFO("[SQL] %s", sql);

    return db_create_table(db_name, sql);
}

int db_exists(const char *db_name, const char *table_name, const char *where_clause, gboolean *exists)
{
    if (!db_name || !table_name || !exists)
    {
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    const char *fields[] = {"1"};
    int ret = db_query(db_name, table_name, fields, 1, where_clause, &result);

    if (ret == ERRCODE_SUCCESS)
    {
        *exists = (result->num_rows > 0);
        db_result_free(result);
        return ERRCODE_SUCCESS;
    }

    return ERRCODE_FAIL;
}

// ============================================================================
// 通用 SQL 执行接口（供服务端 IPC handler 调用）
// ============================================================================

int db_exec_sql(const char *db_name, const char *sql)
{
    if (!db_name || !sql || sql[0] == '\0')
    {
        return -1;
    }

    db_connection_t *conn = db_get_connection(db_name);
    if (!conn || !conn->handle)
    {
        LOG_ERROR("错误: 找不到数据库连接 (db=%s)", db_name);
        return -1;
    }

    LOG_INFO("[SQL] %s", sql);

    g_mutex_lock(&conn->db_mutex);
    char *err_msg = NULL;
    int rc = sqlite3_exec(conn->handle, sql, NULL, NULL, &err_msg);
    int rows_changed = sqlite3_changes(conn->handle);
    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_OK)
    {
        LOG_ERROR("exec_sql 失败: %s", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return rows_changed;
}

int db_query_sql(const char *db_name, const char *sql, db_result_t **result)
{
    if (!db_name || !sql || sql[0] == '\0' || !result)
    {
        return ERRCODE_FAIL;
    }

    db_connection_t *conn = db_get_connection(db_name);
    if (!conn || !conn->handle)
    {
        LOG_ERROR("错误: 找不到数据库连接 (db=%s)", db_name);
        return ERRCODE_FAIL;
    }

    LOG_INFO("[SQL] %s", sql);

    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR("Failed to prepare query_sql: %s", sqlite3_errmsg(conn->handle));
        g_mutex_unlock(&conn->db_mutex);
        return ERRCODE_FAIL;
    }

    db_result_t *res = g_malloc0(sizeof(db_result_t));
    int col_count = sqlite3_column_count(stmt);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        if (res->num_rows >= res->rows_capacity)
        {
            res->rows_capacity = (res->rows_capacity == 0) ? 8 : res->rows_capacity * 2;
            res->rows = g_realloc(res->rows, res->rows_capacity * sizeof(db_row_t *));
        }

        db_row_t *row = g_malloc0(sizeof(db_row_t));
        row->num_fields = col_count;
        row->field_names = g_malloc0(col_count * sizeof(char *));
        row->values = g_malloc0(col_count * sizeof(db_value_t));

        for (int i = 0; i < col_count; i++)
        {
            row->field_names[i] = g_strdup((const char *)sqlite3_column_name(stmt, i));

            switch (sqlite3_column_type(stmt, i))
            {
                case SQLITE_INTEGER:
                    row->values[i] = db_value_int(sqlite3_column_int64(stmt, i));
                    break;
                case SQLITE_FLOAT:
                    row->values[i] = db_value_real(sqlite3_column_double(stmt, i));
                    break;
                case SQLITE_TEXT:
                    row->values[i] = db_value_text((const char *)sqlite3_column_text(stmt, i));
                    break;
                default:
                    row->values[i] = db_value_null();
                    break;
            }
        }

        res->rows[res->num_rows++] = row;
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_DONE)
    {
        LOG_ERROR("query_sql 失败: %s", sqlite3_errmsg(conn->handle));
        db_result_free(res);
        return ERRCODE_FAIL;
    }

    *result = res;
    return ERRCODE_SUCCESS;
}
