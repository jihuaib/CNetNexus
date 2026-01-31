/**
 * @file   nn_cfg_template_renderer.c
 * @brief  配置模板渲染器实现
 * @author jhb
 * @date   2026/01/31
 */
#include "nn_cfg_template_renderer.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_config_template.h"
#include "nn_db.h"
#include "nn_errcode.h"

// ============================================================================
// 内部辅助函数
// ============================================================================

/**
 * @brief 检查模板是否有可用的数据
 * @return TRUE 如果有数据，FALSE 如果无数据
 */
static gboolean template_has_data(nn_config_template_t *template)
{
    if (!template || !template->body || template->body->num_dbs == 0)
        return FALSE;

    // 对于模板定义中的每个数据库表
    for (uint32_t i = 0; i < template->body->num_dbs; i++)
    {
        const char *table_ref = template->body->db_names[i];

        // 解析 "db_name.table_name" 格式
        gchar **parts = g_strsplit(table_ref, ".", 2);
        if (!parts || !parts[0] || !parts[1])
        {
            g_strfreev(parts);
            continue;
        }

        const char *db_name = parts[0];
        const char *table_name = parts[1];

        // 检查是否存在数据
        gboolean exists = FALSE;
        int ret = nn_db_exists(db_name, table_name, NULL, &exists);

        g_strfreev(parts);

        if (ret == NN_ERRCODE_SUCCESS && exists)
        {
            printf("[cfg_renderer] Template '%s' has data\n", template->template_name);
            return TRUE;
        }
    }

    printf("[cfg_renderer] Template '%s' has no data\n", template->template_name);
    return FALSE;
}

/**
 * @brief 根据模板定义查询对应的数据库表
 * @param template 模板定义
 * @return GHashTable，key=table_name，value=nn_db_result_t*（需要调用者释放结果）
 */
static GHashTable *query_template_databases(nn_config_template_t *template)
{
    GHashTable *query_results = g_hash_table_new(g_str_hash, g_str_equal);

    if (!template || !template->body || template->body->num_dbs == 0)
        return query_results;

    // 对于模板定义中的每个数据库表
    for (uint32_t i = 0; i < template->body->num_dbs; i++)
    {
        const char *table_ref = template->body->db_names[i];

        // 解析 "db_name.table_name" 格式
        gchar **parts = g_strsplit(table_ref, ".", 2);
        if (!parts || !parts[0])
        {
            g_strfreev(parts);
            continue;
        }

        const char *db_name = NULL;
        const char *table_name = NULL;

        // 检查是否有"."分隔符
        if (parts[1])
        {
            // 格式: db_name.table_name
            db_name = parts[0];
            table_name = parts[1];
        }
        else
        {
            // 格式: table_name（不推荐，但兼容）
            printf("[cfg_renderer] Warning: table reference '%s' should use 'db_name.table_name' format\n", table_ref);
            g_strfreev(parts);
            continue;
        }

        // 查询数据库
        nn_db_result_t *result = NULL;
        int ret = nn_db_query(db_name, table_name, NULL, 0, NULL, &result);

        if (ret == NN_ERRCODE_SUCCESS && result)
        {
            printf("[cfg_renderer] Queried %s.%s: %u rows\n", db_name, table_name, result->num_rows);
            // 使用 db_name.table_name 作为key，而不仅仅是table_name
            char full_key[256];
            snprintf(full_key, sizeof(full_key), "%s.%s", db_name, table_name);
            g_hash_table_insert(query_results, g_strdup(full_key), result);

            // 同时也使用table_name作为key，便于兼容
            g_hash_table_insert(query_results, g_strdup(table_name), result);
        }
        else
        {
            printf("[cfg_renderer] Failed to query %s.%s\n", db_name, table_name);
        }

        g_strfreev(parts);
    }

    return query_results;
}

/**
 * @brief 从查询结果构建变量映射表
 * @param template 模板定义
 * @param query_results 查询结果（GHashTable，key=table_name 或 db_name.table_name，value=nn_db_result_t*）
 * @return 变量映射表（GHashTable，key=variable_name，value=variable_value_string）
 */
static GHashTable *build_var_map(nn_config_template_t *template, GHashTable *query_results)
{
    GHashTable *var_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    if (!template || !template->body || !query_results)
        return var_map;

    // 遍历数据库表
    for (uint32_t i = 0; i < template->body->num_dbs; i++)
    {
        const char *table_ref = template->body->db_names[i];

        // 解析 "db_name.table_name" 格式
        gchar **parts = g_strsplit(table_ref, ".", 2);
        if (!parts || !parts[0])
        {
            g_strfreev(parts);
            continue;
        }

        const char *db_name = NULL;
        const char *table_name = NULL;

        if (parts[1])
        {
            db_name = parts[0];
            table_name = parts[1];
        }
        else
        {
            // 不推荐的格式
            g_strfreev(parts);
            continue;
        }

        // 查找查询结果（先尝试 "db_name.table_name" key，再尝试 "table_name" key）
        nn_db_result_t *result = NULL;
        char lookup_key[256];
        snprintf(lookup_key, sizeof(lookup_key), "%s.%s", db_name, table_name);
        result = (nn_db_result_t *)g_hash_table_lookup(query_results, lookup_key);
        if (!result)
        {
            result = (nn_db_result_t *)g_hash_table_lookup(query_results, table_name);
        }

        if (!result || result->num_rows == 0)
        {
            printf("[cfg_renderer]   No data for table %s.%s\n", db_name, table_name);
            g_strfreev(parts);
            continue;
        }

        // 提取第一行的所有字段值
        nn_db_row_t *row = result->rows[0];
        for (uint32_t j = 0; j < row->num_fields; j++)
        {
            const char *field_name = row->field_names[j];
            char value_str[1024];
            value_str[0] = '\0';

            // 根据类型转换值为字符串
            switch (row->values[j].type)
            {
                case NN_DB_TYPE_INTEGER:
                    snprintf(value_str, sizeof(value_str), "%ld", row->values[j].data.i64);
                    break;
                case NN_DB_TYPE_REAL:
                    snprintf(value_str, sizeof(value_str), "%f", row->values[j].data.real);
                    break;
                case NN_DB_TYPE_TEXT:
                    if (row->values[j].data.text)
                        snprintf(value_str, sizeof(value_str), "%s", row->values[j].data.text);
                    break;
                case NN_DB_TYPE_BLOB:
                case NN_DB_TYPE_NULL:
                default:
                    break;
            }

            // 创建变量名：table_name.field_name
            char var_name[256];
            snprintf(var_name, sizeof(var_name), "%s.%s", table_name, field_name);
            g_hash_table_insert(var_map, g_strdup(var_name), g_strdup(value_str));

            printf("[cfg_renderer]   Variable: %s = %s\n", var_name, value_str);
        }

        g_strfreev(parts);
    }

    return var_map;
}

/**
 * @brief 释放查询结果哈希表（包含所有 nn_db_result_t）
 *
 * 注意：由于同一个result可能被多个key引用（"db.table" 和 "table"），
 * 我们需要追踪已释放的result以避免重复释放
 */
static void free_query_results(GHashTable *query_results)
{
    if (!query_results)
        return;

    // 收集所有唯一的result指针进行释放
    GHashTable *freed_results = g_hash_table_new(g_direct_hash, g_direct_equal);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, query_results);

    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        nn_db_result_t *result = (nn_db_result_t *)value;
        if (result && !g_hash_table_lookup(freed_results, result))
        {
            nn_db_result_free(result);
            g_hash_table_insert(freed_results, result, GINT_TO_POINTER(1));
        }
    }

    g_hash_table_destroy(freed_results);
    g_hash_table_destroy(query_results);
}

/**
 * @brief 递归渲染模板及其子模板
 */
static char *render_template_recursive(nn_config_template_t *template, GString *output)
{
    if (!template)
        return NULL;

    printf("[cfg_renderer] Rendering template: %s\n", template->template_name);

    // 检查模板是否有数据，如果没有则跳过
    if (!template_has_data(template))
    {
        printf("[cfg_renderer] Template '%s' has no data, skipping\n", template->template_name);
        return NULL;
    }

    // 查询数据库
    GHashTable *query_results = query_template_databases(template);

    // 构建变量映射表
    GHashTable *var_map = build_var_map(template, query_results);

    // 如果模板有主体内容，渲染它
    if (template->body && template->body->content)
    {
        char *rendered = nn_config_template_render(template, var_map);
        if (rendered)
        {
            g_string_append(output, rendered);
            g_string_append(output, "\r\n");
            g_free(rendered);
        }
    }

    // 递归渲染子模板
    if (template->num_children > 0)
    {
        printf("[cfg_renderer]   Template has %u children\n", template->num_children);
        for (uint32_t i = 0; i < template->num_children; i++)
        {
            const char *child_name = template->child_template_names[i];
            nn_config_template_t *child = nn_config_template_find_by_name(child_name);
            if (child)
            {
                render_template_recursive(child, output);
            }
        }
    }

    // 清理
    g_hash_table_destroy(var_map);
    free_query_results(query_results);

    return NULL;
}

// ============================================================================
// 公共 API 实现
// ============================================================================

char *nn_cfg_template_renderer_render_all(void)
{
    GString *output = g_string_new("");

    // 获取所有已注册的模板
    GList *templates = nn_config_template_get_all();

    if (!templates)
    {
        printf("[cfg_renderer] No templates registered\n");
        return g_string_free(output, FALSE);
    }

    // 渲染所有顶级模板（只有在优先级内才认为是顶级）
    // 按优先级排序后渲染（优先级较高的先渲染）
    GList *iter = templates;
    while (iter)
    {
        nn_config_template_t *template = (nn_config_template_t *)iter->data;

        // 只渲染有 priority 的模板（认为是顶级）
        if (template->priority > 0)
        {
            render_template_recursive(template, output);
            g_string_append(output, "\n");
        }

        iter = g_list_next(iter);
    }

    g_list_free(templates);

    return g_string_free(output, FALSE);
}

char *nn_cfg_template_renderer_render_by_name(const char *template_name)
{
    if (!template_name)
        return NULL;

    nn_config_template_t *template = nn_config_template_find_by_name(template_name);
    if (!template)
        return NULL;

    GString *output = g_string_new("");
    render_template_recursive(template, output);

    return g_string_free(output, FALSE);
}
