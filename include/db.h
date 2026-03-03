/**
 * @file   db.h
 * @brief  数据库模块公共接口，提供数据库定义、CRUD 操作及类型验证 API
 * @author jhb
 * @date   2026/01/22
 */

#ifndef DB_H
#define DB_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// 字段值类型
// ============================================================================

/** 数据库值类型枚举 */
typedef enum db_value_type
{
    DB_TYPE_NULL,    /**< 空值 */
    DB_TYPE_INTEGER, /**< 整数类型 */
    DB_TYPE_REAL,    /**< 浮点数类型 */
    DB_TYPE_TEXT,    /**< 文本类型 */
    DB_TYPE_BLOB     /**< 二进制类型 */
} db_value_type_t;

/** 数据库值容器 */
typedef struct db_value
{
    db_value_type_t type; /**< 值类型 */
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
} db_value_t;

// ============================================================================
// 条件过滤类型
// ============================================================================

/** 条件比较操作符 */
typedef enum db_compare_op
{
    DB_CMP_EQ,   /**< 等于 (=) */
    DB_CMP_NE,   /**< 不等于 (!=) */
    DB_CMP_GT,   /**< 大于 (>) */
    DB_CMP_GTE,  /**< 大于等于 (>=) */
    DB_CMP_LT,   /**< 小于 (<) */
    DB_CMP_LTE,  /**< 小于等于 (<=) */
    DB_CMP_LIKE, /**< LIKE 匹配 */
} db_compare_op_t;

/** 单个查询条件（field op value） */
typedef struct db_condition
{
    const char *field_name; /**< 字段名 */
    db_compare_op_t op;     /**< 比较操作符 */
    db_value_t value;       /**< 比较值 */
} db_condition_t;

/** 过滤条件集合（当前为 AND 关系） */
typedef struct db_filter
{
    const db_condition_t *conditions; /**< 条件数组 */
    uint32_t num_conditions;          /**< 条件数量 */
} db_filter_t;

// ============================================================================
// 行/结果类型
// ============================================================================

/** 查询结果行 */
typedef struct db_row
{
    char **field_names;  /**< 字段名称数组 */
    db_value_t *values;  /**< 值数组 */
    uint32_t num_fields; /**< 字段数量 */
} db_row_t;

/** 查询结果集 */
typedef struct db_result
{
    db_row_t **rows;        /**< 行数组 */
    uint32_t num_rows;      /**< 行数 */
    uint32_t rows_capacity; /**< 已分配容量 */
} db_result_t;

// ============================================================================
// 建表定义
// ============================================================================

/** 列约束标志位 */
typedef enum db_column_constraint
{
    DB_COL_NONE = 0,                 /**< 无约束 */
    DB_COL_PRIMARY_KEY = (1 << 0),   /**< 主键 */
    DB_COL_NOT_NULL = (1 << 1),      /**< 非空 */
    DB_COL_UNIQUE = (1 << 2),        /**< 唯一 */
    DB_COL_AUTOINCREMENT = (1 << 3), /**< 自增（仅 INTEGER PRIMARY KEY 有效） */
} db_column_constraint_t;

/** 单列定义 */
typedef struct db_column_def
{
    const char *name;        /**< 列名 */
    db_value_type_t type;    /**< 数据类型 */
    uint32_t constraints;    /**< 约束标志位（db_column_constraint_t 的位组合） */
    const char *default_val; /**< 默认值表达式（SQL 字面量），NULL 表示无默认值 */
} db_column_def_t;

/** 建表定义（结构化描述，替代裸 DDL 字符串） */
typedef struct db_table_def
{
    const char *table_name;      /**< 表名 */
    const db_column_def_t *cols; /**< 列定义数组 */
    uint32_t num_cols;           /**< 列数量 */
} db_table_def_t;

// ============================================================================
// 内存管理
// ============================================================================

/**
 * @brief 释放查询结果
 * @param result 待释放的结果集
 */
void db_result_free(db_result_t *result);

/**
 * @brief 创建整数类型的值
 * @param value 整数值
 * @return 数据库值结构
 */
db_value_t db_value_int(int64_t value);

/**
 * @brief 创建字符串类型的值（会复制字符串）
 * @param value 字符串值
 * @return 数据库值结构
 */
db_value_t db_value_text(const char *value);

/**
 * @brief 创建浮点数类型的值
 * @param value 浮点数值
 * @return 数据库值结构
 */
db_value_t db_value_real(double value);

/**
 * @brief 创建 NULL 值
 * @return 数据库值结构
 */
db_value_t db_value_null(void);

/**
 * @brief 释放值（释放已分配的文本内存）
 * @param value 待释放的值
 */
void db_value_free(db_value_t *value);

// ============================================================================
// 前向声明
// ============================================================================

typedef struct dev_ipc_context dev_ipc_context_t;

// ============================================================================
// 数据库 RPC 接口（通过 IPC 调用 DB 模块）
// ============================================================================

/**
 * @brief 通过 RPC 向数据库表中插入一行数据
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param field_names 字段名称数组
 * @param values     值数组（长度须与 field_names 一致）
 * @param num_fields 字段数量
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_insert(dev_ipc_context_t *ctx, const char *table_name, const char **field_names, const db_value_t *values,
                  uint32_t num_fields);

/**
 * @brief 通过 RPC 更新数据库中符合条件的行
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param field_names 待更新的字段名称数组
 * @param values     新值数组
 * @param num_fields 待更新的字段数量
 * @param filter    结构化过滤条件（为 NULL 或空则更新所有行）
 * @return 更新的行数，错误返回 -1
 */
int db_rpc_update(dev_ipc_context_t *ctx, const char *table_name, const char **field_names, const db_value_t *values,
                  uint32_t num_fields, const db_filter_t *filter);

/**
 * @brief 通过 RPC 删除数据库中符合条件的行
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param filter    结构化过滤条件（为 NULL 或空则删除所有行）
 * @return 删除的行数，错误返回 -1
 */
int db_rpc_delete(dev_ipc_context_t *ctx, const char *table_name, const db_filter_t *filter);

/**
 * @brief 通过 RPC 查询数据库表中的行
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param field_names 待查询的字段名称数组（为 NULL 则查询所有字段）
 * @param num_fields 字段数量（为 0 则查询所有字段）
 * @param filter    结构化过滤条件（为 NULL 或空则查询所有行）
 * @param result     输出结果集（调用者须通过 db_result_free 释放）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_query(dev_ipc_context_t *ctx, const char *table_name, const char **field_names, uint32_t num_fields,
                 const db_filter_t *filter, db_result_t **result);

/**
 * @brief 通过 RPC 检查数据库中是否存在符合条件的行
 * @param ctx        调用方模块的 IPC 上下文
 * @param table_name 表名称
 * @param filter    结构化过滤条件（为 NULL 或空则检查表是否存在任意行）
 * @param exists     输出布尔值（存在则为 TRUE）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_exists(dev_ipc_context_t *ctx, const char *table_name, const db_filter_t *filter, gboolean *exists);

/**
 * @brief 通过 RPC 执行建表 DDL
 * @param ctx  调用方模块的 IPC 上下文
 * @param ddl  完整的 CREATE TABLE ... SQL 语句（通常含 IF NOT EXISTS）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_create_table(dev_ipc_context_t *ctx, const char *ddl);

/**
 * @brief 通过 RPC 按结构化定义建表并自动补齐缺失列
 * @param ctx 调用方模块的 IPC 上下文
 * @param def 表定义（表名、列列表及约束）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_create_table_from_def(dev_ipc_context_t *ctx, const db_table_def_t *def);

#endif // DB_H
