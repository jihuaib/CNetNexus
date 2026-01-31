/**
 * @file   nn_config_template.c
 * @brief  配置模板处理实现
 * @author jhb
 * @date   2026/01/31
 */
#include "nn_config_template.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 全局注册表
// ============================================================================

static GHashTable *g_template_registry = NULL;

// ============================================================================
// 模板操作实现
// ============================================================================

nn_config_template_t *nn_config_template_create(const char *template_name, uint32_t priority)
{
    nn_config_template_t *tmpl = g_malloc0(sizeof(nn_config_template_t));
    if (!tmpl)
        return NULL;

    tmpl->template_name = g_strdup(template_name);
    tmpl->priority = priority;
    tmpl->child_template_names = NULL;
    tmpl->num_children = 0;
    tmpl->body = NULL;

    return tmpl;
}

void nn_config_template_add_child(nn_config_template_t *template, const char *child_name)
{
    if (!template || !child_name)
        return;

    // 扩展子模板列表
    template->child_template_names = g_realloc(template->child_template_names,
                                               (template->num_children + 1) * sizeof(char *));
    template->child_template_names[template->num_children] = g_strdup(child_name);
    template->num_children++;
}

void nn_config_template_set_body(nn_config_template_t *template, const char *content,
                                 const char **db_names, uint32_t num_dbs)
{
    if (!template)
        return;

    // 释放旧的 body
    if (template->body)
    {
        if (template->body->content)
            g_free(template->body->content);
        if (template->body->db_names)
        {
            for (uint32_t i = 0; i < template->body->num_dbs; i++)
                if (template->body->db_names[i])
                    g_free(template->body->db_names[i]);
            g_free(template->body->db_names);
        }
        g_free(template->body);
    }

    // 创建新 body
    template->body = g_malloc0(sizeof(nn_config_template_body_t));
    if (content)
        template->body->content = g_strdup(content);

    if (num_dbs > 0 && db_names)
    {
        template->body->db_names = g_malloc(num_dbs * sizeof(char *));
        for (uint32_t i = 0; i < num_dbs; i++)
            template->body->db_names[i] = g_strdup(db_names[i]);
        template->body->num_dbs = num_dbs;
    }
}

void nn_config_template_free(nn_config_template_t *template)
{
    if (!template)
        return;

    if (template->template_name)
        g_free(template->template_name);

    if (template->child_template_names)
    {
        for (uint32_t i = 0; i < template->num_children; i++)
            if (template->child_template_names[i])
                g_free(template->child_template_names[i]);
        g_free(template->child_template_names);
    }

    if (template->body)
    {
        if (template->body->content)
            g_free(template->body->content);
        if (template->body->db_names)
        {
            for (uint32_t i = 0; i < template->body->num_dbs; i++)
                if (template->body->db_names[i])
                    g_free(template->body->db_names[i]);
            g_free(template->body->db_names);
        }
        g_free(template->body);
    }

    g_free(template);
}

// ============================================================================
// 注册表操作
// ============================================================================

void nn_config_template_registry_add(nn_config_template_t *template)
{
    if (!template || !template->template_name)
        return;

    if (!g_template_registry)
        g_template_registry = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                                     (GDestroyNotify)nn_config_template_free);

    g_hash_table_insert(g_template_registry, template->template_name, template);
}

nn_config_template_t *nn_config_template_find_by_name(const char *template_name)
{
    if (!template_name || !g_template_registry)
        return NULL;

    return (nn_config_template_t *)g_hash_table_lookup(g_template_registry, template_name);
}

// 比较函数：用于按 priority 排序
static gint compare_templates_by_priority(gconstpointer a, gconstpointer b)
{
    const nn_config_template_t *ta = (const nn_config_template_t *)a;
    const nn_config_template_t *tb = (const nn_config_template_t *)b;
    return (int)tb->priority - (int)ta->priority;
}

GList *nn_config_template_get_all(void)
{
    if (!g_template_registry)
        return NULL;

    // 获取所有值并按 priority 排序
    GList *templates = g_hash_table_get_values(g_template_registry);

    // 按 priority 降序排列
    templates = g_list_sort(templates, compare_templates_by_priority);

    return templates;
}

void nn_config_template_registry_clear(void)
{
    if (g_template_registry)
    {
        g_hash_table_destroy(g_template_registry);
        g_template_registry = NULL;
    }
}

// ============================================================================
// 模板渲染
// ============================================================================

/**
 * @brief 解析模板中的变量引用（如 {table.field}）
 */
static GList *parse_template_variables(const char *content)
{
    GList *vars = NULL;

    if (!content)
        return NULL;

    // 找到所有 {xxx.yyy} 的模式
    const char *p = content;
    while ((p = strchr(p, '{')))
    {
        const char *end = strchr(p, '}');
        if (!end)
        {
            p++;
            continue;
        }

        // 提取变量名
        size_t len = end - p - 1;
        char *var_name = g_strndup(p + 1, len);
        vars = g_list_append(vars, var_name);

        p = end + 1;
    }

    return vars;
}

char *nn_config_template_render(nn_config_template_t *template, GHashTable *var_values)
{
    if (!template || !template->body || !template->body->content)
        return g_strdup("");

    GString *result = g_string_new(template->body->content);

    // 如果没有变量表，直接返回原文本
    if (!var_values)
        return g_string_free(result, FALSE);

    // 获取所有变量
    GList *vars = parse_template_variables(template->body->content);
    GList *iter = vars;

    while (iter)
    {
        const char *var_name = (const char *)iter->data;
        const char *var_value = (const char *)g_hash_table_lookup(var_values, var_name);

        if (var_value)
        {
            // 替换所有 {var_name} 为 var_value
            char pattern[512];
            snprintf(pattern, sizeof(pattern), "{%s}", var_name);
            char *temp = result->str;
            gchar **parts = g_strsplit(temp, pattern, -1);
            g_string_free(result, TRUE);
            result = g_string_new(g_strjoinv(var_value, parts));
            g_strfreev(parts);
        }

        iter = g_list_next(iter);
    }

    // 清理变量列表
    g_list_free_full(vars, g_free);

    // 将 \n 替换为 \r\n（telnet 协议需要 \r\n 才能正确回到行首）
    char *raw = g_string_free(result, FALSE);
    gchar **lines = g_strsplit(raw, "\n", -1);
    char *final = g_strjoinv("\r\n", lines);
    g_strfreev(lines);
    g_free(raw);

    return final;
}

