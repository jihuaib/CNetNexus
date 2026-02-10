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

// Request serialization
void db_serialize_request_insert(const char *db_name, const char *table_name, const char **field_names,
                                 const db_value_t *values, uint32_t num_fields, void **out_data, uint32_t *out_len);

void db_serialize_request_update(const char *db_name, const char *table_name, const char **field_names,
                                 const db_value_t *values, uint32_t num_fields, const char *where_clause,
                                 void **out_data, uint32_t *out_len);

void db_serialize_request_delete(const char *db_name, const char *table_name, const char *where_clause, void **out_data,
                                 uint32_t *out_len);

void db_serialize_request_query(const char *db_name, const char *table_name, const char **field_names,
                                uint32_t num_fields, const char *where_clause, void **out_data, uint32_t *out_len);

void db_serialize_request_exists(const char *db_name, const char *table_name, const char *where_clause, void **out_data,
                                 uint32_t *out_len);

// Response serialization
void db_serialize_response(int32_t retval, const db_result_t *result, void **out_data, uint32_t *out_len);

// Request deserialization (outputs allocated, caller must free)
int db_deserialize_request_insert(const void *data, uint32_t len, char **db_name, char **table_name,
                                  char ***field_names, db_value_t **values, uint32_t *num_fields);

int db_deserialize_request_update(const void *data, uint32_t len, char **db_name, char **table_name,
                                  char ***field_names, db_value_t **values, uint32_t *num_fields, char **where_clause);

int db_deserialize_request_delete(const void *data, uint32_t len, char **db_name, char **table_name,
                                  char **where_clause);

int db_deserialize_request_query(const void *data, uint32_t len, char **db_name, char **table_name, char ***field_names,
                                 uint32_t *num_fields, char **where_clause);

int db_deserialize_request_exists(const void *data, uint32_t len, char **db_name, char **table_name,
                                  char **where_clause);

// Response deserialization
int db_deserialize_response(const void *data, uint32_t len, int32_t *retval, db_result_t **result);

#endif // DB_SERIALIZE_H
