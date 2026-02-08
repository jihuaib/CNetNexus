/**
 * @file   config_template.c
 * @brief  配置模板处理实现
 * @author jhb
 * @date   2026/01/31
 */
#include "config_template.h"

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

config_template_t *config_template_create(const char *template_name, uint32_t priority)
{
    config_template_t *tmpl = g_malloc0(sizeof(config_template_t));
    if (!tmpl)
    {
        return NULL;
    }

    tmpl->template_name = g_strdup(template_name);
    tmpl->priority = priority;
    tmpl->child_template_names = NULL;
    tmpl->num_children = 0;
    tmpl->body = NULL;

    return tmpl;
}

void config_template_add_child(config_template_t *template, const char *child_name)
{
    if (!template || !child_name)
    {
        return;
    }

    // 扩展子模板列表
    template->child_template_names =
        g_realloc(template->child_template_names, (template->num_children + 1) * sizeof(char *));
    template->child_template_names[template->num_children] = g_strdup(child_name);
    template->num_children++;
}

void config_template_set_body(config_template_t *template, const char *content, const char **db_names,
                                 uint32_t num_dbs)
{
    if (!template)
    {
        return;
    }

    // 释放旧的 body
    if (template->body)
    {
        if (template->body->content)
        {
            g_free(template->body->content);
        }
        if (template->body->db_names)
        {
            for (uint32_t i = 0; i < template->body->num_dbs; i++)
            {
                if (template->body->db_names[i])
                {
                    g_free(template->body->db_names[i]);
                }
            }
            g_free(template->body->db_names);
        }
        g_free(template->body);
    }

    // 创建新 body
    template->body = g_malloc0(sizeof(config_template_body_t));
    if (content)
    {
        template->body->content = g_strdup(content);
    }

    if (num_dbs > 0 && db_names)
    {
        template->body->db_names = g_malloc(num_dbs * sizeof(char *));
        for (uint32_t i = 0; i < num_dbs; i++)
        {
            template->body->db_names[i] = g_strdup(db_names[i]);
        }
        template->body->num_dbs = num_dbs;
    }
}

void config_template_free(config_template_t *template)
{
    if (!template)
    {
        return;
    }

    if (template->template_name)
    {
        g_free(template->template_name);
    }

    if (template->child_template_names)
    {
        for (uint32_t i = 0; i < template->num_children; i++)
        {
            if (template->child_template_names[i])
            {
                g_free(template->child_template_names[i]);
            }
        }
        g_free(template->child_template_names);
    }

    if (template->body)
    {
        if (template->body->content)
        {
            g_free(template->body->content);
        }
        if (template->body->db_names)
        {
            for (uint32_t i = 0; i < template->body->num_dbs; i++)
            {
                if (template->body->db_names[i])
                {
                    g_free(template->body->db_names[i]);
                }
            }
            g_free(template->body->db_names);
        }
        g_free(template->body);
    }

    g_free(template);
}

// ============================================================================
// 注册表操作
// ============================================================================

void config_template_registry_add(config_template_t *template)
{
    if (!template || !template->template_name)
    {
        return;
    }

    if (!g_template_registry)
    {
        g_template_registry =
            g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)config_template_free);
    }

    g_hash_table_insert(g_template_registry, template->template_name, template);
}

config_template_t *config_template_find_by_name(const char *template_name)
{
    if (!template_name || !g_template_registry)
    {
        return NULL;
    }

    return (config_template_t *)g_hash_table_lookup(g_template_registry, template_name);
}

// 比较函数：用于按 priority 排序
static gint compare_templates_by_priority(gconstpointer a, gconstpointer b)
{
    const config_template_t *ta = (const config_template_t *)a;
    const config_template_t *tb = (const config_template_t *)b;
    return (int)tb->priority - (int)ta->priority;
}

GList *config_template_get_all(void)
{
    if (!g_template_registry)
    {
        return NULL;
    }

    // 获取所有值并按 priority 排序
    GList *templates = g_hash_table_get_values(g_template_registry);

    // 按 priority 降序排列
    templates = g_list_sort(templates, compare_templates_by_priority);

    return templates;
}

void config_template_registry_clear(void)
{
    if (g_template_registry)
    {
        g_hash_table_destroy(g_template_registry);
        g_template_registry = NULL;
    }
}

// ============================================================================
// 模板条件指令处理（{% if %} / {% elif %} / {% else %} / {% endif %}）
// ============================================================================

/**
 * @brief 条件栈帧，用于跟踪嵌套的 if/elif/else 块
 */
typedef struct
{
    gboolean condition_met;  /**< 当前分支条件是否满足 */
    gboolean any_branch_met; /**< 是否有任何分支已满足（用于 elif/else） */
    gboolean parent_active;  /**< 父级是否处于激活状态 */
} template_cond_frame_t;

/**
 * @brief 去除字符串首尾空白
 */
static char *trim_whitespace(const char *str)
{
    if (!str)
    {
        return NULL;
    }

    // 跳过前导空白
    while (*str == ' ' || *str == '\t')
    {
        str++;
    }

    if (*str == '\0')
    {
        return g_strdup("");
    }

    // 找到尾部非空白位置
    const char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t'))
    {
        end--;
    }

    return g_strndup(str, end - str + 1);
}

/**
 * @brief 查找不在引号内的运算符
 * @param expr 表达式字符串
 * @param op 要查找的运算符（如 "&&", "||"）
 * @return 运算符位置，未找到返回 NULL
 */
static const char *find_operator_outside_quotes(const char *expr, const char *op)
{
    size_t op_len = strlen(op);
    gboolean in_quotes = FALSE;

    for (const char *p = expr; *p; p++)
    {
        if (*p == '"')
        {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && strncmp(p, op, op_len) == 0)
        {
            return p;
        }
    }

    return NULL;
}

/**
 * @brief 求值条件表达式
 *
 * 支持的语法：
 * - 比较：var == value, var != value, var > 0, var < 10, var >= 5, var <= 20
 * - 逻辑：expr1 && expr2, expr1 || expr2, !expr
 * - 字符串比较：var == "text", var != "text"
 * - 空值检查：var（变量非空且非 "0" 为真）
 *
 * @param expr 条件表达式
 * @param var_values 变量映射表
 * @return TRUE 条件满足，FALSE 条件不满足
 */
static gboolean evaluate_condition(const char *expr, GHashTable *var_values)
{
    if (!expr)
    {
        return FALSE;
    }

    char *trimmed = trim_whitespace(expr);
    if (!trimmed || trimmed[0] == '\0')
    {
        g_free(trimmed);
        return FALSE;
    }

    // 处理逻辑或 ||（最低优先级）
    const char *or_pos = find_operator_outside_quotes(trimmed, "||");
    if (or_pos)
    {
        char *left = g_strndup(trimmed, or_pos - trimmed);
        const char *right = or_pos + 2;
        gboolean result = evaluate_condition(left, var_values) || evaluate_condition(right, var_values);
        g_free(left);
        g_free(trimmed);
        return result;
    }

    // 处理逻辑与 &&
    const char *and_pos = find_operator_outside_quotes(trimmed, "&&");
    if (and_pos)
    {
        char *left = g_strndup(trimmed, and_pos - trimmed);
        const char *right = and_pos + 2;
        gboolean result = evaluate_condition(left, var_values) && evaluate_condition(right, var_values);
        g_free(left);
        g_free(trimmed);
        return result;
    }

    // 处理逻辑非 !
    if (trimmed[0] == '!')
    {
        gboolean result = !evaluate_condition(trimmed + 1, var_values);
        g_free(trimmed);
        return result;
    }

    // 处理比较运算符（按长度从长到短检查：!=, >=, <=, ==, >, <）
    const char *operators[] = {"!=", ">=", "<=", "==", ">", "<"};
    int num_ops = 6;

    for (int i = 0; i < num_ops; i++)
    {
        const char *op_pos = find_operator_outside_quotes(trimmed, operators[i]);
        if (!op_pos)
        {
            continue;
        }

        size_t op_len = strlen(operators[i]);
        char *left_raw = g_strndup(trimmed, op_pos - trimmed);
        char *right_raw = g_strdup(op_pos + op_len);
        char *left_name = trim_whitespace(left_raw);
        char *right_str = trim_whitespace(right_raw);

        // 左侧：从变量表查找值
        const char *left_value = NULL;
        if (var_values)
        {
            left_value = (const char *)g_hash_table_lookup(var_values, left_name);
        }
        if (!left_value)
        {
            left_value = left_name; // 如果不是变量，当作字面量
        }

        // 右侧：去除引号（如果有）
        char *right_value = NULL;
        if (right_str[0] == '"' && right_str[strlen(right_str) - 1] == '"')
        {
            // 字符串字面量，去除引号
            right_value = g_strndup(right_str + 1, strlen(right_str) - 2);
        }
        else
        {
            // 尝试作为变量查找
            const char *rv = NULL;
            if (var_values)
            {
                rv = (const char *)g_hash_table_lookup(var_values, right_str);
            }
            right_value = g_strdup(rv ? rv : right_str);
        }

        gboolean result = FALSE;

        // 尝试数值比较
        char *left_end = NULL, *right_end = NULL;
        double left_num = strtod(left_value, &left_end);
        double right_num = strtod(right_value, &right_end);
        gboolean numeric = (left_end && *left_end == '\0' && right_end && *right_end == '\0');

        if (numeric)
        {
            if (strcmp(operators[i], "==") == 0)
            {
                result = (left_num == right_num);
            }
            else if (strcmp(operators[i], "!=") == 0)
            {
                result = (left_num != right_num);
            }
            else if (strcmp(operators[i], ">") == 0)
            {
                result = (left_num > right_num);
            }
            else if (strcmp(operators[i], "<") == 0)
            {
                result = (left_num < right_num);
            }
            else if (strcmp(operators[i], ">=") == 0)
            {
                result = (left_num >= right_num);
            }
            else if (strcmp(operators[i], "<=") == 0)
            {
                result = (left_num <= right_num);
            }
        }
        else
        {
            // 字符串比较
            int cmp = strcmp(left_value, right_value);
            if (strcmp(operators[i], "==") == 0)
            {
                result = (cmp == 0);
            }
            else if (strcmp(operators[i], "!=") == 0)
            {
                result = (cmp != 0);
            }
            else if (strcmp(operators[i], ">") == 0)
            {
                result = (cmp > 0);
            }
            else if (strcmp(operators[i], "<") == 0)
            {
                result = (cmp < 0);
            }
            else if (strcmp(operators[i], ">=") == 0)
            {
                result = (cmp >= 0);
            }
            else if (strcmp(operators[i], "<=") == 0)
            {
                result = (cmp <= 0);
            }
        }

        g_free(left_raw);
        g_free(right_raw);
        g_free(left_name);
        g_free(right_str);
        g_free(right_value);
        g_free(trimmed);
        return result;
    }

    // 无运算符：单独变量名，检查是否非空且非 "0"
    const char *value = NULL;
    if (var_values)
    {
        value = (const char *)g_hash_table_lookup(var_values, trimmed);
    }

    gboolean result = FALSE;
    if (value && value[0] != '\0' && strcmp(value, "0") != 0)
    {
        result = TRUE;
    }

    g_free(trimmed);
    return result;
}

/**
 * @brief 检查条件栈中当前是否应输出文本
 * @param cond_stack 条件栈
 * @return TRUE 应输出，FALSE 应跳过
 */
static gboolean is_output_active(GArray *cond_stack)
{
    if (cond_stack->len == 0)
    {
        return TRUE;
    }

    // 检查栈顶帧
    template_cond_frame_t *top = &g_array_index(cond_stack, template_cond_frame_t, cond_stack->len - 1);
    return top->parent_active && top->condition_met;
}

/**
 * @brief 处理模板中的条件指令（{% if %} / {% elif %} / {% else %} / {% endif %}）
 *
 * 在变量替换之前执行，移除条件块标记，保留满足条件的内容。
 *
 * @param content 模板内容（含 {% %} 指令）
 * @param var_values 变量映射表
 * @return 处理后的字符串（调用者负责 g_free）
 */
static char *process_template_directives(const char *content, GHashTable *var_values)
{
    if (!content)
    {
        return g_strdup("");
    }

    // 快速检查：如果没有 {% 标记，直接返回原文本
    if (!strstr(content, "{%"))
    {
        return g_strdup(content);
    }

    GString *output = g_string_new("");
    GArray *cond_stack = g_array_new(FALSE, TRUE, sizeof(template_cond_frame_t));

    const char *p = content;

    while (*p)
    {
        // 检测 {% 指令开头
        if (p[0] == '{' && p[1] == '%')
        {
            // 找到 %} 结尾
            const char *end = strstr(p + 2, "%}");
            if (!end)
            {
                // 未找到闭合标记，作为普通文本输出
                if (is_output_active(cond_stack))
                {
                    g_string_append_c(output, *p);
                }
                p++;
                continue;
            }

            // 检查指令是否独占一行：{%前面到行首只有空白
            // 如果是，则移除该行前导空白（避免产生多余缩进或空行）
            gboolean standalone_line = TRUE;
            const char *line_start = p;
            while (line_start > content && *(line_start - 1) != '\n')
            {
                line_start--;
            }
            for (const char *c = line_start; c < p; c++)
            {
                if (*c != ' ' && *c != '\t')
                {
                    standalone_line = FALSE;
                    break;
                }
            }

            if (standalone_line)
            {
                // 回退输出缓冲区中该行的前导空白（仅当这些空白确实被写入时）
                size_t whitespace_len = p - line_start;
                if (whitespace_len > 0 && output->len >= whitespace_len)
                {
                    // 验证输出末尾确实是这些空白字符（可能因条件不满足而未写入）
                    const char *out_tail = output->str + output->len - whitespace_len;
                    gboolean match = TRUE;
                    for (size_t i = 0; i < whitespace_len; i++)
                    {
                        if (out_tail[i] != line_start[i])
                        {
                            match = FALSE;
                            break;
                        }
                    }
                    if (match)
                    {
                        g_string_truncate(output, output->len - whitespace_len);
                    }
                }
            }

            // 提取指令内容（{% 和 %} 之间的文本）
            char *directive = g_strndup(p + 2, end - p - 2);
            char *trimmed_dir = trim_whitespace(directive);

            if (g_str_has_prefix(trimmed_dir, "if "))
            {
                // {% if expr %} — 压入新帧
                const char *expr = trimmed_dir + 3;
                gboolean parent_active = is_output_active(cond_stack);
                gboolean cond = parent_active ? evaluate_condition(expr, var_values) : FALSE;

                template_cond_frame_t frame = {
                    .condition_met = cond,
                    .any_branch_met = cond,
                    .parent_active = parent_active,
                };
                g_array_append_val(cond_stack, frame);
            }
            else if (g_str_has_prefix(trimmed_dir, "elif "))
            {
                // {% elif expr %} — 更新栈顶
                if (cond_stack->len > 0)
                {
                    template_cond_frame_t *top =
                        &g_array_index(cond_stack, template_cond_frame_t, cond_stack->len - 1);
                    if (top->parent_active && !top->any_branch_met)
                    {
                        const char *expr = trimmed_dir + 5;
                        gboolean cond = evaluate_condition(expr, var_values);
                        top->condition_met = cond;
                        if (cond)
                        {
                            top->any_branch_met = TRUE;
                        }
                    }
                    else
                    {
                        top->condition_met = FALSE;
                    }
                }
            }
            else if (strcmp(trimmed_dir, "else") == 0)
            {
                // {% else %} — 如果之前无分支满足，激活
                if (cond_stack->len > 0)
                {
                    template_cond_frame_t *top =
                        &g_array_index(cond_stack, template_cond_frame_t, cond_stack->len - 1);
                    if (top->parent_active && !top->any_branch_met)
                    {
                        top->condition_met = TRUE;
                        top->any_branch_met = TRUE;
                    }
                    else
                    {
                        top->condition_met = FALSE;
                    }
                }
            }
            else if (strcmp(trimmed_dir, "endif") == 0)
            {
                // {% endif %} — 弹栈
                if (cond_stack->len > 0)
                {
                    g_array_remove_index(cond_stack, cond_stack->len - 1);
                }
            }

            g_free(directive);
            g_free(trimmed_dir);

            // 跳过整个 {% ... %} 标记
            p = end + 2;

            // 如果指令独占一行，跳过尾随换行（整行移除）
            if (standalone_line)
            {
                if (*p == '\n')
                {
                    p++;
                }
                else if (p[0] == '\r' && p[1] == '\n')
                {
                    p += 2;
                }
            }

            continue;
        }

        // 普通字符：检查条件栈状态决定是否输出
        if (is_output_active(cond_stack))
        {
            g_string_append_c(output, *p);
        }

        p++;
    }

    g_array_free(cond_stack, TRUE);

    // 去除末尾多余的换行（避免与渲染器追加的 \r\n 叠加产生空行）
    while (output->len > 0 && (output->str[output->len - 1] == '\n' || output->str[output->len - 1] == '\r'))
    {
        g_string_truncate(output, output->len - 1);
    }

    return g_string_free(output, FALSE);
}

// ============================================================================
// 模板渲染
// ============================================================================

static char *format_value_with_spec(const char *value, const char *spec)
{
    const char *val = value ? value : "";
    if (!spec || spec[0] == '\0')
    {
        return g_strdup(val);
    }

    char align = '<';
    const char *p = spec;
    if (*p == '<' || *p == '>' || *p == '^')
    {
        align = *p;
        p++;
    }

    int width = atoi(p);
    if (width <= 0)
    {
        return g_strdup(val);
    }

    size_t len = strlen(val);
    if ((int)len >= width)
    {
        return g_strdup(val);
    }

    int pad = width - (int)len;
    int left_pad = 0;
    int right_pad = 0;
    if (align == '>')
    {
        left_pad = pad;
    }
    else if (align == '^')
    {
        left_pad = pad / 2;
        right_pad = pad - left_pad;
    }
    else
    {
        right_pad = pad;
    }

    GString *out = g_string_new("");
    for (int i = 0; i < left_pad; i++)
    {
        g_string_append_c(out, ' ');
    }
    g_string_append(out, val);
    for (int i = 0; i < right_pad; i++)
    {
        g_string_append_c(out, ' ');
    }

    return g_string_free(out, FALSE);
}

char *config_template_render(config_template_t *template, GHashTable *var_values)
{
    if (!template || !template->body || !template->body->content)
    {
        return g_strdup("");
    }

    // 第一步：处理条件指令（{% if %} / {% elif %} / {% else %} / {% endif %}）
    char *processed = process_template_directives(template->body->content, var_values);

    // 如果没有变量表，直接返回处理后的文本
    if (!var_values)
    {
        // 将 \n 替换为 \r\n（telnet 协议需要 \r\n 才能正确回到行首）
        gchar **lines = g_strsplit(processed, "\n", -1);
        char *final = g_strjoinv("\r\n", lines);
        g_strfreev(lines);
        g_free(processed);
        return final;
    }

    // 第二步：变量替换（支持格式化：{var:<16} / {var:>6} / {var:^10} / {var:16}）
    GString *result = g_string_new("");
    const char *p = processed;
    while (*p)
    {
        if (*p == '{' && p[1] != '%')
        {
            const char *end = strchr(p, '}');
            if (!end)
            {
                g_string_append_c(result, *p);
                p++;
                continue;
            }

            char *token = g_strndup(p + 1, end - p - 1);
            char *token_stripped = g_strstrip(token);

            char *fmt = strchr(token_stripped, ':');
            if (fmt)
            {
                *fmt = '\0';
                fmt++;
            }

            const char *var_value = (const char *)g_hash_table_lookup(var_values, token_stripped);
            if (!var_value)
            {
                var_value = "";
            }

            char *formatted = format_value_with_spec(var_value, fmt);
            g_string_append(result, formatted);
            g_free(formatted);
            g_free(token);

            p = end + 1;
            continue;
        }

        g_string_append_c(result, *p);
        p++;
    }

    g_free(processed);

    // 将 \n 替换为 \r\n（telnet 协议需要 \r\n 才能正确回到行首）
    char *raw = g_string_free(result, FALSE);
    gchar **lines = g_strsplit(raw, "\n", -1);
    char *final = g_strjoinv("\r\n", lines);
    g_strfreev(lines);
    g_free(raw);

    return final;
}
