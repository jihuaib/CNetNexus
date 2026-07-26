/**
 * @file   cli_config_plan.c
 * @brief  层级配置回滚计划的预检与生成
 */
#include "cli_config_plan.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cli_handler.h"
#include "cli_tree.h"

typedef struct cli_config_prepared_node
{
    const cli_config_node_t *source;
    cli_view_node_t *parent_view;
    cli_view_node_t *child_view;
    cli_match_result_t *match;
    GPtrArray *children; /**< cli_config_prepared_node_t* */
} cli_config_prepared_node_t;

typedef struct cli_config_plan_builder
{
    cli_view_node_t *view_root;
    cli_tree_node_t *global_cmd_tree;
    cli_config_plan_t *plan;
} cli_config_plan_builder_t;

GQuark cli_config_plan_error_quark(void)
{
    return g_quark_from_static_string("cli-config-plan-error-quark");
}

static void cli_config_plan_step_free(gpointer data)
{
    cli_config_plan_step_t *step = data;
    if (!step)
    {
        return;
    }
    g_free(step->command);
    g_free(step);
}

static cli_config_plan_t *cli_config_plan_new(void)
{
    cli_config_plan_t *plan = g_new0(cli_config_plan_t, 1);
    plan->steps = g_ptr_array_new_with_free_func(cli_config_plan_step_free);
    return plan;
}

void cli_config_plan_free(cli_config_plan_t *plan)
{
    if (!plan)
    {
        return;
    }
    if (plan->steps)
    {
        g_ptr_array_free(plan->steps, TRUE);
    }
    g_free(plan);
}

gboolean cli_config_plan_is_empty(const cli_config_plan_t *plan)
{
    return !plan || !plan->steps || plan->steps->len == 0;
}

static void cli_config_prepared_node_free(gpointer data)
{
    cli_config_prepared_node_t *node = data;
    if (!node)
    {
        return;
    }
    if (node->match)
    {
        cli_match_result_free(node->match);
    }
    if (node->children)
    {
        g_ptr_array_free(node->children, TRUE);
    }
    g_free(node);
}

static void cli_config_plan_set_error(GError **error, cli_config_plan_error_t code, const cli_config_node_t *node,
                                      const gchar *format, ...)
{
    if (!error || *error)
    {
        return;
    }

    va_list args;
    va_start(args, format);
    gchar *detail = g_strdup_vprintf(format, args);
    va_end(args);

    if (node && node->line_number > 0)
    {
        g_set_error(error, CLI_CONFIG_PLAN_ERROR, code, "第 %u 行 `%s`: %s", node->line_number,
                    node->command ? node->command : "", detail);
    }
    else
    {
        g_set_error(error, CLI_CONFIG_PLAN_ERROR, code, "%s", detail);
    }
    g_free(detail);
}

static gboolean cli_config_prepare_nodes(const GPtrArray *nodes, cli_view_node_t *parent_view,
                                         cli_view_node_t *view_root, cli_tree_node_t *global_cmd_tree, GPtrArray **out,
                                         GError **error)
{
    GPtrArray *prepared = g_ptr_array_new_with_free_func(cli_config_prepared_node_free);

    for (guint i = 0; nodes && i < nodes->len; i++)
    {
        const cli_config_node_t *source = g_ptr_array_index((GPtrArray *)nodes, i);
        /* 配置模型只允许匹配所在配置视图的命令，禁止借全局命令树把
         * reboot/show/terminal 等运维命令带入启动回放或回滚计划。 */
        cli_match_result_t *match = cli_tree_match_command_full(parent_view->cmd_tree, source->command);
        if (!match || !match->final_node || !match->final_node->is_end_node)
        {
            if (match)
            {
                cli_match_result_free(match);
            }
            cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_COMMAND, source,
                                      "命令在视图 `%s` 中无效或不完整", parent_view->view_name);
            g_ptr_array_free(prepared, TRUE);
            return FALSE;
        }

        cli_config_prepared_node_t *node = g_new0(cli_config_prepared_node_t, 1);
        node->source = source;
        node->parent_view = parent_view;
        node->match = match;
        node->children = g_ptr_array_new_with_free_func(cli_config_prepared_node_free);

        const gchar *child_view_name = match->final_node->target_view_name;
        if (child_view_name)
        {
            node->child_view = cli_view_find_by_name(view_root, child_view_name);
            if (!node->child_view)
            {
                cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_HIERARCHY, source,
                                          "命令声明了不存在的目标视图 `%s`", child_view_name);
                cli_config_prepared_node_free(node);
                g_ptr_array_free(prepared, TRUE);
                return FALSE;
            }
        }

        if (source->children && source->children->len > 0)
        {
            if (!node->child_view)
            {
                cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_HIERARCHY, source,
                                          "命令不进入子视图，却包含 %u 条子配置", source->children->len);
                cli_config_prepared_node_free(node);
                g_ptr_array_free(prepared, TRUE);
                return FALSE;
            }

            GPtrArray *children = NULL;
            if (!cli_config_prepare_nodes(source->children, node->child_view, view_root, global_cmd_tree, &children,
                                          error))
            {
                cli_config_prepared_node_free(node);
                g_ptr_array_free(prepared, TRUE);
                return FALSE;
            }
            g_ptr_array_free(node->children, TRUE);
            node->children = children;
        }

        g_ptr_array_add(prepared, node);
    }

    *out = prepared;
    return TRUE;
}

static GPtrArray *cli_config_prepare_model(const cli_config_model_t *model, cli_view_node_t *config_view,
                                           cli_view_node_t *view_root, cli_tree_node_t *global_cmd_tree, GError **error)
{
    GPtrArray *prepared = NULL;
    const GPtrArray *roots = model ? model->roots : NULL;
    if (!cli_config_prepare_nodes(roots, config_view, view_root, global_cmd_tree, &prepared, error))
    {
        return NULL;
    }
    return prepared;
}

static void cli_config_plan_add_step(cli_config_plan_t *plan, const gchar *command, guint depth, gint view_delta,
                                     cli_config_plan_action_t action, guint source_line)
{
    cli_config_plan_step_t *step = g_new0(cli_config_plan_step_t, 1);
    step->command = g_strdup(command);
    step->depth = depth;
    step->view_delta = view_delta;
    step->action = action;
    step->source_line = source_line;
    g_ptr_array_add(plan->steps, step);
}

static const gchar *cli_config_match_cfg_value(const cli_match_result_t *match, guint cfg_id)
{
    if (!match)
    {
        return NULL;
    }

    for (guint i = 0; i < match->num_elements; i++)
    {
        const cli_match_element_t *element = &match->elements[i];
        if (element->cfg_id == cfg_id && element->value)
        {
            return element->value;
        }
    }
    return NULL;
}

static gchar *cli_config_expand_inverse(const gchar *template_text, const cli_config_prepared_node_t *node,
                                        GError **error)
{
    GString *expanded = g_string_new("");
    const gchar *cursor = template_text;

    while (cursor && *cursor)
    {
        const gchar *open = strstr(cursor, "{cfg:");
        if (!open)
        {
            g_string_append(expanded, cursor);
            break;
        }
        g_string_append_len(expanded, cursor, open - cursor);

        errno = 0;
        gchar *end = NULL;
        unsigned long cfg_id = strtoul(open + 5, &end, 10);
        if (errno != 0 || end == open + 5 || !end || *end != '}' || cfg_id == 0 || cfg_id > G_MAXUINT)
        {
            cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_INVERSE, node->source,
                                      "逆命令模板 `%s` 中的占位符无效", template_text);
            g_string_free(expanded, TRUE);
            return NULL;
        }

        const gchar *value = cli_config_match_cfg_value(node->match, (guint)cfg_id);
        if (!value)
        {
            cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_INVERSE, node->source,
                                      "逆命令模板 `%s` 找不到 cfg-id %lu 的参数值", template_text, cfg_id);
            g_string_free(expanded, TRUE);
            return NULL;
        }
        if (strchr(value, '\r') || strchr(value, '\n'))
        {
            cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_INVERSE, node->source,
                                      "逆命令参数包含换行符");
            g_string_free(expanded, TRUE);
            return NULL;
        }
        g_string_append(expanded, value);
        cursor = end + 1;
    }

    gchar *result = g_string_free(expanded, FALSE);
    g_strstrip(result);
    return result;
}

static cli_match_result_t *cli_config_match_inverse(const cli_config_prepared_node_t *node,
                                                    cli_tree_node_t *global_cmd_tree, const gchar *candidate,
                                                    gboolean allow_cross_group)
{
    (void)global_cmd_tree;
    if (!candidate || candidate[0] == '\0')
    {
        return NULL;
    }

    cli_match_result_t *match = cli_tree_match_command_full(node->parent_view->cmd_tree, candidate);
    if (!match || !match->final_node || !match->final_node->is_end_node || match->module_id != node->match->module_id ||
        (!allow_cross_group && match->group_id != node->match->group_id))
    {
        if (match)
        {
            cli_match_result_free(match);
        }
        return NULL;
    }
    return match;
}

static gchar *cli_config_generic_inverse(const cli_config_prepared_node_t *node, cli_tree_node_t *global_cmd_tree,
                                         cli_match_result_t **inverse_match)
{
    const gchar *command = node->source->command;
    if (g_str_has_prefix(command, "no "))
    {
        gchar *candidate = g_strdup(command + 3);
        cli_match_result_t *match = cli_config_match_inverse(node, global_cmd_tree, candidate, FALSE);
        if (match)
        {
            *inverse_match = match;
            return candidate;
        }
        g_free(candidate);
        return NULL;
    }

    gchar **tokens = g_strsplit(command, " ", -1);
    guint count = g_strv_length(tokens);
    for (guint keep = count; keep > 0; keep--)
    {
        GString *candidate_text = g_string_new("no ");
        for (guint i = 0; i < keep; i++)
        {
            if (i > 0)
            {
                g_string_append_c(candidate_text, ' ');
            }
            g_string_append(candidate_text, tokens[i]);
        }
        gchar *candidate = g_string_free(candidate_text, FALSE);
        cli_match_result_t *match = cli_config_match_inverse(node, global_cmd_tree, candidate, FALSE);
        if (match)
        {
            g_strfreev(tokens);
            *inverse_match = match;
            return candidate;
        }
        g_free(candidate);
    }
    g_strfreev(tokens);
    return NULL;
}

static gchar *cli_config_find_inverse(const cli_config_prepared_node_t *node, cli_tree_node_t *global_cmd_tree,
                                      cli_match_result_t **inverse_match, GError **error)
{
    *inverse_match = NULL;
    const gchar *template_text =
        cli_tree_node_get_inverse_template(node->match->final_node, node->match->module_id, node->match->group_id);
    if (template_text)
    {
        gchar *candidate = cli_config_expand_inverse(template_text, node, error);
        if (!candidate)
        {
            return NULL;
        }
        cli_match_result_t *match = cli_config_match_inverse(node, global_cmd_tree, candidate, TRUE);
        if (!match)
        {
            cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_INVERSE, node->source,
                                      "声明的逆命令 `%s` 在视图 `%s` 中无效", candidate, node->parent_view->view_name);
            g_free(candidate);
            return NULL;
        }
        *inverse_match = match;
        return candidate;
    }

    return cli_config_generic_inverse(node, global_cmd_tree, inverse_match);
}

static GHashTable *cli_config_prepared_index(const GPtrArray *nodes)
{
    GHashTable *index = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)g_queue_free);
    for (guint i = 0; nodes && i < nodes->len; i++)
    {
        const cli_config_prepared_node_t *node = g_ptr_array_index((GPtrArray *)nodes, i);
        GQueue *positions = g_hash_table_lookup(index, node->source->command);
        if (!positions)
        {
            positions = g_queue_new();
            g_hash_table_insert(index, node->source->command, positions);
        }
        g_queue_push_tail(positions, GUINT_TO_POINTER(i + 1));
    }
    return index;
}

static gboolean cli_config_plan_transform(cli_config_plan_builder_t *builder, const GPtrArray *current,
                                          const GPtrArray *target, guint depth, GError **error);

static gboolean cli_config_plan_remove_node(cli_config_plan_builder_t *builder, const cli_config_prepared_node_t *node,
                                            guint depth, GError **error)
{
    cli_match_result_t *inverse_match = NULL;
    gchar *inverse = cli_config_find_inverse(node, builder->global_cmd_tree, &inverse_match, error);
    if (inverse)
    {
        gint delta = inverse_match->final_node->target_view_name ? 1 : 0;
        if (delta > 0 && depth + 2 > CLI_PROMPT_STACK_DEPTH)
        {
            cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_HIERARCHY, node->source,
                                      "逆命令会超过 CLI 最大视图深度 %u", CLI_PROMPT_STACK_DEPTH);
            cli_match_result_free(inverse_match);
            g_free(inverse);
            return FALSE;
        }
        cli_config_plan_add_step(builder->plan, inverse, depth, delta, CLI_CONFIG_PLAN_ACTION_UNDO,
                                 node->source->line_number);
        if (delta > 0)
        {
            cli_config_plan_add_step(builder->plan, "exit", depth + 1, -1, CLI_CONFIG_PLAN_ACTION_EXIT, 0);
        }
        cli_match_result_free(inverse_match);
        g_free(inverse);
        return TRUE;
    }
    if (error && *error)
    {
        return FALSE;
    }

    /*
     * 固定视图（如物理接口、line console）可能没有删除入口命令。此时可以进入
     * 视图并撤销其子配置；空视图本身无法从 BDR 中安全删除，因此明确拒绝。
     */
    if (node->child_view && node->children && node->children->len > 0)
    {
        if (depth + 2 > CLI_PROMPT_STACK_DEPTH)
        {
            cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_HIERARCHY, node->source,
                                      "配置会超过 CLI 最大视图深度 %u", CLI_PROMPT_STACK_DEPTH);
            return FALSE;
        }
        cli_config_plan_add_step(builder->plan, node->source->command, depth, 1, CLI_CONFIG_PLAN_ACTION_ENTER,
                                 node->source->line_number);
        if (!cli_config_plan_transform(builder, node->children, NULL, depth + 1, error))
        {
            return FALSE;
        }
        cli_config_plan_add_step(builder->plan, "exit", depth + 1, -1, CLI_CONFIG_PLAN_ACTION_EXIT, 0);
        return TRUE;
    }

    cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_NO_INVERSE, node->source,
                              "无法在视图 `%s` 中求得安全 undo；请为该命令声明 `<inverse>`",
                              node->parent_view->view_name);
    return FALSE;
}

static gboolean cli_config_plan_add_node(cli_config_plan_builder_t *builder, const cli_config_prepared_node_t *node,
                                         guint depth, GError **error)
{
    gint delta = node->child_view ? 1 : 0;
    if (delta > 0 && depth + 2 > CLI_PROMPT_STACK_DEPTH)
    {
        cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_HIERARCHY, node->source,
                                  "配置会超过 CLI 最大视图深度 %u", CLI_PROMPT_STACK_DEPTH);
        return FALSE;
    }
    cli_config_plan_add_step(builder->plan, node->source->command, depth, delta, CLI_CONFIG_PLAN_ACTION_ADD,
                             node->source->line_number);
    if (node->child_view)
    {
        if (!cli_config_plan_transform(builder, NULL, node->children, depth + 1, error))
        {
            return FALSE;
        }
        cli_config_plan_add_step(builder->plan, "exit", depth + 1, -1, CLI_CONFIG_PLAN_ACTION_EXIT, 0);
    }
    return TRUE;
}

static gboolean cli_config_plan_transform(cli_config_plan_builder_t *builder, const GPtrArray *current,
                                          const GPtrArray *target, guint depth, GError **error)
{
    guint current_len = current ? current->len : 0;
    guint target_len = target ? target->len : 0;
    gint *current_to_target = g_new(gint, current_len);
    gint *target_to_current = g_new(gint, target_len);
    for (guint i = 0; i < current_len; i++)
    {
        current_to_target[i] = -1;
    }
    for (guint i = 0; i < target_len; i++)
    {
        target_to_current[i] = -1;
    }

    GHashTable *target_index = cli_config_prepared_index(target);
    for (guint i = 0; i < current_len; i++)
    {
        const cli_config_prepared_node_t *node = g_ptr_array_index((GPtrArray *)current, i);
        GQueue *positions = g_hash_table_lookup(target_index, node->source->command);
        if (positions && !g_queue_is_empty(positions))
        {
            guint target_pos = GPOINTER_TO_UINT(g_queue_pop_head(positions)) - 1;
            current_to_target[i] = (gint)target_pos;
            target_to_current[target_pos] = (gint)i;
        }
    }
    g_hash_table_destroy(target_index);

    /* 先 undo，且同层按 current 的逆序执行，避免父级删除先于其后的兄弟依赖。 */
    for (gint i = (gint)current_len - 1; i >= 0; i--)
    {
        if (current_to_target[i] >= 0)
        {
            continue;
        }
        const cli_config_prepared_node_t *node = g_ptr_array_index((GPtrArray *)current, (guint)i);
        if (!cli_config_plan_remove_node(builder, node, depth, error))
        {
            g_free(current_to_target);
            g_free(target_to_current);
            return FALSE;
        }
    }

    /* 共同视图仅在其内部确有变化时进入，并在完成后显式 exit。 */
    for (guint i = 0; i < target_len; i++)
    {
        if (target_to_current[i] < 0)
        {
            continue;
        }
        const cli_config_prepared_node_t *target_node = g_ptr_array_index((GPtrArray *)target, i);
        const cli_config_prepared_node_t *current_node =
            g_ptr_array_index((GPtrArray *)current, (guint)target_to_current[i]);

        if (!target_node->child_view)
        {
            continue;
        }
        if (depth + 2 > CLI_PROMPT_STACK_DEPTH)
        {
            cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_HIERARCHY, target_node->source,
                                      "配置会超过 CLI 最大视图深度 %u", CLI_PROMPT_STACK_DEPTH);
            g_free(current_to_target);
            g_free(target_to_current);
            return FALSE;
        }

        guint before = builder->plan->steps->len;
        cli_config_plan_add_step(builder->plan, target_node->source->command, depth, 1, CLI_CONFIG_PLAN_ACTION_ENTER,
                                 target_node->source->line_number);
        if (!cli_config_plan_transform(builder, current_node->children, target_node->children, depth + 1, error))
        {
            g_free(current_to_target);
            g_free(target_to_current);
            return FALSE;
        }
        if (builder->plan->steps->len == before + 1)
        {
            g_ptr_array_remove_index(builder->plan->steps, before);
        }
        else
        {
            cli_config_plan_add_step(builder->plan, "exit", depth + 1, -1, CLI_CONFIG_PLAN_ACTION_EXIT, 0);
        }
    }

    /* 再按 target 顺序新增，保证父视图先于子配置。 */
    for (guint i = 0; i < target_len; i++)
    {
        if (target_to_current[i] >= 0)
        {
            continue;
        }
        const cli_config_prepared_node_t *node = g_ptr_array_index((GPtrArray *)target, i);
        if (!cli_config_plan_add_node(builder, node, depth, error))
        {
            g_free(current_to_target);
            g_free(target_to_current);
            return FALSE;
        }
    }

    g_free(current_to_target);
    g_free(target_to_current);
    return TRUE;
}

gboolean cli_config_plan_build(const cli_config_model_t *current, const cli_config_model_t *target,
                               cli_view_node_t *config_view, cli_view_node_t *view_root,
                               cli_tree_node_t *global_cmd_tree, cli_config_plan_t **out_plan, GError **error)
{
    if (out_plan)
    {
        *out_plan = NULL;
    }
    if (!out_plan || !config_view || !config_view->cmd_tree || !view_root)
    {
        cli_config_plan_set_error(error, CLI_CONFIG_PLAN_ERROR_INVALID_ARGUMENT, NULL, "回滚计划参数不完整");
        return FALSE;
    }

    GPtrArray *prepared_current = cli_config_prepare_model(current, config_view, view_root, global_cmd_tree, error);
    if (!prepared_current)
    {
        return FALSE;
    }
    GPtrArray *prepared_target = cli_config_prepare_model(target, config_view, view_root, global_cmd_tree, error);
    if (!prepared_target)
    {
        g_ptr_array_free(prepared_current, TRUE);
        return FALSE;
    }

    cli_config_plan_t *plan = cli_config_plan_new();
    cli_config_plan_builder_t builder = {
        .view_root = view_root,
        .global_cmd_tree = global_cmd_tree,
        .plan = plan,
    };
    gboolean ok = cli_config_plan_transform(&builder, prepared_current, prepared_target, 0, error);

    g_ptr_array_free(prepared_current, TRUE);
    g_ptr_array_free(prepared_target, TRUE);
    if (!ok)
    {
        cli_config_plan_free(plan);
        return FALSE;
    }

    *out_plan = plan;
    return TRUE;
}
