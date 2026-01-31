/**
 * @file   nn_config_template.h
 * @brief  配置模板定义和处理头文件
 * @author jhb
 * @date   2026/01/31
 */
#ifndef NN_CONFIG_TEMPLATE_H
#define NN_CONFIG_TEMPLATE_H

#include <glib.h>
#include <stdint.h>

// ============================================================================
// 模板数据结构
// ============================================================================

/**
 * @brief 模板主体（包含变量和文本内容）
 */
typedef struct nn_config_template_body
{
    char *content;          /**< 模板内容（包含 {table.field} 变量） */
    char **db_names;        /**< 数据库表名列表（如 "bgp_protocol", "bgp_session"） */
    uint32_t num_dbs;       /**< 数据库表数量 */
} nn_config_template_body_t;

/**
 * @brief 模板定义（template-def 元素）
 */
typedef struct nn_config_template_def
{
    char *template_name;    /**< 模板名称 */
    uint32_t priority;      /**< 优先级（在系统视图中的顺序） */
    char **child_names;     /**< 子模板名称列表 */
    uint32_t num_children;  /**< 子模板数量 */
} nn_config_template_def_t;

/**
 * @brief 完整的配置模板（包含定义和主体）
 */
typedef struct nn_config_template
{
    char *template_name;              /**< 模板名称 */
    uint32_t priority;                /**< 优先级 */
    char **child_template_names;      /**< 子模板名称列表 */
    uint32_t num_children;            /**< 子模板数量 */
    nn_config_template_body_t *body;  /**< 模板主体（可选） */
} nn_config_template_t;

// ============================================================================
// 模板操作 API
// ============================================================================

/**
 * @brief 创建模板定义
 * @param template_name 模板名称
 * @param priority 优先级
 * @return 新分配的模板定义
 */
nn_config_template_t *nn_config_template_create(const char *template_name, uint32_t priority);

/**
 * @brief 向模板添加子模板
 * @param template 父模板
 * @param child_name 子模板名称
 */
void nn_config_template_add_child(nn_config_template_t *template, const char *child_name);

/**
 * @brief 设置模板主体
 * @param template 目标模板
 * @param content 模板内容（会复制字符串）
 * @param db_names 数据库名称数组
 * @param num_dbs 数据库数量
 */
void nn_config_template_set_body(nn_config_template_t *template, const char *content,
                                 const char **db_names, uint32_t num_dbs);

/**
 * @brief 释放模板
 * @param template 待释放的模板
 */
void nn_config_template_free(nn_config_template_t *template);

// ============================================================================
// 模板注册和检索 API（全局使用）
// ============================================================================

/**
 * @brief 注册模板到全局注册表
 * @param template 待注册的模板（所有权转移）
 */
void nn_config_template_registry_add(nn_config_template_t *template);

/**
 * @brief 按名称查找已注册的模板
 * @param template_name 模板名称
 * @return 模板指针（不转移所有权），未找到返回 NULL
 */
nn_config_template_t *nn_config_template_find_by_name(const char *template_name);

/**
 * @brief 获取注册的所有模板列表
 * @return GList*（元素为 nn_config_template_t*），由调用者负责释放列表（不释放元素）
 */
GList *nn_config_template_get_all(void);

/**
 * @brief 清空模板注册表（仅在模块卸载时使用）
 */
void nn_config_template_registry_clear(void);

// ============================================================================
// 模板渲染 API
// ============================================================================

/**
 * @brief 模板变量替换结果
 */
typedef struct nn_template_var_value
{
    char *var_name;  /**< 变量名称（如 "bgp_protocol.as_number"） */
    char *value;     /**< 替换值 */
} nn_template_var_value_t;

/**
 * @brief 渲染模板主体
 * @param template 目标模板
 * @param var_values 变量替换表（variable_name -> value_string 映射，如 "bgp_protocol.as_number" -> "65000"）
 * @return 渲染后的字符串（调用者负责 g_free）
 */
char *nn_config_template_render(nn_config_template_t *template, GHashTable *var_values);

#endif // NN_CONFIG_TEMPLATE_H
