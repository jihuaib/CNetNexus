/**
 * @file   nn_db_api.c
 * @brief  数据库 CRUD 操作 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_db.h"
#include "nn_db_main.h"
#include "nn_db_registry.h"
#include "nn_errcode.h"

// ============================================================================
// Initialization API
// ============================================================================

int nn_db_initialize_all(void)
{
    if (!g_nn_db_local || !g_nn_db_local->registry)
    {
        fprintf(stderr, "[db] Context or registry not initialized\n");
        return NN_ERRCODE_FAIL;
    }

    nn_db_registry_t *registry = g_nn_db_local->registry;
    int failed_count = 0;

    g_mutex_lock(&registry->registry_mutex);

    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, registry->databases);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        nn_db_definition_t *db_def = (nn_db_definition_t *)value;
        if (nn_db_initialize_database(db_def) != NN_ERRCODE_SUCCESS)
        {
            fprintf(stderr, "[db] Failed to initialize database: %s\n", db_def->db_name);
            failed_count++;
        }
    }

    g_mutex_unlock(&registry->registry_mutex);

    if (failed_count > 0)
    {
        fprintf(stderr, "[db] %d database(s) failed to initialize\n", failed_count);
        return NN_ERRCODE_FAIL;
    }

    return NN_ERRCODE_SUCCESS;
}

// ============================================================================
// Value Helper Functions
// ============================================================================

nn_db_value_t nn_db_value_int(int64_t value)
{
    nn_db_value_t v;
    v.type = NN_DB_TYPE_INTEGER;
    v.data.i64 = value;
    return v;
}

nn_db_value_t nn_db_value_text(const char *value)
{
    nn_db_value_t v;
    v.type = NN_DB_TYPE_TEXT;
    v.data.text = value ? g_strdup(value) : NULL;
    return v;
}

nn_db_value_t nn_db_value_real(double value)
{
    nn_db_value_t v;
    v.type = NN_DB_TYPE_REAL;
    v.data.real = value;
    return v;
}

nn_db_value_t nn_db_value_null(void)
{
    nn_db_value_t v;
    v.type = NN_DB_TYPE_NULL;
    return v;
}

void nn_db_value_free(nn_db_value_t *value)
{
    if (!value)
    {
        return;
    }

    if (value->type == NN_DB_TYPE_TEXT && value->data.text)
    {
        g_free(value->data.text);
        value->data.text = NULL;
    }
    else if (value->type == NN_DB_TYPE_BLOB && value->data.blob.data)
    {
        g_free(value->data.blob.data);
        value->data.blob.data = NULL;
    }
}

// ============================================================================
// Result Management
// ============================================================================

void nn_db_result_free(nn_db_result_t *result)
{
    if (!result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        nn_db_row_t *row = result->rows[i];
        if (row)
        {
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                g_free(row->field_names[j]);
                nn_db_value_free(&row->values[j]);
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

int nn_db_insert(const char *db_name, const char *table_name, const char **field_names, const nn_db_value_t *values,
                 uint32_t num_fields)
{
    if (!db_name || !table_name || !field_names || !values || num_fields == 0)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_db_connection_t *conn = nn_db_get_connection(db_name);
    if (!conn || !conn->handle)
    {
        fprintf(stderr, "[db] Database not found: %s\n", db_name);
        return NN_ERRCODE_FAIL;
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

    // Prepare statement
    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to prepare INSERT: %s\n", sqlite3_errmsg(conn->handle));
        g_mutex_unlock(&conn->db_mutex);
        return NN_ERRCODE_FAIL;
    }

    // Bind values
    for (uint32_t i = 0; i < num_fields; i++)
    {
        const nn_db_value_t *val = &values[i];
        int bind_idx = i + 1;

        switch (val->type)
        {
            case NN_DB_TYPE_NULL:
                sqlite3_bind_null(stmt, bind_idx);
                break;
            case NN_DB_TYPE_INTEGER:
                sqlite3_bind_int64(stmt, bind_idx, val->data.i64);
                break;
            case NN_DB_TYPE_REAL:
                sqlite3_bind_double(stmt, bind_idx, val->data.real);
                break;
            case NN_DB_TYPE_TEXT:
                sqlite3_bind_text(stmt, bind_idx, val->data.text, -1, SQLITE_TRANSIENT);
                break;
            case NN_DB_TYPE_BLOB:
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
        fprintf(stderr, "[db] INSERT failed: %s\n", sqlite3_errmsg(conn->handle));
        return NN_ERRCODE_FAIL;
    }

    return NN_ERRCODE_SUCCESS;
}

int nn_db_update(const char *db_name, const char *table_name, const char **field_names, const nn_db_value_t *values,
                 uint32_t num_fields, const char *where_clause)
{
    if (!db_name || !table_name || !field_names || !values || num_fields == 0)
    {
        return -1;
    }

    nn_db_connection_t *conn = nn_db_get_connection(db_name);
    if (!conn || !conn->handle)
    {
        fprintf(stderr, "[db] Database not found: %s\n", db_name);
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

    // Prepare statement
    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to prepare UPDATE: %s\n", sqlite3_errmsg(conn->handle));
        g_mutex_unlock(&conn->db_mutex);
        return -1;
    }

    // Bind values
    for (uint32_t i = 0; i < num_fields; i++)
    {
        const nn_db_value_t *val = &values[i];
        int bind_idx = i + 1;

        switch (val->type)
        {
            case NN_DB_TYPE_NULL:
                sqlite3_bind_null(stmt, bind_idx);
                break;
            case NN_DB_TYPE_INTEGER:
                sqlite3_bind_int64(stmt, bind_idx, val->data.i64);
                break;
            case NN_DB_TYPE_REAL:
                sqlite3_bind_double(stmt, bind_idx, val->data.real);
                break;
            case NN_DB_TYPE_TEXT:
                sqlite3_bind_text(stmt, bind_idx, val->data.text, -1, SQLITE_TRANSIENT);
                break;
            case NN_DB_TYPE_BLOB:
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
        fprintf(stderr, "[db] UPDATE failed: %s\n", sqlite3_errmsg(conn->handle));
        return -1;
    }

    return rows_changed;
}

int nn_db_delete(const char *db_name, const char *table_name, const char *where_clause)
{
    if (!db_name || !table_name)
    {
        return -1;
    }

    nn_db_connection_t *conn = nn_db_get_connection(db_name);
    if (!conn || !conn->handle)
    {
        fprintf(stderr, "[db] Database not found: %s\n", db_name);
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

    // Execute
    g_mutex_lock(&conn->db_mutex);

    char *err_msg = NULL;
    int rc = sqlite3_exec(conn->handle, sql, NULL, NULL, &err_msg);
    int rows_changed = sqlite3_changes(conn->handle);

    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] DELETE failed: %s\n", err_msg);
        sqlite3_free(err_msg);
        return -1;
    }

    return rows_changed;
}

int nn_db_query(const char *db_name, const char *table_name, const char **field_names, uint32_t num_fields,
                const char *where_clause, nn_db_result_t **result)
{
    if (!db_name || !table_name || !result)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_db_connection_t *conn = nn_db_get_connection(db_name);
    if (!conn || !conn->handle)
    {
        fprintf(stderr, "[db] Database not found: %s\n", db_name);
        return NN_ERRCODE_FAIL;
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

    // Prepare statement
    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to prepare SELECT: %s\n", sqlite3_errmsg(conn->handle));
        g_mutex_unlock(&conn->db_mutex);
        return NN_ERRCODE_FAIL;
    }

    // Create result set
    nn_db_result_t *res = g_malloc0(sizeof(nn_db_result_t));
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
            res->rows = g_realloc(res->rows, res->rows_capacity * sizeof(nn_db_row_t *));
        }

        // Create row
        nn_db_row_t *row = g_malloc0(sizeof(nn_db_row_t));
        row->num_fields = col_count;
        row->field_names = g_malloc0(col_count * sizeof(char *));
        row->values = g_malloc0(col_count * sizeof(nn_db_value_t));

        for (int i = 0; i < col_count; i++)
        {
            row->field_names[i] = g_strdup((const char *)sqlite3_column_name(stmt, i));

            int col_type = sqlite3_column_type(stmt, i);
            switch (col_type)
            {
                case SQLITE_INTEGER:
                    row->values[i] = nn_db_value_int(sqlite3_column_int64(stmt, i));
                    break;
                case SQLITE_FLOAT:
                    row->values[i] = nn_db_value_real(sqlite3_column_double(stmt, i));
                    break;
                case SQLITE_TEXT:
                    row->values[i] = nn_db_value_text((const char *)sqlite3_column_text(stmt, i));
                    break;
                case SQLITE_NULL:
                    row->values[i] = nn_db_value_null();
                    break;
                default:
                    row->values[i] = nn_db_value_null();
                    break;
            }
        }

        res->rows[res->num_rows++] = row;
    }

    sqlite3_finalize(stmt);
    g_mutex_unlock(&conn->db_mutex);

    if (rc != SQLITE_DONE)
    {
        fprintf(stderr, "[db] SELECT failed: %s\n", sqlite3_errmsg(conn->handle));
        nn_db_result_free(res);
        return NN_ERRCODE_FAIL;
    }

    *result = res;
    return NN_ERRCODE_SUCCESS;
}

int nn_db_exists(const char *db_name, const char *table_name, const char *where_clause, gboolean *exists)
{
    if (!db_name || !table_name || !exists)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_db_result_t *result = NULL;
    const char *fields[] = {"1"};
    int ret = nn_db_query(db_name, table_name, fields, 1, where_clause, &result);

    if (ret == NN_ERRCODE_SUCCESS)
    {
        *exists = (result->num_rows > 0);
        nn_db_result_free(result);
        return NN_ERRCODE_SUCCESS;
    }

    return NN_ERRCODE_FAIL;
}

// ============================================================================
// Type Validation
// ============================================================================

gboolean nn_db_validate_field(const char *db_name, const char *table_name, const char *field_name,
                              const nn_db_value_t *value, char *error_msg, uint32_t error_msg_len)
{
    if (!db_name || !table_name || !field_name || !value)
    {
        return FALSE;
    }

    // Look up field definition from registry
    nn_db_field_t *field = nn_db_registry_find_field(db_name, table_name, field_name);
    if (!field || !field->param_type)
    {
        return TRUE; // No validation defined
    }

    // Convert value to string for validation
    char value_str[256];

    if (value->type == NN_DB_TYPE_INTEGER)
    {
        snprintf(value_str, sizeof(value_str), "%ld", value->data.i64);
    }
    else if (value->type == NN_DB_TYPE_TEXT)
    {
        if (value->data.text)
        {
            snprintf(value_str, sizeof(value_str), "%s", value->data.text);
        }
        else
        {
            value_str[0] = '\0';
        }
    }
    else
    {
        return TRUE; // Skip validation for other types
    }

    // Use existing CLI validation logic
    return nn_cfg_param_type_validate(field->param_type, value_str, error_msg, error_msg_len);
}
