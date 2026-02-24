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

#endif // DB_H
