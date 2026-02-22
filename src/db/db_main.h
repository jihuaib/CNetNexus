/**
 * @file   db_main.h
 * @brief  数据库模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef DB_MAIN_H
#define DB_MAIN_H

#include <glib.h>
#include <sqlite3.h>
#include <stdint.h>

#include "db_registry.h"
#include "ipc.h"

// ============================================================================
// Runtime Database Connection
// ============================================================================

typedef struct db_connection
{
    char *db_path;            // Path to SQLite database file
    sqlite3 *handle;          // SQLite handle
    GMutex db_mutex;          // Per-database mutex for thread safety
    uint32_t owner_module_id; // Module ID that owns this DB
    ipc_context_t *ipc_ctx;   // IPC context for RPC (if remote)
} db_connection_t;

// ============================================================================
// Module Context
// ============================================================================

typedef struct db_local
{
    GHashTable *connections; // Map: db_name (char*) -> db_connection_t*
    db_registry_t *registry; // Database definitions registry
    ipc_context_t *ipc_ctx;  // IPC 上下文（由 DEV 创建和管理）
} db_local_t;

extern db_local_t *g_db_local;

// ============================================================================
// Internal Module Functions
// ============================================================================

/**
 * @brief Get database connection by name
 * @param db_name Database name
 * @return Connection or NULL if not found
 */
db_connection_t *db_get_connection(const char *db_name);

/**
 * @brief Free a database connection
 * @param conn Connection to free
 */
void db_connection_free(db_connection_t *conn);

// ============================================================================
// Schema Management Functions (db_schema.c)
// ============================================================================

/**
 * @brief Create a database file and open connection
 * @param db_name Database name
 * @param db_path Path to database file
 * @param handle Output SQLite handle
 * @return ERRCODE_SUCCESS or ERRCODE_FAIL
 */
int db_create_database_file(const char *db_name, const char *db_path, sqlite3 **handle);

/**
 * @brief Create a table from its definition
 * @param handle SQLite handle
 * @param table_name Table name
 * @param table_def Table definition
 * @return ERRCODE_SUCCESS or ERRCODE_FAIL
 */
int db_create_table(sqlite3 *handle, const char *table_name, db_table_t *table_def);

/**
 * @brief Initialize database schema from definition
 * @param db_def Database definition
 * @return ERRCODE_SUCCESS or ERRCODE_FAIL
 */
int db_initialize_database(db_definition_t *db_def);

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void db_msg_handler(ipc_context_t *ctx, ipc_message_t *msg);

#endif // DB_MAIN_H
