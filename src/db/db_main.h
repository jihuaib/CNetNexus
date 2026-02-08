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

// ============================================================================
// 数据库连接
// ============================================================================

/** 运行时数据库连接 */
typedef struct db_connection
{
    char *db_path;   /**< SQLite 数据库文件路径 */
    sqlite3 *handle; /**< SQLite 句柄 */
    GMutex db_mutex; /**< 线程安全互斥锁 */
} db_connection_t;

// ============================================================================
// 模块上下文
// ============================================================================

/** IPC 上下文前向声明 */
typedef struct ipc_context ipc_context_t;

/** 模块全局状态 */
typedef struct db_local
{
    GHashTable *connections;    /**< 数据库连接表: db_name -> db_connection_t* */
    db_registry_t *registry; /**< 数据库 schema 定义注册表 */
    ipc_context_t *ipc_ctx;  /**< IPC 上下文（接收远程请求） */
} db_local_t;

/** 全局上下文实例 */
extern db_local_t *g_db_local;

// ============================================================================
// 模块生命周期
// ============================================================================

/**
 * @brief 模块初始化
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int32_t db_module_init(void);

/**
 * @brief 模块清理
 */
void db_module_cleanup(void);

/**
 * @brief 获取数据库连接
 * @param db_name 数据库名称
 * @return 连接对象，未找到返回 NULL
 */
db_connection_t *db_get_connection(const char *db_name);

// ============================================================================
// Schema 管理（db_schema.c）
// ============================================================================

/**
 * @brief 创建数据库文件并打开连接
 * @param db_name 数据库名称
 * @param db_path 数据库文件路径
 * @param handle 输出 SQLite 句柄
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_create_database_file(const char *db_name, const char *db_path, sqlite3 **handle);

/**
 * @brief 根据定义创建表
 * @param handle SQLite 句柄
 * @param table_name 表名
 * @param table_def 表定义
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_create_table(sqlite3 *handle, const char *table_name, db_table_t *table_def);

/**
 * @brief 根据定义初始化数据库 schema
 * @param db_def 数据库定义
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_initialize_database(db_definition_t *db_def);

#endif // DB_MAIN_H
