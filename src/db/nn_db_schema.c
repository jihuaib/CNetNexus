#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nn_db_main.h"
#include "nn_db_registry.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Create directory recursively
 */
static int create_directory_recursive(const char *path)
{
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
    {
        tmp[len - 1] = 0;
    }

    for (p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
            {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
    {
        return -1;
    }

    return 0;
}

/**
 * @brief Get database file path for a given database name and module ID
 */
static int get_database_path(const char *db_name, uint32_t module_id, char *path_buf, size_t buf_size)
{
    char module_name[32];

    // Get module name
    if (nn_dev_get_module_name(module_id, module_name) != NN_ERRCODE_SUCCESS)
    {
        snprintf(module_name, sizeof(module_name), "module_%u", module_id);
    }

    // Use ./data for development
    snprintf(path_buf, buf_size, "./data/%s/%s.db", module_name, db_name);

    return 0;
}

// ============================================================================
// Database Creation
// ============================================================================

/**
 * @brief Create a database file and open connection
 */
int nn_db_create_database_file(const char *db_name, const char *db_path, sqlite3 **handle)
{
    // Create parent directory
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s", db_path);

    char *last_slash = strrchr(dir_path, '/');
    if (last_slash)
    {
        *last_slash = '\0';
        if (create_directory_recursive(dir_path) != 0)
        {
            fprintf(stderr, "[db] Failed to create directory: %s\n", dir_path);
            return NN_ERRCODE_FAIL;
        }
    }

    // Open/create database file
    int rc = sqlite3_open(db_path, handle);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to open database %s: %s\n", db_name, sqlite3_errmsg(*handle));
        sqlite3_close(*handle);
        return NN_ERRCODE_FAIL;
    }

    // Configure SQLite for better concurrency
    char *err_msg = NULL;

    // Enable WAL mode
    rc = sqlite3_exec(*handle, "PRAGMA journal_mode=WAL;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to enable WAL mode: %s\n", err_msg);
        sqlite3_free(err_msg);
        // Non-fatal, continue
    }

    // Enable foreign keys
    rc = sqlite3_exec(*handle, "PRAGMA foreign_keys=ON;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to enable foreign keys: %s\n", err_msg);
        sqlite3_free(err_msg);
        // Non-fatal, continue
    }

    // Set busy timeout
    sqlite3_busy_timeout(*handle, 5000);

    printf("[db] Created database file: %s\n", db_path);
    return NN_ERRCODE_SUCCESS;
}

// ============================================================================
// Table Creation
// ============================================================================

/**
 * @brief Create a table from its definition
 */
int nn_db_create_table(sqlite3 *handle, const char *table_name, nn_db_table_t *table_def)
{
    if (!handle || !table_name || !table_def)
    {
        return NN_ERRCODE_FAIL;
    }

    // Build CREATE TABLE SQL
    char sql[4096];
    int offset = 0;

    offset += snprintf(sql + offset, sizeof(sql) - offset, "CREATE TABLE IF NOT EXISTS %s (", table_name);

    for (uint32_t i = 0; i < table_def->num_fields; i++)
    {
        nn_db_field_t *field = table_def->fields[i];

        if (i > 0)
        {
            offset += snprintf(sql + offset, sizeof(sql) - offset, ", ");
        }

        offset += snprintf(sql + offset, sizeof(sql) - offset, "%s %s", field->field_name, field->sql_type);
    }

    offset += snprintf(sql + offset, sizeof(sql) - offset, ");");

    // Execute CREATE TABLE
    char *err_msg = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "[db] Failed to create table %s: %s\n", table_name, err_msg);
        sqlite3_free(err_msg);
        return NN_ERRCODE_FAIL;
    }

    printf("[db]   Created table: %s\n", table_name);
    return NN_ERRCODE_SUCCESS;
}

// ============================================================================
// Schema Initialization
// ============================================================================

/**
 * @brief Initialize database schema from definition
 */
int nn_db_initialize_database(nn_db_definition_t *db_def)
{
    if (!db_def)
    {
        return NN_ERRCODE_FAIL;
    }

    printf("[db] Initializing database: %s\n", db_def->db_name);

    // Get database file path
    char db_path[512];
    if (get_database_path(db_def->db_name, db_def->module_id, db_path, sizeof(db_path)) != 0)
    {
        fprintf(stderr, "[db] Failed to get database path for: %s\n", db_def->db_name);
        return NN_ERRCODE_FAIL;
    }

    // Create database file and open connection
    sqlite3 *handle = NULL;
    if (nn_db_create_database_file(db_def->db_name, db_path, &handle) != NN_ERRCODE_SUCCESS)
    {
        return NN_ERRCODE_FAIL;
    }

    // Create tables
    for (uint32_t i = 0; i < db_def->num_tables; i++)
    {
        nn_db_table_t *table = db_def->tables[i];
        if (nn_db_create_table(handle, table->table_name, table) != NN_ERRCODE_SUCCESS)
        {
            sqlite3_close(handle);
            return NN_ERRCODE_FAIL;
        }
    }

    // Store connection in context
    nn_db_connection_t *conn = g_malloc0(sizeof(nn_db_connection_t));
    conn->db_path = g_strdup(db_path);
    conn->handle = handle;
    g_mutex_init(&conn->db_mutex);

    g_hash_table_insert(g_nn_db_local->connections, g_strdup(db_def->db_name), conn);

    printf("[db] Database initialized: %s\n", db_def->db_name);
    return NN_ERRCODE_SUCCESS;
}
