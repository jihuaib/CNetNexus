/**
 * @file   db_serialize.h
 * @brief  数据库序列化/反序列化头文件
 * @author jhb
 * @date   2026/02/10
 */

#ifndef DB_SERIALIZE_H
#define DB_SERIALIZE_H

#include <stddef.h>
#include <stdint.h>

#include "db.h"

/**
 * @brief 序列化 SQL 请求（db_name + sql 字符串），适用于 EXEC_SQL 和 QUERY_SQL
 * @param db_name   数据库名称
 * @param sql       完整 SQL 语句
 * @param out_data  输出序列化数据（调用方负责 g_free）
 * @param out_len   输出数据长度
 */
void db_serialize_request_sql(const char *db_name, const char *sql, void **out_data, uint32_t *out_len);

/**
 * @brief 反序列化 SQL 请求
 * @param data      序列化数据
 * @param len       数据长度
 * @param db_name   输出数据库名称（调用方负责 g_free）
 * @param sql       输出 SQL 语句（调用方负责 g_free）
 * @return 0 成功，-1 失败
 */
int db_deserialize_request_sql(const void *data, uint32_t len, char **db_name, char **sql);

/**
 * @brief 序列化响应（retval + 可选结果集）
 * @param retval  返回值（操作码或影响行数）
 * @param result  查询结果集，无则传 NULL
 * @param out_data 输出序列化数据
 * @param out_len  输出数据长度
 */
void db_serialize_response(int32_t retval, const db_result_t *result, void **out_data, uint32_t *out_len);

/**
 * @brief 反序列化响应
 * @param data    序列化数据
 * @param len     数据长度
 * @param retval  输出返回值
 * @param result  输出结果集（可为 NULL，调用方负责 db_result_free）
 * @return 0 成功，-1 失败
 */
int db_deserialize_response(const void *data, uint32_t len, int32_t *retval, db_result_t **result);

#endif // DB_SERIALIZE_H
