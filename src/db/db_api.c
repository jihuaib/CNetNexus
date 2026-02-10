/**
 * @file   db_api.c
 * @brief  数据库 CRUD 操作 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "db_main.h"
#include "db_registry.h"
#include "db_serialize.h"
#include "errcode.h"
#include "ipc.h"

// ============================================================================
// Initialization API
// ============================================================================

int db_client_init(ipc_context_t *ipc_ctx)
{
    if (g_db_local)
    {
        return ERRCODE_SUCCESS;
    }

    g_db_local = g_malloc0(sizeof(db_local_t));
    g_db_local->connections =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)db_connection_free);
    g_db_local->registry = db_registry_get_instance();
    g_db_local->ipc_ctx = ipc_ctx;

    return ERRCODE_SUCCESS;
}

int db_initialize_all(void)
{
    if (!g_db_local || !g_db_local->registry)
    {
        fprintf(stderr, "[db] Context or registry not initialized\n");
        return ERRCODE_FAIL;
    }

    db_registry_t *registry = g_db_local->registry;
    int failed_count = 0;

    g_mutex_lock(&registry->registry_mutex);

    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, registry->databases);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        db_definition_t *db_def = (db_definition_t *)value;
        uint32_t my_module_id = g_db_local->ipc_ctx ? ipc_get_module_id(g_db_local->ipc_ctx) : 0;

        if (db_def->module_id == my_module_id)
        {
            // Local Database used by owner
            if (db_initialize_database(db_def) != ERRCODE_SUCCESS)
            {
                fprintf(stderr, "[db] Failed to initialize database: %s\n", db_def->db_name);
                failed_count++;
            }
            else
            {
                // Update connection info
                db_connection_t *conn = db_get_connection(db_def->db_name);
                if (conn)
                {
                    conn->owner_module_id = db_def->module_id;
                    conn->ipc_ctx = NULL; // Local access
                }
            }
        }
        else
        {
            // Remote Database (RPC)
            db_connection_t *conn = g_malloc0(sizeof(db_connection_t));
            conn->db_path = NULL; // Remote
            conn->handle = NULL;  // Remote
            conn->owner_module_id = db_def->module_id;
            conn->ipc_ctx = g_db_local->ipc_ctx;
            g_mutex_init(&conn->db_mutex);

            // Add to connections map
            g_hash_table_insert(g_db_local->connections, g_strdup(db_def->db_name), conn);

            // printf("[db] Initialized remote connection to %s (Owner: 0x%X)\n", db_def->db_name, db_def->module_id);
        }
    }

    g_mutex_unlock(&registry->registry_mutex);

    if (failed_count > 0)
    {
        fprintf(stderr, "[db] %d database(s) failed to initialize\n", failed_count);
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

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
        fprintf(stderr, "[db] 错误: 找不到数据库连接 (db=%s)\n", db_name);
        return ERRCODE_FAIL;
    }

    // Check if remote (no handle, but has IPC context)
    if (!conn->handle && conn->ipc_ctx)
    {
        void *payload = NULL;
        uint32_t payload_len = 0;
        db_serialize_request_insert(db_name, table_name, field_names, values, num_fields, &payload, &payload_len);

        // Use conn->owner_module_id as destination
        uint32_t target_id = (conn->owner_module_id << 16);

        ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_INSERT, ipc_get_module_id(conn->ipc_ctx),
                                                conn->owner_module_id, 0, payload, payload_len, g_free);

        ipc_message_t *resp = ipc_query(conn->ipc_ctx, target_id, req, 5000);
        ipc_message_free(req);

        if (!resp)
        {
            fprintf(stderr, "[db_api] RPC 插入失败: 无响应\n");
            return ERRCODE_FAIL;
        }

        int32_t retval = ERRCODE_FAIL;
        db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
        ipc_message_free(resp);
        return retval;
    }

    if (!conn->handle)
    {
        fprintf(stderr, "[db] 错误: 数据库未连接 (db=%s)\n", db_name);
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

    // Prepare statement
    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to prepare INSERT: %s\n", sqlite3_errmsg(conn->handle));
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
        fprintf(stderr, "[db] INSERT failed: %s\n", sqlite3_errmsg(conn->handle));
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
        fprintf(stderr, "[db] 错误: 找不到数据库连接 (db=%s)\n", db_name);
        return -1;
    }

    if (!conn->handle && conn->ipc_ctx)
    {
        void *payload = NULL;
        uint32_t payload_len = 0;
        db_serialize_request_update(db_name, table_name, field_names, values, num_fields, where_clause, &payload,
                                    &payload_len);

        uint32_t target_id = (conn->owner_module_id << 16);

        ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_UPDATE, ipc_get_module_id(conn->ipc_ctx),
                                                conn->owner_module_id, 0, payload, payload_len, g_free);

        ipc_message_t *resp = ipc_query(conn->ipc_ctx, target_id, req, 5000);
        ipc_message_free(req);

        if (!resp)
        {
            fprintf(stderr, "[db_api] RPC 更新失败: 无响应\n");
            return -1;
        }

        int32_t retval = -1;
        db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
        ipc_message_free(resp);
        return retval;
    }

    if (!conn->handle)
    {
        fprintf(stderr, "[db] 错误: 数据库未连接 (db=%s)\n", db_name);
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
        fprintf(stderr, "[db] UPDATE failed: %s\n", sqlite3_errmsg(conn->handle));
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
        fprintf(stderr, "[db] 错误: 找不到数据库连接 (db=%s)\n", db_name);
        return -1;
    }

    if (!conn->handle && conn->ipc_ctx)
    {
        void *payload = NULL;
        uint32_t payload_len = 0;
        db_serialize_request_delete(db_name, table_name, where_clause, &payload, &payload_len);

        uint32_t target_id = (conn->owner_module_id << 16);

        ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_DELETE, ipc_get_module_id(conn->ipc_ctx),
                                                conn->owner_module_id, 0, payload, payload_len, g_free);

        ipc_message_t *resp = ipc_query(conn->ipc_ctx, target_id, req, 5000);
        ipc_message_free(req);

        if (!resp)
        {
            fprintf(stderr, "[db_api] RPC 删除失败: 无响应\n");
            return -1;
        }

        int32_t retval = -1;
        db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
        ipc_message_free(resp);
        return retval;
    }

    if (!conn->handle)
    {
        fprintf(stderr, "[db] 错误: 数据库未连接 (db=%s)\n", db_name);
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
        fprintf(stderr, "[db] 错误: 找不到数据库连接 (db=%s)\n", db_name);
        return ERRCODE_FAIL;
    }

    if (!conn->handle && conn->ipc_ctx)
    {
        void *payload = NULL;
        uint32_t payload_len = 0;
        db_serialize_request_query(db_name, table_name, field_names, num_fields, where_clause, &payload, &payload_len);

        uint32_t target_id = (conn->owner_module_id << 16);

        ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_QUERY, ipc_get_module_id(conn->ipc_ctx),
                                                conn->owner_module_id, 0, payload, payload_len, g_free);

        ipc_message_t *resp = ipc_query(conn->ipc_ctx, target_id, req, 5000);
        ipc_message_free(req);

        if (!resp)
        {
            fprintf(stderr, "[db_api] RPC 查询失败: 无响应\n");
            return ERRCODE_FAIL;
        }

        int32_t retval = ERRCODE_FAIL;
        db_deserialize_response(resp->payload, resp->payload_len, &retval, result);
        ipc_message_free(resp);
        return retval;
    }

    if (!conn->handle)
    {
        fprintf(stderr, "[db] 错误: 数据库未连接 (db=%s)\n", db_name);
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

    // Prepare statement
    sqlite3_stmt *stmt;
    g_mutex_lock(&conn->db_mutex);

    int rc = sqlite3_prepare_v2(conn->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to prepare SELECT: %s\n", sqlite3_errmsg(conn->handle));
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
        fprintf(stderr, "[db] SELECT failed: %s\n", sqlite3_errmsg(conn->handle));
        db_result_free(res);
        return ERRCODE_FAIL;
    }

    *result = res;
    return ERRCODE_SUCCESS;
}

int db_exists(const char *db_name, const char *table_name, const char *where_clause, gboolean *exists)
{
    if (!db_name || !table_name || !exists)
    {
        return ERRCODE_FAIL;
    }

    // Check for local connection first
    // Check for local connection first
    db_connection_t *conn = db_get_connection(db_name);
    if (!conn)
    {
        return ERRCODE_FAIL;
    }

    if (!conn->handle && conn->ipc_ctx)
    {
        void *payload = NULL;
        uint32_t payload_len = 0;
        db_serialize_request_exists(db_name, table_name, where_clause, &payload, &payload_len);

        uint32_t target_id = (conn->owner_module_id << 16);

        ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_EXISTS, ipc_get_module_id(conn->ipc_ctx),
                                                conn->owner_module_id, 0, payload, payload_len, g_free);
        ipc_message_t *resp = ipc_query(conn->ipc_ctx, target_id, req, 5000);
        ipc_message_free(req);

        if (!resp)
        {
            return ERRCODE_FAIL;
        }

        int32_t retval = 0;
        db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
        ipc_message_free(resp);

        if (retval == ERRCODE_FAIL)
        {
            return ERRCODE_FAIL;
        }

        *exists = (retval != 0);
        return ERRCODE_SUCCESS;
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
// Type Validation
// ============================================================================

gboolean db_validate_field(const char *db_name, const char *table_name, const char *field_name, const db_value_t *value,
                           char *error_msg, uint32_t error_msg_len)
{
    if (!db_name || !table_name || !field_name || !value)
    {
        return FALSE;
    }

    // Look up field definition from registry
    db_field_t *field = db_registry_find_field(db_name, table_name, field_name);
    if (!field || !field->param_type)
    {
        return TRUE; // No validation defined
    }

    // Convert value to string for validation
    char value_str[256];

    if (value->type == DB_TYPE_INTEGER)
    {
        snprintf(value_str, sizeof(value_str), "%ld", value->data.i64);
    }
    else if (value->type == DB_TYPE_TEXT)
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
    return cfg_param_type_validate(field->param_type, value_str, error_msg, error_msg_len);
}
