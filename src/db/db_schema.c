/**
 * @file   db_schema.c
 * @brief  数据库 Schema 管理，表创建和迁移
 * @author jhb
 * @date   2026/01/22
 */
#define LOG_TAG "db"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "db_main.h"
#include "db_registry.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

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
static int get_database_path(const char *db_name, char *path_buf, size_t buf_size)
{
    // Use ./data for development
    snprintf(path_buf, buf_size, "./data/%s.db", db_name);

    return 0;
}

// ============================================================================
// Database Creation
// ============================================================================

/**
 * @brief Create a database file and open connection
 */
int db_create_database_file(const char *db_name, const char *db_path, sqlite3 **handle)
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
            LOG_ERROR("Failed to create directory: %s", dir_path);
            return ERRCODE_FAIL;
        }
    }

    // Open/create database file
    int rc = sqlite3_open(db_path, handle);
    if (rc != SQLITE_OK)
    {
        LOG_ERROR("Failed to open database %s: %s", db_name, sqlite3_errmsg(*handle));
        sqlite3_close(*handle);
        return ERRCODE_FAIL;
    }

    // Configure SQLite for better concurrency
    char *err_msg = NULL;

    // Enable WAL mode
    rc = sqlite3_exec(*handle, "PRAGMA journal_mode=WAL;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK)
    {
        LOG_WARN("Failed to enable WAL mode: %s", err_msg);
        sqlite3_free(err_msg);
        // Non-fatal, continue
    }

    // Enable foreign keys
    rc = sqlite3_exec(*handle, "PRAGMA foreign_keys=ON;", NULL, NULL, &err_msg);
    if (rc != SQLITE_OK)
    {
        LOG_WARN("Failed to enable foreign keys: %s", err_msg);
        sqlite3_free(err_msg);
        // Non-fatal, continue
    }

    // Set busy timeout
    sqlite3_busy_timeout(*handle, 5000);

    LOG_INFO("Created database file: %s", db_path);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Table Creation
// ============================================================================

/**
 * @brief Create a table from its definition
 */
int db_create_table(sqlite3 *handle, const char *table_name, db_table_t *table_def)
{
    if (!handle || !table_name || !table_def)
    {
        return ERRCODE_FAIL;
    }

    // Build CREATE TABLE SQL
    char sql[4096];
    int offset = 0;

    offset += snprintf(sql + offset, sizeof(sql) - offset, "CREATE TABLE IF NOT EXISTS %s (", table_name);

    for (uint32_t i = 0; i < table_def->num_fields; i++)
    {
        db_field_t *field = table_def->fields[i];

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
        LOG_ERROR("Failed to create table %s: %s", table_name, err_msg);
        sqlite3_free(err_msg);
        return ERRCODE_FAIL;
    }

    LOG_INFO("Created table: %s", table_name);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Schema Initialization
// ============================================================================

/**
 * @brief Initialize database schema from definition
 */
int db_initialize_database(db_definition_t *db_def)
{
    if (!db_def)
    {
        return ERRCODE_FAIL;
    }

    LOG_INFO("Initializing database: %s", db_def->db_name);

    // Get database file path
    char db_path[512];
    if (get_database_path(db_def->db_name, db_path, sizeof(db_path)) != 0)
    {
        LOG_ERROR("Failed to get database path for: %s", db_def->db_name);
        return ERRCODE_FAIL;
    }

    // Create database file and open connection
    sqlite3 *handle = NULL;
    if (db_create_database_file(db_def->db_name, db_path, &handle) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    // Create tables
    for (uint32_t i = 0; i < db_def->num_tables; i++)
    {
        db_table_t *table = db_def->tables[i];
        if (db_create_table(handle, table->table_name, table) != ERRCODE_SUCCESS)
        {
            sqlite3_close(handle);
            return ERRCODE_FAIL;
        }
    }

    // Store connection in context
    db_connection_t *conn = g_malloc0(sizeof(db_connection_t));
    conn->db_path = g_strdup(db_path);
    conn->handle = handle;
    g_mutex_init(&conn->db_mutex);

    g_hash_table_insert(g_db_local->connections, g_strdup(db_def->db_name), conn);

    LOG_INFO("Database initialized: %s", db_def->db_name);
    return ERRCODE_SUCCESS;
}
