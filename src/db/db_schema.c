/**
 * @file   db_schema.c
 * @brief  数据库 Schema 初始化与连接管理
 * @author jhb
 * @date   2026/01/22
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "db_main.h"
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
 * @brief 获取统一数据库文件路径（所有模块共享同一个数据库文件）
 *
 * 设置了 NN_WORK_DIR 时使用 $NN_WORK_DIR/data/netnexus.db，
 * 否则回退到 ./data/netnexus.db（开发环境）。
 */
static int get_database_path(char *path_buf, size_t buf_size)
{
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir != NULL)
    {
        snprintf(path_buf, buf_size, "%s/data/netnexus.db", work_dir);
    }
    else
    {
        snprintf(path_buf, buf_size, "./data/netnexus.db");
    }
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
// Schema Initialization
// ============================================================================

/**
 * @brief 打开统一数据库文件，建立 main_conn（仅执行一次）
 */
int db_initialize_database(void)
{
    if (g_db_local->main_conn)
    {
        return ERRCODE_SUCCESS;
    }

    char db_path[512];
    if (get_database_path(db_path, sizeof(db_path)) != 0)
    {
        LOG_ERROR("Failed to get unified database path");
        return ERRCODE_FAIL;
    }

    sqlite3 *handle = NULL;
    if (db_create_database_file("netnexus", db_path, &handle) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    db_connection_t *conn = g_malloc0(sizeof(db_connection_t));
    conn->db_path = g_strdup(db_path);
    conn->handle = handle;
    g_mutex_init(&conn->db_mutex);

    g_db_local->main_conn = conn;
    LOG_INFO("Unified database connection established: %s", db_path);
    return ERRCODE_SUCCESS;
}
