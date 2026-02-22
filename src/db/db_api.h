/**
 * @file   db_api.h
 * @brief  数据库本地 CRUD 操作 API（DB 模块内部使用）
 * @author jhb
 * @date   2026/02/11
 */

#ifndef DB_API_H
#define DB_API_H

#include <glib.h>
#include <stdint.h>

#include "db.h"

/**
 * @brief 向表中插入一行数据（本地 SQLite 操作）
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param field_names 字段名称数组
 * @param values 值数组
 * @param num_fields 字段数量
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_insert(const char *db_name, const char *table_name, const char **field_names, const db_value_t *values,
              uint32_t num_fields);

/**
 * @brief 更新符合条件的行（本地 SQLite 操作）
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param field_names 待更新的字段名称数组
 * @param values 新值数组
 * @param num_fields 待更新的字段数量
 * @param where_clause SQL WHERE 子句，为 NULL 则更新所有行
 * @return 更新的行数，错误返回 -1
 */
int db_update(const char *db_name, const char *table_name, const char **field_names, const db_value_t *values,
              uint32_t num_fields, const char *where_clause);

/**
 * @brief 删除符合条件的行（本地 SQLite 操作）
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param where_clause SQL WHERE 子句，为 NULL 则删除所有行
 * @return 删除的行数，错误返回 -1
 */
int db_delete(const char *db_name, const char *table_name, const char *where_clause);

/**
 * @brief 查询表中的行（本地 SQLite 操作）
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param field_names 待查询的字段名称数组（为 NULL 则查询所有字段）
 * @param num_fields 字段数量（为 0 则查询所有字段）
 * @param where_clause SQL WHERE 子句，为 NULL 则查询所有行
 * @param result 输出结果集（调用者须通过 db_result_free 释放）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_query(const char *db_name, const char *table_name, const char **field_names, uint32_t num_fields,
             const char *where_clause, db_result_t **result);

/**
 * @brief 检查是否存在符合条件的行（本地 SQLite 操作）
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param where_clause SQL WHERE 子句
 * @param exists 输出布尔值（存在则为 TRUE）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_exists(const char *db_name, const char *table_name, const char *where_clause, gboolean *exists);

#endif // DB_API_H
