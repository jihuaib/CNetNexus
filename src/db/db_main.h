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

#include "cli.h"
#include "dev.h"

// ============================================================================
// Runtime Database Connection
// ============================================================================

typedef struct db_connection
{
    char *db_path;                  // Path to SQLite database file
    sqlite3 *handle;                // SQLite handle
    GMutex db_mutex;                // Per-database mutex for thread safety
    uint32_t owner_module_id;       // Module ID that owns this DB
    dev_ipc_context_t *dev_ipc_ctx; // IPC context for RPC (if remote)
} db_connection_t;

// ============================================================================
// Module Context
// ============================================================================

typedef struct db_local
{
    db_connection_t *main_conn;     /**< 全局统一数据库连接（所有模块共享同一个 SQLite 文件） */
    dev_ipc_context_t *dev_ipc_ctx; /**< IPC 上下文（由 DEV 创建和管理） */
    cli_chunk_stream_t show_stream; /**< CLI show 命令分片输出状态 */
} db_local_t;

extern db_local_t *g_db_local;

/**
 * @brief 获取 DB 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *db_local_ipc_ctx(void)
{
    return g_db_local->dev_ipc_ctx;
}

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
 * @brief 打开统一数据库文件，建立 main_conn（仅执行一次）
 * @return ERRCODE_SUCCESS or ERRCODE_FAIL
 */
int db_initialize_database(void);

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void db_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief DB 模块初始化（由 db_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int db_module_init(void);

#endif // DB_MAIN_H
