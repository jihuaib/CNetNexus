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
 * @brief 通过 RPC 按结构化定义建表并自动补齐缺失列
 * @param ctx 调用方模块的 IPC 上下文
 * @param def 表定义（表名、列列表及约束）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_create_table_from_def(dev_ipc_context_t *ctx, const db_table_def_t *def);

// ============================================================================
// db_record_t — 写操作键值构建器
// ============================================================================

/** 写操作记录构建器（不透明类型，通过 db_record_* 函数操作） */
typedef struct db_record db_record_t;

/**
 * @brief 创建一个空的记录构建器
 * @return 新分配的 db_record_t，须通过 db_record_free 释放
 */
db_record_t *db_record_new(void);

/**
 * @brief 释放记录构建器及其所有内部资源
 * @param rec 待释放的记录
 */
void db_record_free(db_record_t *rec);

/**
 * @brief 向记录中设置整数字段
 * @param rec   目标记录
 * @param field 字段名
 * @param value 整数值
 */
void db_record_set_int(db_record_t *rec, const char *field, int64_t value);

/**
 * @brief 向记录中设置文本字段（内部复制字符串）
 * @param rec   目标记录
 * @param field 字段名
 * @param value 文本值
 */
void db_record_set_text(db_record_t *rec, const char *field, const char *value);

/**
 * @brief 向记录中设置浮点字段
 * @param rec   目标记录
 * @param field 字段名
 * @param value 浮点值
 */
void db_record_set_real(db_record_t *rec, const char *field, double value);

// ============================================================================
// db_row_t 读取辅助（按字段名查找）
// ============================================================================

/**
 * @brief 按字段名从结果行中读取整数值
 * @param row         结果行
 * @param field       字段名
 * @param default_val 字段不存在或类型不匹配时返回的默认值
 * @return 字段整数值或 default_val
 */
int64_t db_row_get_int(const db_row_t *row, const char *field, int64_t default_val);

/**
 * @brief 按字段名从结果行中读取文本值
 * @param row         结果行
 * @param field       字段名
 * @param default_val 字段不存在或类型不匹配时返回的默认值
 * @return 字段文本指针（生命周期与 row 绑定）或 default_val
 */
const char *db_row_get_text(const db_row_t *row, const char *field, const char *default_val);

/**
 * @brief 按字段名从结果行中读取浮点值
 * @param row         结果行
 * @param field       字段名
 * @param default_val 字段不存在或类型不匹配时返回的默认值
 * @return 字段浮点值或 default_val
 */
double db_row_get_real(const db_row_t *row, const char *field, double default_val);

// ============================================================================
// 基于 db_record_t 的新 RPC 函数
// ============================================================================

/**
 * @brief 通过 RPC 向数据库表中插入一条记录
 * @param ctx   调用方模块的 IPC 上下文
 * @param table 表名
 * @param rec   记录构建器
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_insert_record(dev_ipc_context_t *ctx, const char *table, const db_record_t *rec);

/**
 * @brief 通过 RPC 更新数据库中符合条件的记录
 * @param ctx    调用方模块的 IPC 上下文
 * @param table  表名
 * @param rec    记录构建器（待更新的字段和值）
 * @param filter 过滤条件
 * @return 更新行数，错误返回 -1
 */
int db_rpc_update_record(dev_ipc_context_t *ctx, const char *table, const db_record_t *rec, const db_filter_t *filter);

/**
 * @brief 通过 RPC 执行 upsert：存在则 update，不存在则 insert
 * @param ctx    调用方模块的 IPC 上下文
 * @param table  表名
 * @param rec    记录构建器
 * @param filter 用于检测是否存在及 update 的过滤条件
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_upsert(dev_ipc_context_t *ctx, const char *table, const db_record_t *rec, const db_filter_t *filter);

#endif // DB_H
