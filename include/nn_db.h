/**
 * @file   nn_db.h
 * @brief  数据库模块公共接口，提供数据库定义、CRUD 操作及类型验证 API
 * @author jhb
 * @date   2026/01/22
 */

#ifndef NN_DB_H
#define NN_DB_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

#include "nn_cfg.h"

typedef struct nn_db_field nn_db_field_t;
typedef struct nn_db_table nn_db_table_t;
typedef struct nn_db_definition nn_db_definition_t;

// ============================================================================
// 字段值类型
// ============================================================================

/** 数据库值类型枚举 */
typedef enum nn_db_value_type
{
    NN_DB_TYPE_NULL,    /**< 空值 */
    NN_DB_TYPE_INTEGER, /**< 整数类型 */
    NN_DB_TYPE_REAL,    /**< 浮点数类型 */
    NN_DB_TYPE_TEXT,    /**< 文本类型 */
    NN_DB_TYPE_BLOB     /**< 二进制类型 */
} nn_db_value_type_t;

/** 数据库值容器 */
typedef struct nn_db_value
{
    nn_db_value_type_t type; /**< 值类型 */
    union
    {
        int64_t i64; /**< 整数值 */
        double real; /**< 浮点数值 */
        char *text;  /**< 文本值（已分配，须释放） */
        struct
        {
            void *data; /**< BLOB 数据 */
            size_t len; /**< BLOB 长度 */
        } blob;
    } data;
} nn_db_value_t;

// ============================================================================
// 行/结果类型
// ============================================================================

/** 查询结果行 */
typedef struct nn_db_row
{
    char **field_names;  /**< 字段名称数组 */
    nn_db_value_t *values; /**< 值数组 */
    uint32_t num_fields; /**< 字段数量 */
} nn_db_row_t;

/** 查询结果集 */
typedef struct nn_db_result
{
    nn_db_row_t **rows;     /**< 行数组 */
    uint32_t num_rows;      /**< 行数 */
    uint32_t rows_capacity; /**< 已分配容量 */
} nn_db_result_t;

// ============================================================================
// 数据库定义 API
// ============================================================================

/**
 * @brief 创建字段定义
 * @param field_name 字段名称
 * @param type_str 类型字符串（如 "uint(1-4294967295)"）
 * @return 新分配的字段定义
 */
nn_db_field_t *nn_db_field_create(const char *field_name, const char *type_str);

/**
 * @brief 创建数据库定义
 * @param db_name 数据库名称
 * @param module_id 模块 ID
 * @return 新分配的数据库定义
 */
nn_db_definition_t *nn_db_definition_create(const char *db_name, uint32_t module_id);

/**
 * @brief 创建表定义
 * @param table_name 表名称
 * @return 新分配的表定义
 */
nn_db_table_t *nn_db_table_create(const char *table_name);

/**
 * @brief 向表中添加字段
 * @param table 目标表
 * @param field 待添加的字段（所有权转移）
 */
void nn_db_table_add_field(nn_db_table_t *table, nn_db_field_t *field);

/**
 * @brief 向数据库定义中添加表
 * @param db_def 数据库定义
 * @param table 待添加的表（所有权转移）
 */
void nn_db_definition_add_table(nn_db_definition_t *db_def, nn_db_table_t *table);

/**
 * @brief 将数据库定义添加到注册表
 * @param db_def 待添加的数据库定义（所有权转移）
 */
void nn_db_registry_add(nn_db_definition_t *db_def);

// ============================================================================
// 初始化 API（由 CFG 模块调用）
// ============================================================================

/**
 * @brief 根据已注册的定义初始化所有数据库
 *
 * 基于 XML 定义创建数据库文件和表，在所有 XML 文件加载完成后由 CFG 模块调用
 * @return NN_ERRCODE_SUCCESS 或 NN_ERRCODE_FAIL
 */
int nn_db_initialize_all(void);

// ============================================================================
// CRUD 操作
// ============================================================================

/**
 * @brief 向表中插入一行数据
 * @param db_name 数据库名称（如 "bgp_db"）
 * @param table_name 表名称（如 "bgp_protocol"）
 * @param field_names 字段名称数组
 * @param values 值数组（长度须与 field_names 一致）
 * @param num_fields 字段数量
 * @return NN_ERRCODE_SUCCESS 或 NN_ERRCODE_FAIL
 */
int nn_db_insert(const char *db_name, const char *table_name, const char **field_names, const nn_db_value_t *values,
                 uint32_t num_fields);

/**
 * @brief 更新符合条件的行
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param field_names 待更新的字段名称数组
 * @param values 新值数组
 * @param num_fields 待更新的字段数量
 * @param where_clause SQL WHERE 子句（如 "as_number = 65001"），为 NULL 则更新所有行
 * @return 更新的行数，错误返回 -1
 */
int nn_db_update(const char *db_name, const char *table_name, const char **field_names, const nn_db_value_t *values,
                 uint32_t num_fields, const char *where_clause);

/**
 * @brief 删除符合条件的行
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param where_clause SQL WHERE 子句，为 NULL 则删除所有行
 * @return 删除的行数，错误返回 -1
 */
int nn_db_delete(const char *db_name, const char *table_name, const char *where_clause);

/**
 * @brief 查询表中的行
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param field_names 待查询的字段名称数组（为 NULL 则查询所有字段 "*"）
 * @param num_fields 字段数量（为 0 则查询所有字段）
 * @param where_clause SQL WHERE 子句，为 NULL 则查询所有行
 * @param result 输出结果集（调用者须通过 nn_db_result_free 释放）
 * @return NN_ERRCODE_SUCCESS 或 NN_ERRCODE_FAIL
 */
int nn_db_query(const char *db_name, const char *table_name, const char **field_names, uint32_t num_fields,
                const char *where_clause, nn_db_result_t **result);

/**
 * @brief 检查是否存在符合条件的行
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param where_clause SQL WHERE 子句
 * @param exists 输出布尔值（存在则为 TRUE）
 * @return NN_ERRCODE_SUCCESS 或 NN_ERRCODE_FAIL
 */
int nn_db_exists(const char *db_name, const char *table_name, const char *where_clause, gboolean *exists);

// ============================================================================
// 内存管理
// ============================================================================

/**
 * @brief 释放查询结果
 * @param result 待释放的结果集
 */
void nn_db_result_free(nn_db_result_t *result);

/**
 * @brief 创建整数类型的值
 * @param value 整数值
 * @return 数据库值结构
 */
nn_db_value_t nn_db_value_int(int64_t value);

/**
 * @brief 创建字符串类型的值（会复制字符串）
 * @param value 字符串值
 * @return 数据库值结构
 */
nn_db_value_t nn_db_value_text(const char *value);

/**
 * @brief 创建浮点数类型的值
 * @param value 浮点数值
 * @return 数据库值结构
 */
nn_db_value_t nn_db_value_real(double value);

/**
 * @brief 创建 NULL 值
 * @return 数据库值结构
 */
nn_db_value_t nn_db_value_null(void);

/**
 * @brief 释放值（释放已分配的文本内存）
 * @param value 待释放的值
 */
void nn_db_value_free(nn_db_value_t *value);

// ============================================================================
// 类型验证（基于 XML 类型定义）
// ============================================================================

/**
 * @brief 根据 XML 中字段的类型定义验证值的有效性
 * @param db_name 数据库名称
 * @param table_name 表名称
 * @param field_name 字段名称
 * @param value 待验证的值
 * @param error_msg 错误信息输出缓冲区（可选）
 * @param error_msg_len 错误信息缓冲区长度
 * @return 有效返回 TRUE，无效返回 FALSE
 */
gboolean nn_db_validate_field(const char *db_name, const char *table_name, const char *field_name,
                              const nn_db_value_t *value, char *error_msg, uint32_t error_msg_len);

#endif // NN_DB_H
