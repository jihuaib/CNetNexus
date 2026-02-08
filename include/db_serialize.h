/**
 * @file   db_serialize.h
 * @brief  数据库操作参数序列化/反序列化头文件
 * @author jhb
 * @date   2026/02/02
 */

#ifndef DB_SERIALIZE_H
#define DB_SERIALIZE_H

#include <glib.h>
#include <stdint.h>

#include "db.h"

/**
 * @brief 序列化 DB INSERT 操作参数
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param field_names 字段名称数组
 * @param values 值数组
 * @param num_fields 字段数量
 * @param out_data 输出序列化数据（调用者 g_free）
 * @param out_len 输出数据长度
 * @return 成功返回 0，失败返回 -1
 */
int db_serialize_insert(const char *db_name, const char *table_name, const char **field_names,
                            const db_value_t *values, uint32_t num_fields, void **out_data, uint32_t *out_len);

/**
 * @brief 序列化 DB UPDATE 操作参数
 */
int db_serialize_update(const char *db_name, const char *table_name, const char **field_names,
                            const db_value_t *values, uint32_t num_fields, const char *where_clause, void **out_data,
                            uint32_t *out_len);

/**
 * @brief 序列化 DB DELETE 操作参数
 */
int db_serialize_delete(const char *db_name, const char *table_name, const char *where_clause, void **out_data,
                            uint32_t *out_len);

/**
 * @brief 序列化 DB QUERY 操作参数
 */
int db_serialize_query(const char *db_name, const char *table_name, const char **field_names, uint32_t num_fields,
                           const char *where_clause, void **out_data, uint32_t *out_len);

/**
 * @brief 序列化 DB EXISTS 操作参数
 */
int db_serialize_exists(const char *db_name, const char *table_name, const char *where_clause, void **out_data,
                            uint32_t *out_len);

/**
 * @brief 反序列化 DB 响应（含返回值和结果集）
 * @param data 响应数据
 * @param len 数据长度
 * @param out_retval 输出返回值
 * @param out_result 输出结果集（可为 NULL）
 * @return 成功返回 0，失败返回 -1
 */
int db_deserialize_response(const void *data, uint32_t len, int32_t *out_retval, db_result_t **out_result);

/**
 * @brief 序列化 DB 响应（含返回值和结果集）
 * @param retval 返回值
 * @param result 结果集（可为 NULL）
 * @param out_data 输出序列化数据（调用者 g_free）
 * @param out_len 输出数据长度
 * @return 成功返回 0，失败返回 -1
 */
int db_serialize_response(int32_t retval, const db_result_t *result, void **out_data, uint32_t *out_len);

#endif // DB_SERIALIZE_H
