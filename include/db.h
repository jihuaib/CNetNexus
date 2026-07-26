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
typedef struct dev_ipc_message dev_ipc_message_t;

// ============================================================================
// DB 可用性检查 / 配置 Guard
// ----------------------------------------------------------------------------
// 目的：业务模块在 CLI 配置入口先做一次 DB 可用性检查。DB 不在线时直接回错，
// 不进入内存/OS 修改，避免"内存改了 / DB 写失败"的静默偏移。
// ============================================================================

/**
 * @brief 检查 DB 模块当前是否可用（IPC 已建联）
 *
 * 仅做本地 O(1) 连接状态检查，不发送任何网络请求。
 *
 * @param ctx 调用方模块的 IPC 上下文
 * @return 1 表示 DB 可用（已建联），0 表示不可用
 */
int db_rpc_is_available(dev_ipc_context_t *ctx);

/**
 * @brief 配置入口 Guard：DB 不可用时直接回错，调用方应立即丢弃 msg 并 return
 *
 * @param ctx        调用方模块的 IPC 上下文（查询连接状态 + 发响应）
 * @param cli_msg    原始 CLI 命令消息（用于路由响应）
 * @param module_tag 业务模块简短标签（如 "VRF"/"BGP"），仅用于错误文案
 * @return 1 = DB 不可用、已回错；0 = DB 可用，继续正常处理
 */
int db_rpc_guard_reject(dev_ipc_context_t *ctx, dev_ipc_message_t *cli_msg, const char *module_tag);

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

/**
 * @brief 创建父表删除后的单子表级联删除触发器
 *
 * 所有名称都必须是合法 SQL 标识符。触发器使用
 * CREATE TRIGGER IF NOT EXISTS 创建，父表 DELETE 与子表 DELETE
 * 由 SQLite 作为同一条语句原子执行。
 *
 * @param ctx           调用方模块的 IPC 上下文
 * @param trigger_name  触发器名称
 * @param parent_table  父表名称
 * @param parent_column 父表关联列
 * @param child_table   子表名称
 * @param child_column  子表关联列
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int db_rpc_create_delete_cascade(dev_ipc_context_t *ctx, const char *trigger_name, const char *parent_table,
                                 const char *parent_column, const char *child_table, const char *child_column);

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

// ============================================================================
// 通用便捷 API：以 (列名,值) 数组直接执行写操作，免去 db_record_t 模板代码
// ============================================================================

/**
 * @brief 列名 + 值的轻量封装，调用方在栈上构造数组传入
 *
 * 用法示例：
 *   db_col_t cols[] = {
 *       { "open_caps",        db_value_int((int64_t)caps) },
 *       { "source_interface", db_value_text(if_name) },
 *   };
 *   db_rpc_update_cols(ctx, "bgp_session", &pk.filter, cols, G_N_ELEMENTS(cols));
 *
 * 内部会按 type 分发到 db_record_set_int/text/real，文本字段值由内部复制；
 * 调用方传入的 db_value_t 不会被本接口接管，调用方自行决定是否需要 db_value_free
 * （直接通过 db_value_int/real 构造的 INTEGER/REAL 值无堆内存，无需释放）。
 */
typedef struct db_col
{
    const char *name; /**< 列名（指向调用方持有的字符串字面量或长寿命对象） */
    db_value_t value; /**< 列值（TEXT 用借用指针：内部 db_record_set_text 会 g_strdup 复制，
                       *  调用方应通过 DB_COL_TEXT 宏构造以避免双重分配） */
} db_col_t;

/** 栈上构造整数列（无需释放） */
#define DB_COL_INT(field_name, v)                                                                                      \
    {                                                                                                                  \
        (field_name),                                                                                                  \
        {                                                                                                              \
            .type = DB_TYPE_INTEGER, .data.i64 = (int64_t)(v)                                                          \
        }                                                                                                              \
    }

/** 栈上构造文本列（借用字符串指针，不持有所有权；helper 内部会复制） */
#define DB_COL_TEXT(field_name, s)                                                                                     \
    {                                                                                                                  \
        (field_name),                                                                                                  \
        {                                                                                                              \
            .type = DB_TYPE_TEXT, .data.text = (char *)(s)                                                             \
        }                                                                                                              \
    }

/** 栈上构造浮点列（无需释放） */
#define DB_COL_REAL(field_name, v)                                                                                     \
    {                                                                                                                  \
        (field_name),                                                                                                  \
        {                                                                                                              \
            .type = DB_TYPE_REAL, .data.real = (double)(v)                                                             \
        }                                                                                                              \
    }

/** PK / WHERE 条件构造器，封装 db_condition_t 数组 + 内部 db_value_t 释放 */
typedef struct db_filter_builder
{
    db_condition_t conds[8]; /**< 实际表 PK 最多 3-4 列，8 已足够 */
    uint32_t n;              /**< 已添加条件数 */
    db_filter_t filter;      /**< 与 conds/n 同步维护，可直接传给 db_rpc_* */
} db_filter_builder_t;

/**
 * @brief 初始化 PK 构造器（清零 + 设置 filter.conditions 指针）
 */
void db_filter_init(db_filter_builder_t *b);

/**
 * @brief 追加一个整数等值条件
 */
void db_filter_add_int(db_filter_builder_t *b, const char *name, int64_t v);

/**
 * @brief 追加一个文本等值条件（内部复制字符串）
 */
void db_filter_add_text(db_filter_builder_t *b, const char *name, const char *v);

/**
 * @brief 释放构造器内部所有 db_value_t 占用（文本等）
 *
 * 调用后构造器即不再可用。栈分配的 builder 本身无需释放。
 */
void db_filter_clear(db_filter_builder_t *b);

/**
 * @brief 一行调用：用 (列名,值) 数组更新指定行
 * @param ctx     调用方 IPC 上下文
 * @param table   表名
 * @param where   过滤条件（不可为 NULL；为防止全表更新，必须显式传 PK）
 * @param cols    列数组
 * @param n_cols  列数
 * @return 更新行数，错误返回 -1
 */
int db_rpc_update_cols(dev_ipc_context_t *ctx, const char *table, const db_filter_t *where, const db_col_t *cols,
                       size_t n_cols);

/**
 * @brief 一行调用：用 (列名,值) 数组插入一行
 * @return ERRCODE_SUCCESS / ERRCODE_FAIL
 */
int db_rpc_insert_cols(dev_ipc_context_t *ctx, const char *table, const db_col_t *cols, size_t n_cols);

#endif // DB_H
