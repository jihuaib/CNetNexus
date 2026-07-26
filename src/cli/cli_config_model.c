/**
 * @file   cli_config_model.c
 * @brief  独立的 BDR 层级配置模型与上下文差异实现
 */
#include "cli_config_model.h"

#include <string.h>

typedef enum cli_config_diff_side
{
    CLI_CONFIG_DIFF_ADD,
    CLI_CONFIG_DIFF_REMOVE,
} cli_config_diff_side_t;

GQuark cli_config_model_error_quark(void)
{
    return g_quark_from_static_string("cli-config-model-error-quark");
}

/**
 * @brief 递归释放一个配置节点
 */
static void cli_config_node_free(gpointer data)
{
    cli_config_node_t *node = (cli_config_node_t *)data;
    if (!node)
    {
        return;
    }

    g_free(node->command);
    g_free(node->original_indent);
    if (node->children)
    {
        g_ptr_array_free(node->children, TRUE);
    }
    g_free(node);
}

static cli_config_model_t *cli_config_model_new(void)
{
    cli_config_model_t *model = g_new0(cli_config_model_t, 1);
    model->roots = g_ptr_array_new_with_free_func(cli_config_node_free);
    return model;
}

void cli_config_model_free(cli_config_model_t *model)
{
    if (!model)
    {
        return;
    }

    if (model->roots)
    {
        g_ptr_array_free(model->roots, TRUE);
    }
    g_free(model);
}

/**
 * @brief 设置一条始终带行号的解析错误
 */
static void cli_config_model_set_line_error(GError **error, cli_config_model_error_t code, gsize line_number,
                                            const gchar *message)
{
    if (error && !*error)
    {
        g_set_error(error, CLI_CONFIG_MODEL_ERROR, code, "第 %" G_GSIZE_FORMAT " 行: %s", line_number, message);
    }
}

/**
 * @brief 将命令首尾空白删除，并将内部 ASCII 空白序列折叠为一个空格
 */
static gchar *cli_config_normalize_command(const gchar *text, gsize length)
{
    GString *normalized = g_string_sized_new(length);
    gboolean pending_space = FALSE;

    for (gsize i = 0; i < length; i++)
    {
        guchar ch = (guchar)text[i];
        if (g_ascii_isspace(ch))
        {
            if (normalized->len > 0)
            {
                pending_space = TRUE;
            }
            continue;
        }

        if (pending_space)
        {
            g_string_append_c(normalized, ' ');
            pending_space = FALSE;
        }
        g_string_append_c(normalized, (gchar)ch);
    }

    return g_string_free(normalized, FALSE);
}

static gboolean cli_config_is_control_command(const gchar *command)
{
    /* CLI 命令匹配区分大小写；这里必须使用同一规则，否则大写 EXIT 会在模型
     * 预检时被忽略、执行时却失败，从而留下部分配置。 */
    return strcmp(command, "config") == 0 || strcmp(command, "exit") == 0 || strcmp(command, "end") == 0;
}

/**
 * @brief 解析一行；返回的 command/original_indent 由调用方接管
 */
static gboolean cli_config_parse_line(const gchar *line, gsize line_length, gsize line_number, gchar **command,
                                      gchar **original_indent, guint *original_depth, gboolean *skip, GError **error)
{
    gsize indent_length = 0;
    gsize content_end = line_length;

    *command = NULL;
    *original_indent = NULL;
    *original_depth = 0;
    *skip = FALSE;

    while (indent_length < line_length && (line[indent_length] == ' ' || line[indent_length] == '\t'))
    {
        indent_length++;
    }
    while (content_end > indent_length && g_ascii_isspace((guchar)line[content_end - 1]))
    {
        content_end--;
    }

    if (content_end == indent_length || line[indent_length] == '!')
    {
        *skip = TRUE;
        return TRUE;
    }

    gsize command_length = content_end - indent_length;
    if (command_length >= CLI_CONFIG_MODEL_MAX_CMD_LEN)
    {
        gchar *message = g_strdup_printf("命令长度为 %" G_GSIZE_FORMAT "，必须小于 %u", command_length,
                                         (guint)CLI_CONFIG_MODEL_MAX_CMD_LEN);
        cli_config_model_set_line_error(error, CLI_CONFIG_MODEL_ERROR_COMMAND_TOO_LONG, line_number, message);
        g_free(message);
        return FALSE;
    }

    gchar *normalized = cli_config_normalize_command(line + indent_length, command_length);
    if (normalized[0] == '\0' || normalized[0] == '!' || cli_config_is_control_command(normalized))
    {
        g_free(normalized);
        *skip = TRUE;
        return TRUE;
    }

    if (indent_length > G_MAXUINT)
    {
        cli_config_model_set_line_error(error, CLI_CONFIG_MODEL_ERROR_DEPTH_EXCEEDED, line_number,
                                        "原始缩进长度超出可表示范围");
        g_free(normalized);
        return FALSE;
    }

    *command = normalized;
    *original_indent = g_strndup(line, indent_length);
    *original_depth = (guint)indent_length;
    return TRUE;
}

gboolean cli_config_model_parse(const gchar *text, cli_config_model_t **out_model, GError **error)
{
    if (out_model)
    {
        *out_model = NULL;
    }
    if (!out_model)
    {
        cli_config_model_set_line_error(error, CLI_CONFIG_MODEL_ERROR_INVALID_ARGUMENT, 0, "out_model 不能为 NULL");
        return FALSE;
    }
    if (!text)
    {
        cli_config_model_set_line_error(error, CLI_CONFIG_MODEL_ERROR_INVALID_ARGUMENT, 0, "text 不能为 NULL");
        return FALSE;
    }

    cli_config_model_t *model = cli_config_model_new();
    cli_config_node_t *parents[CLI_CONFIG_MODEL_MAX_DEPTH + 1] = {NULL};
    gboolean have_previous = FALSE;
    guint previous_depth = 0;
    gsize line_number = 1;
    const gchar *line_start = text;

    while (TRUE)
    {
        const gchar *newline = strchr(line_start, '\n');
        gsize line_length = newline ? (gsize)(newline - line_start) : strlen(line_start);

        gchar *command = NULL;
        gchar *original_indent = NULL;
        guint original_depth = 0;
        gboolean skip = FALSE;
        if (!cli_config_parse_line(line_start, line_length, line_number, &command, &original_indent, &original_depth,
                                   &skip, error))
        {
            cli_config_model_free(model);
            return FALSE;
        }

        if (!skip)
        {
            if (original_depth > CLI_CONFIG_MODEL_MAX_DEPTH)
            {
                gchar *message =
                    g_strdup_printf("缩进层级 %u 超过最大层级 %u", original_depth, (guint)CLI_CONFIG_MODEL_MAX_DEPTH);
                cli_config_model_set_line_error(error, CLI_CONFIG_MODEL_ERROR_DEPTH_EXCEEDED, line_number, message);
                g_free(message);
                g_free(command);
                g_free(original_indent);
                cli_config_model_free(model);
                return FALSE;
            }

            if (!have_previous && original_depth != 0)
            {
                gchar *message = g_strdup_printf("首条配置命令不能从层级 %u 开始", original_depth);
                cli_config_model_set_line_error(error, CLI_CONFIG_MODEL_ERROR_INDENT_JUMP, line_number, message);
                g_free(message);
                g_free(command);
                g_free(original_indent);
                cli_config_model_free(model);
                return FALSE;
            }

            if (have_previous && original_depth > previous_depth + 1)
            {
                gchar *message =
                    g_strdup_printf("缩进从层级 %u 跳到层级 %u，每次最多深入一级", previous_depth, original_depth);
                cli_config_model_set_line_error(error, CLI_CONFIG_MODEL_ERROR_INDENT_JUMP, line_number, message);
                g_free(message);
                g_free(command);
                g_free(original_indent);
                cli_config_model_free(model);
                return FALSE;
            }

            if (original_depth > 0 && !parents[original_depth - 1])
            {
                gchar *message = g_strdup_printf("层级 %u 缺少父配置上下文", original_depth);
                cli_config_model_set_line_error(error, CLI_CONFIG_MODEL_ERROR_INDENT_JUMP, line_number, message);
                g_free(message);
                g_free(command);
                g_free(original_indent);
                cli_config_model_free(model);
                return FALSE;
            }

            cli_config_node_t *node = g_new0(cli_config_node_t, 1);
            node->command = command;
            node->original_indent = original_indent;
            node->original_depth = original_depth;
            node->depth = original_depth;
            node->line_number = (guint)MIN(line_number, (gsize)G_MAXUINT);
            node->children = g_ptr_array_new_with_free_func(cli_config_node_free);

            GPtrArray *siblings = original_depth == 0 ? model->roots : parents[original_depth - 1]->children;
            g_ptr_array_add(siblings, node);
            parents[original_depth] = node;
            for (guint depth = original_depth + 1; depth <= CLI_CONFIG_MODEL_MAX_DEPTH; depth++)
            {
                parents[depth] = NULL;
            }

            previous_depth = original_depth;
            have_previous = TRUE;
        }

        if (!newline)
        {
            break;
        }
        line_start = newline + 1;
        line_number++;
    }

    *out_model = model;
    return TRUE;
}

/**
 * @brief 为一个兄弟节点数组建立 command -> 节点下标队列
 *
 * key 借用节点 command，不单独释放。队列确保重复命令按 multiset 逐个消费。
 */
static GHashTable *cli_config_build_command_index(const GPtrArray *nodes)
{
    GHashTable *index = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)g_queue_free);
    if (!nodes)
    {
        return index;
    }

    for (guint i = 0; i < nodes->len; i++)
    {
        const cli_config_node_t *node = g_ptr_array_index((GPtrArray *)nodes, i);
        GQueue *positions = g_hash_table_lookup(index, node->command);
        if (!positions)
        {
            positions = g_queue_new();
            g_hash_table_insert(index, node->command, positions);
        }

        /* 下标加一，避免将下标 0 编码成 NULL。 */
        g_queue_push_tail(positions, GUINT_TO_POINTER(i + 1));
    }
    return index;
}

static const GPtrArray *cli_config_roots_or_null(const cli_config_model_t *model)
{
    return model ? model->roots : NULL;
}

/**
 * @brief 以前序顺序输出一整棵独有子树
 */
static void cli_config_render_subtree(const cli_config_node_t *node, gchar marker, GString *output)
{
    g_string_append_c(output, marker);
    g_string_append(output, node->original_indent);
    g_string_append(output, node->command);
    g_string_append(output, "\r\n");

    for (guint i = 0; i < node->children->len; i++)
    {
        cli_config_render_subtree(g_ptr_array_index(node->children, i), marker, output);
    }
}

/**
 * @brief 在共同父视图下渲染差异
 *
 * 共同容器的子树存在差异时，以空格标记输出一次容器上下文。这样同一条命令
 * 分别位于两个 VRF/AF 时，用户仍能看出真正变化的是哪一棵配置路径。
 */
static void cli_config_render_diff(const GPtrArray *current, const GPtrArray *target, GString *output)
{
    guint current_len = current ? current->len : 0;
    gboolean *current_matched = g_new0(gboolean, current_len);
    GHashTable *current_index = cli_config_build_command_index(current);

    for (guint i = 0; target && i < target->len; i++)
    {
        const cli_config_node_t *target_node = g_ptr_array_index((GPtrArray *)target, i);
        GQueue *positions = g_hash_table_lookup(current_index, target_node->command);
        if (!positions || g_queue_is_empty(positions))
        {
            cli_config_render_subtree(target_node, '+', output);
            continue;
        }

        guint current_position = GPOINTER_TO_UINT(g_queue_pop_head(positions)) - 1;
        current_matched[current_position] = TRUE;
        const cli_config_node_t *current_node = g_ptr_array_index((GPtrArray *)current, current_position);

        GString *children = g_string_new("");
        cli_config_render_diff(current_node->children, target_node->children, children);
        if (children->len > 0)
        {
            g_string_append_c(output, ' ');
            g_string_append(output, target_node->original_indent);
            g_string_append(output, target_node->command);
            g_string_append(output, "\r\n");
            g_string_append(output, children->str);
        }
        g_string_free(children, TRUE);
    }

    for (guint i = 0; current && i < current->len; i++)
    {
        if (!current_matched[i])
        {
            cli_config_render_subtree(g_ptr_array_index((GPtrArray *)current, i), '-', output);
        }
    }

    g_hash_table_destroy(current_index);
    g_free(current_matched);
}

GString *cli_config_model_diff(const cli_config_model_t *current, const cli_config_model_t *target)
{
    GString *output = g_string_new("");
    const GPtrArray *current_roots = cli_config_roots_or_null(current);
    const GPtrArray *target_roots = cli_config_roots_or_null(target);

    cli_config_render_diff(current_roots, target_roots, output);
    return output;
}

/**
 * @brief 无输出地检查单侧差异，发现首个差异后立即返回
 */
static gboolean cli_config_side_has_diff(const GPtrArray *current, const GPtrArray *target, cli_config_diff_side_t side)
{
    const GPtrArray *primary = side == CLI_CONFIG_DIFF_ADD ? target : current;
    const GPtrArray *other = side == CLI_CONFIG_DIFF_ADD ? current : target;
    if (!primary || primary->len == 0)
    {
        return FALSE;
    }

    gboolean different = FALSE;
    GHashTable *other_index = cli_config_build_command_index(other);
    for (guint i = 0; i < primary->len; i++)
    {
        const cli_config_node_t *node = g_ptr_array_index((GPtrArray *)primary, i);
        GQueue *positions = g_hash_table_lookup(other_index, node->command);
        if (!positions || g_queue_is_empty(positions))
        {
            different = TRUE;
            break;
        }

        guint other_position = GPOINTER_TO_UINT(g_queue_pop_head(positions)) - 1;
        const cli_config_node_t *other_node = g_ptr_array_index((GPtrArray *)other, other_position);
        if ((side == CLI_CONFIG_DIFF_ADD && cli_config_side_has_diff(other_node->children, node->children, side)) ||
            (side == CLI_CONFIG_DIFF_REMOVE && cli_config_side_has_diff(node->children, other_node->children, side)))
        {
            different = TRUE;
            break;
        }
    }

    g_hash_table_destroy(other_index);
    return different;
}

gboolean cli_config_model_has_diff(const cli_config_model_t *current, const cli_config_model_t *target)
{
    const GPtrArray *current_roots = cli_config_roots_or_null(current);
    const GPtrArray *target_roots = cli_config_roots_or_null(target);
    return cli_config_side_has_diff(current_roots, target_roots, CLI_CONFIG_DIFF_ADD) ||
           cli_config_side_has_diff(current_roots, target_roots, CLI_CONFIG_DIFF_REMOVE);
}
