/**
 * @file   db_rpc.h
 * @brief  数据库 RPC 接口，供其他模块通过 IPC 调用 DB 模块
 * @author jhb
 * @date   2026/02/11
 *
 * 全局只有一个 SQLite 数据库，调用方无需关心数据库名称，
 * 只需指定表名即可完成 CRUD 操作。
 */

#ifndef DB_RPC_H
#define DB_RPC_H

#include <glib.h>
#include <stdint.h>

#include "db.h"
#include "ipc.h"

/**
 * @brief 通过 RPC 向数据库表中插入一行数据
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param field_names 字段名称数组
 * @param values     值数组（长度须与 field_names 一致）
 * @param num_fields 字段数量
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_insert(ipc_context_t *ctx, const char *table_name, const char **field_names, const db_value_t *values,
                  uint32_t num_fields);

/**
 * @brief 通过 RPC 更新数据库中符合条件的行
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param field_names 待更新的字段名称数组
 * @param values     新值数组
 * @param num_fields 待更新的字段数量
 * @param where_clause SQL WHERE 子句（如 "as_number = 65001"），为 NULL 则更新所有行
 * @return 更新的行数，错误返回 -1
 */
int db_rpc_update(ipc_context_t *ctx, const char *table_name, const char **field_names, const db_value_t *values,
                  uint32_t num_fields, const char *where_clause);

/**
 * @brief 通过 RPC 删除数据库中符合条件的行
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param where_clause SQL WHERE 子句，为 NULL 则删除所有行
 * @return 删除的行数，错误返回 -1
 */
int db_rpc_delete(ipc_context_t *ctx, const char *table_name, const char *where_clause);

/**
 * @brief 通过 RPC 查询数据库表中的行
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param field_names 待查询的字段名称数组（为 NULL 则查询所有字段）
 * @param num_fields 字段数量（为 0 则查询所有字段）
 * @param where_clause SQL WHERE 子句，为 NULL 则查询所有行
 * @param result     输出结果集（调用者须通过 db_result_free 释放）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_query(ipc_context_t *ctx, const char *table_name, const char **field_names, uint32_t num_fields,
                 const char *where_clause, db_result_t **result);

/**
 * @brief 通过 RPC 检查数据库中是否存在符合条件的行
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param where_clause SQL WHERE 子句
 * @param exists     输出布尔值（存在则为 TRUE）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_exists(ipc_context_t *ctx, const char *table_name, const char *where_clause, gboolean *exists);

/**
 * @brief 通过 RPC 执行建表 DDL
 * @param ctx  调用方模块的 IPC 上下文
 * @param ddl  完整的 CREATE TABLE ... SQL 语句（通常含 IF NOT EXISTS）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_create_table(ipc_context_t *ctx, const char *ddl);

/**
 * @brief 通过 RPC 按结构化定义建表（自动生成 DDL 并打印 SQL）
 * @param ctx 调用方模块的 IPC 上下文
 * @param def 表定义（表名、列列表及约束）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_create_table_from_def(ipc_context_t *ctx, const db_table_def_t *def);

#endif // DB_RPC_H
