/**
 * @file   cli_tree.c
 * @brief  CLI 命令树匹配和补全
 * @author jhb
 * @date   2026/01/22
 */
#include "cli_tree.h"

#include <ctype.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli_handler.h"
#include "cli_param_type.h"
#include "errcode.h"

enum
{
    INITIAL_CHILDREN_CAPACITY = 4
};

static void cli_tree_node_add_cfg_binding(cli_tree_node_t *node, uint32_t module_id, uint32_t group_id, uint32_t cfg_id)
{
    if (!node || cfg_id == 0)
    {
        return;
    }

    for (uint32_t i = 0; i < node->num_cfg_bindings; i++)
    {
        cli_cfg_binding_t *b = &node->cfg_bindings[i];
        if (b->module_id == module_id && b->group_id == group_id)
        {
            b->cfg_id = cfg_id;
            return;
        }
    }

    if (node->num_cfg_bindings >= node->cfg_bindings_capacity)
    {
        uint32_t new_capacity = (node->cfg_bindings_capacity == 0) ? 4 : (node->cfg_bindings_capacity * 2);
        cli_cfg_binding_t *new_bindings = g_realloc(node->cfg_bindings, new_capacity * sizeof(cli_cfg_binding_t));
        if (!new_bindings)
        {
            return;
        }
        node->cfg_bindings = new_bindings;
        node->cfg_bindings_capacity = new_capacity;
    }

    cli_cfg_binding_t *dst = &node->cfg_bindings[node->num_cfg_bindings++];
    dst->module_id = module_id;
    dst->group_id = group_id;
    dst->cfg_id = cfg_id;
}

static void cli_tree_node_merge_cfg_bindings(cli_tree_node_t *dst, const cli_tree_node_t *src)
{
    if (!dst || !src)
    {
        return;
    }

    for (uint32_t i = 0; i < src->num_cfg_bindings; i++)
    {
        const cli_cfg_binding_t *b = &src->cfg_bindings[i];
        cli_tree_node_add_cfg_binding(dst, b->module_id, b->group_id, b->cfg_id);
    }
}

static uint32_t cli_tree_node_find_cfg_binding(const cli_tree_node_t *node, uint32_t module_id, uint32_t group_id)
{
    if (!node)
    {
        return 0;
    }

    for (uint32_t i = 0; i < node->num_cfg_bindings; i++)
    {
        const cli_cfg_binding_t *b = &node->cfg_bindings[i];
        if (b->module_id == module_id && b->group_id == group_id)
        {
            return b->cfg_id;
        }
    }

    return 0;
}

// Create a new CLI tree node
cli_tree_node_t *cli_tree_create_node(uint32_t cfg_id, const char *name, const char *description, cli_node_type_t type,
                                      uint32_t module_id, uint32_t group_id, const char *target_view_name)
{
    cli_tree_node_t *node = (cli_tree_node_t *)g_malloc0(sizeof(cli_tree_node_t));

    node->cfg_id = cfg_id;
    node->module_id = module_id;
    node->group_id = group_id;
    node->name = name ? g_strdup(name) : NULL;
    node->description = description ? g_strdup(description) : NULL;
    node->type = type;
    node->target_view_name = target_view_name ? g_strdup(target_view_name) : NULL;
    node->param_type = NULL;
    node->is_end_node = FALSE;
    node->allow_auto_start = FALSE;
    node->children = NULL;
    node->num_children = 0;
    node->children_capacity = 0;
    node->cfg_bindings = NULL;
    node->num_cfg_bindings = 0;
    node->cfg_bindings_capacity = 0;
    cli_tree_node_add_cfg_binding(node, module_id, group_id, cfg_id);

    return node;
}

// Add a child node to a parent
void cli_tree_add_child(cli_tree_node_t *parent, cli_tree_node_t *child)
{
    if (!parent || !child)
    {
        return;
    }

    // Check if a child with the same name already exists
    cli_tree_node_t *existing = cli_tree_find_child(parent, child->name);

    if (existing)
    {
        /* 合并 cfg 绑定，确保同名共享节点可按 module/group 还原 cfg-id */
        cli_tree_node_merge_cfg_bindings(existing, child);

        // Merge children from new node into existing node
        for (uint32_t i = 0; i < child->num_children; i++)
        {
            cli_tree_add_child(existing, child->children[i]);
        }

        // 转移 context_out（若子节点有而现有节点没有）
        if (child->context_out && !existing->context_out)
        {
            existing->context_out = child->context_out;
            existing->num_context_out = child->num_context_out;
            child->context_out = NULL;
        }

        // 转移 target_view_name（若子节点有而现有节点没有）
        if (child->target_view_name && !existing->target_view_name)
        {
            existing->target_view_name = child->target_view_name;
            child->target_view_name = NULL;
        }

        // 转移 is_end_node 标志
        if (child->is_end_node)
        {
            existing->is_end_node = TRUE;
        }
        if (child->allow_auto_start)
        {
            existing->allow_auto_start = TRUE;
        }

        /* 合并时 child 节点会被释放；参数类型要么转移给 existing，要么显式释放，
         * 否则 clone 出来的 type_str 会泄漏。 */
        if (child->param_type)
        {
            if (!existing->param_type)
            {
                existing->param_type = child->param_type;
                child->param_type = NULL;
            }
            else
            {
                cli_param_type_free(child->param_type);
                child->param_type = NULL;
            }
        }

        // Free the new node (but not its children, as they were moved)
        g_free(child->name);
        g_free(child->description);
        g_free(child->context_out);
        g_free(child->target_view_name);
        g_free(child->cfg_bindings);
        g_free(child->children);
        g_free(child);
        return;
    }

    // No existing child - add as new
    // Allocate or expand children array
    if (parent->num_children >= parent->children_capacity)
    {
        uint32_t new_capacity =
            parent->children_capacity == ERRCODE_SUCCESS ? INITIAL_CHILDREN_CAPACITY : parent->children_capacity * 2;
        cli_tree_node_t **new_children =
            (cli_tree_node_t **)realloc(parent->children, new_capacity * sizeof(cli_tree_node_t *));
        if (!new_children)
        {
            return;
        }

        parent->children = new_children;
        parent->children_capacity = new_capacity;
    }

    parent->children[parent->num_children++] = child;
}

// Find a child node by name (exact match)
cli_tree_node_t *cli_tree_find_child(cli_tree_node_t *parent, const char *name)
{
    if (!parent || !name)
    {
        return NULL;
    }

    for (uint32_t i = 0; i < parent->num_children; i++)
    {
        if ((parent->children[i]->name) && (strncmp(parent->children[i]->name, name, strlen(name)) == ERRCODE_SUCCESS))
        {
            return parent->children[i];
        }
    }

    return NULL;
}

// Find a child node by input token (returns first match)
// COMMAND nodes: exact full-word match (strcmp)
// ARGUMENT nodes: validates against param_type
cli_tree_node_t *cli_tree_find_child_input_token(cli_tree_node_t *parent, const char *token)
{
    if (!parent || !token)
    {
        return NULL;
    }

    // First, try to find exact match in COMMAND nodes
    for (uint32_t i = 0; i < parent->num_children; i++)
    {
        cli_tree_node_t *child = parent->children[i];
        if (child->type == CLI_NODE_COMMAND && child->name &&
            strncmp(child->name, token, strlen(token)) == ERRCODE_SUCCESS)
        {
            return child;
        }
    }

    // If no COMMAND match, try ARGUMENT nodes with validation
    for (uint32_t i = 0; i < parent->num_children; i++)
    {
        cli_tree_node_t *child = parent->children[i];
        if (child->type == CLI_NODE_ARGUMENT)
        {
            // If param_type exists, validate the token
            if (child->param_type)
            {
                char error_msg[256];
                if (cli_param_type_validate(child->param_type, token, error_msg, sizeof(error_msg)))
                {
                    return child;
                }
            }
            else
            {
                // No param_type, accept any input
                return NULL;
            }
        }
    }

    return NULL;
}

// Find all child nodes matching input token (returns list for COMMAND type)
uint32_t cli_tree_find_children_input_token(cli_tree_node_t *parent, const char *token, cli_tree_node_t **matches,
                                            uint32_t max_matches)
{
    if (!parent || !token || !matches)
    {
        return ERRCODE_SUCCESS;
    }

    uint32_t count = 0;
    size_t token_len = strlen(token);

    // First, find all matching COMMAND nodes
    for (uint32_t i = 0; i < parent->num_children && count < max_matches; i++)
    {
        if (parent->children[i]->name && parent->children[i]->type == CLI_NODE_COMMAND &&
            strncmp(parent->children[i]->name, token, token_len) == ERRCODE_SUCCESS)
        {
            matches[count++] = parent->children[i];
        }
    }

    for (uint32_t i = 0; i < parent->num_children && count < max_matches; i++)
    {
        if (parent->children[i]->type == CLI_NODE_ARGUMENT)
        {
            if (token_len == 0)
            {
                matches[count++] = parent->children[i];
                continue;
            }

            // If param_type exists, validate the token
            if (parent->children[i]->param_type)
            {
                char error_msg[256];
                if (cli_param_type_validate(parent->children[i]->param_type, token, error_msg, sizeof(error_msg)))
                {
                    matches[count++] = parent->children[i];
                }
            }
        }
    }

    return count;
}

// Set parameter type for a node
void cli_tree_set_param_type(cli_tree_node_t *node, cli_param_type_t *param_type)
{
    if (node)
    {
        if (node->param_type)
        {
            cli_param_type_free(node->param_type);
        }
        node->param_type = param_type;
    }
}

// Free a tree node and all its children
void cli_tree_free(cli_tree_node_t *root)
{
    if (!root)
    {
        return;
    }

    // Free all children recursively
    for (uint32_t i = 0; i < root->num_children; i++)
    {
        cli_tree_free(root->children[i]);
    }

    g_free(root->children);
    g_free(root->name);
    g_free(root->description);
    g_free(root->target_view_name);
    g_free(root->context_out);
    g_free(root->cfg_bindings);
    if (root->param_type)
    {
        cli_param_type_free(root->param_type);
    }
    g_free(root);
}

// Clone a tree node and all its children
cli_tree_node_t *cli_tree_clone(cli_tree_node_t *node)
{
    if (!node)
    {
        return NULL;
    }

    // Create new node with same properties
    cli_tree_node_t *clone = cli_tree_create_node(node->cfg_id, node->name, node->description, node->type,
                                                  node->module_id, node->group_id, node->target_view_name);
    if (!clone)
    {
        return NULL;
    }

    // Clone param_type if exists
    if (node->param_type && node->param_type->type_str)
    {
        clone->param_type = cli_param_type_parse(node->param_type->type_str);
    }

    // Clone is_end_node flag
    clone->is_end_node = node->is_end_node;
    clone->allow_auto_start = node->allow_auto_start;

    // Clone context_out entries
    if (node->context_out && node->num_context_out > 0)
    {
        clone->context_out = g_malloc(node->num_context_out * sizeof(cli_ctx_out_entry_t));
        memcpy(clone->context_out, node->context_out, node->num_context_out * sizeof(cli_ctx_out_entry_t));
        clone->num_context_out = node->num_context_out;
    }

    if (node->cfg_bindings && node->num_cfg_bindings > 0)
    {
        /* cli_tree_create_node() 已为 clone 分配过默认绑定，这里要先释放再覆盖。 */
        g_free(clone->cfg_bindings);
        clone->cfg_bindings = NULL;
        clone->cfg_bindings = g_malloc(node->num_cfg_bindings * sizeof(cli_cfg_binding_t));
        memcpy(clone->cfg_bindings, node->cfg_bindings, node->num_cfg_bindings * sizeof(cli_cfg_binding_t));
        clone->num_cfg_bindings = node->num_cfg_bindings;
        clone->cfg_bindings_capacity = node->num_cfg_bindings;
    }

    // Clone all children recursively
    for (uint32_t i = 0; i < node->num_children; i++)
    {
        cli_tree_node_t *child_clone = cli_tree_clone(node->children[i]);
        if (child_clone)
        {
            cli_tree_add_child(clone, child_clone);
        }
    }

    return clone;
}

// Trim whitespace from string
static char *trim_whitespace(char *str)
{
    char *end;

    while (isspace((unsigned char)*str))
    {
        str++;
    }
    if (*str == ERRCODE_SUCCESS)
    {
        return str;
    }

    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
    {
        end--;
    }
    *(end + 1) = '\0';

    return str;
}

// Match command against tree and return the matching node
cli_tree_node_t *cli_tree_match_command(cli_tree_node_t *root, const char *cmd_line)
{
    if (!root || !cmd_line)
    {
        return NULL;
    }

    char *cmd_copy = g_strdup(cmd_line);
    if (!cmd_copy)
    {
        return NULL;
    }

    char *trimmed = trim_whitespace(cmd_copy);
    if (strlen(trimmed) == ERRCODE_SUCCESS)
    {
        g_free(cmd_copy);
        return root;
    }

    cli_tree_node_t *current = root;
    char *token = strtok(trimmed, " ");
    cli_tree_node_t *last_match = NULL;

    while (token)
    {
        cli_tree_node_t *child = cli_tree_find_child_input_token(current, token);

        if (child)
        {
            last_match = child;
            current = child;
            token = strtok(NULL, " ");
        }
        else
        {
            last_match = NULL;
            break;
        }
    }

    g_free(cmd_copy);
    return last_match;
}

// Get all matching commands for the last token in cmd_line
uint32_t cli_tree_match_command_get_matches(cli_tree_node_t *root, const char *cmd_line, cli_tree_node_t **matches,
                                            uint32_t max_matches)
{
    if (!root || !cmd_line || !matches)
    {
        return ERRCODE_SUCCESS;
    }

    size_t cmd_len = strlen(cmd_line);
    if (cmd_len == 0)
    {
        return ERRCODE_SUCCESS;
    }

    const char *first_non_space = cmd_line;
    while (*first_non_space && isspace((unsigned char)*first_non_space))
    {
        first_non_space++;
    }
    if (*first_non_space == '\0')
    {
        return ERRCODE_SUCCESS;
    }

    if (isspace((unsigned char)cmd_line[cmd_len - 1]))
    {
        return ERRCODE_SUCCESS;
    }

    char *cmd_copy = g_strdup(cmd_line);
    if (!cmd_copy)
    {
        return ERRCODE_SUCCESS;
    }

    char *trimmed = trim_whitespace(cmd_copy);
    if (strlen(trimmed) == ERRCODE_SUCCESS)
    {
        g_free(cmd_copy);
        return ERRCODE_SUCCESS;
    }

    // Find the last token
    char *last_token = NULL;
    char *token = strtok(trimmed, " ");
    cli_tree_node_t *current = root;

    while (token)
    {
        char *next_token = strtok(NULL, " ");
        if (!next_token)
        {
            // This is the last token
            last_token = token;
            break;
        }

        // Not the last token, try to advance in the tree
        cli_tree_node_t *child = cli_tree_find_child_input_token(current, token);
        if (child)
        {
            current = child;
        }
        else
        {
            // Can't advance further, break
            break;
        }

        token = next_token;
    }

    // Get all matches for the last token
    uint32_t count = ERRCODE_SUCCESS;
    if (last_token)
    {
        count = cli_tree_find_children_input_token(current, last_token, matches, max_matches);
    }

    g_free(cmd_copy);
    return count;
}

// 双树 get_matches：先从 view 树收集，再从 global 树收集，按名称去重
uint32_t cli_tree_match_command_get_matches_dual(cli_tree_node_t *view_root, cli_tree_node_t *global_root,
                                                 const char *cmd_line, cli_tree_node_t **matches, uint32_t max_matches)
{
    if (!matches || max_matches == 0)
    {
        return 0;
    }

    uint32_t count = 0;

    /* 先从 view tree 收集 */
    if (view_root)
    {
        count = cli_tree_match_command_get_matches(view_root, cmd_line, matches, max_matches);
    }

    /* 再从 global tree 收集，按名称去重后追加 */
    if (global_root && count < max_matches)
    {
        cli_tree_node_t *global_matches[50];
        uint32_t global_count =
            cli_tree_match_command_get_matches(global_root, cmd_line, global_matches, max_matches - count);

        for (uint32_t i = 0; i < global_count && count < max_matches; i++)
        {
            gboolean dup = FALSE;
            for (uint32_t j = 0; j < count; j++)
            {
                if (matches[j]->name && global_matches[i]->name &&
                    strcmp(matches[j]->name, global_matches[i]->name) == 0)
                {
                    dup = TRUE;
                    break;
                }
            }
            if (!dup)
            {
                matches[count++] = global_matches[i];
            }
        }
    }

    return count;
}

// 双树全量匹配：先尝试 view tree，失败后回退 global tree
cli_match_result_t *cli_tree_match_command_full_dual(cli_tree_node_t *view_root, cli_tree_node_t *global_root,
                                                     const char *cmd_line)
{
    if (view_root)
    {
        cli_match_result_t *result = cli_tree_match_command_full(view_root, cmd_line);
        if (result)
        {
            return result;
        }
    }

    if (global_root)
    {
        return cli_tree_match_command_full(global_root, cmd_line);
    }

    return NULL;
}

// ============================================================================
// Command Match Result Functions
// ============================================================================

#define MATCH_RESULT_INITIAL_CAPACITY 8

// Create a new match result
cli_match_result_t *cli_match_result_create(void)
{
    cli_match_result_t *result = g_malloc0(sizeof(cli_match_result_t));

    result->module_id = 0;
    result->group_id = 0;
    result->elements = g_malloc0(MATCH_RESULT_INITIAL_CAPACITY * sizeof(cli_match_element_t));
    result->num_elements = 0;
    result->capacity = MATCH_RESULT_INITIAL_CAPACITY;
    result->final_node = NULL;
    result->allow_auto_start = FALSE;

    return result;
}

// Add an element to match result
void cli_match_result_add_element(cli_match_result_t *result, uint32_t cfg_id, cli_node_type_t type, const char *value,
                                  cli_param_type_t *param_type)
{
    if (!result)
    {
        return;
    }

    // Expand array if needed
    if (result->num_elements >= result->capacity)
    {
        result->capacity *= 2;
        result->elements = g_realloc(result->elements, result->capacity * sizeof(cli_match_element_t));
    }

    cli_match_element_t *elem = &result->elements[result->num_elements++];
    elem->cfg_id = cfg_id;
    elem->type = type;

    // Clone param_type to avoid sharing pointers
    if (param_type && param_type->type_str)
    {
        elem->param_type = cli_param_type_parse(param_type->type_str);
    }
    else
    {
        elem->param_type = NULL;
    }

    if (value)
    {
        elem->value = g_strdup(value);

        // Calculate length based on parameter type
        if (param_type)
        {
            elem->value_len = cli_param_type_get_value_length(param_type, value);
        }
        else
        {
            // Fallback to strlen if no param_type
            elem->value_len = strlen(value);
        }
    }
    else
    {
        elem->value = NULL;
        elem->value_len = 0;
    }
}

// Free match result
void cli_match_result_free(cli_match_result_t *result)
{
    if (!result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_elements; i++)
    {
        g_free(result->elements[i].value);
        if (result->elements[i].param_type)
        {
            cli_param_type_free(result->elements[i].param_type);
        }
    }

    g_free(result->elements);
    g_free(result);
}

// Match command and return full result with all matched elements
cli_match_result_t *cli_tree_match_command_full(cli_tree_node_t *root, const char *cmd_line)
{
    if (!root || !cmd_line)
    {
        return NULL;
    }

    char *cmd_copy = g_strdup(cmd_line);
    if (!cmd_copy)
    {
        return NULL;
    }

    char *trimmed = trim_whitespace(cmd_copy);
    if (strlen(trimmed) == 0)
    {
        g_free(cmd_copy);
        return NULL;
    }

    cli_match_result_t *result = cli_match_result_create();
    cli_tree_node_t *current = root;
    GArray *path_nodes = g_array_new(FALSE, FALSE, sizeof(cli_tree_node_t *));
    GArray *path_values = g_array_new(FALSE, FALSE, sizeof(char *));
    if (!path_nodes || !path_values)
    {
        if (path_nodes)
        {
            g_array_free(path_nodes, TRUE);
        }
        if (path_values)
        {
            g_array_free(path_values, TRUE);
        }
        cli_match_result_free(result);
        g_free(cmd_copy);
        return NULL;
    }

    // Save original string for extracting values
    char *cmd_for_values = g_strdup(cmd_line);
    char *trimmed_values = trim_whitespace(cmd_for_values);
    char *saveptr = NULL;
    char *value_token = strtok_r(trimmed_values, " ", &saveptr);

    char *token = strtok(trimmed, " ");

    while (token && value_token)
    {
        cli_tree_node_t *child = cli_tree_find_child_input_token(current, token);

        if (!child)
        {
            // No match - g_free result and return NULL
            g_array_free(path_nodes, TRUE);
            g_array_free(path_values, TRUE);
            cli_match_result_free(result);
            g_free(cmd_copy);
            g_free(cmd_for_values);
            return NULL;
        }

        /* 检测 "no" 前缀关键字（无需 cfg-id） */
        if (child->type == CLI_NODE_COMMAND && child->name && strcmp(child->name, "no") == 0)
        {
            result->has_no_prefix = TRUE;
        }
        /* 检测 "show" 关键字 */
        if (child->type == CLI_NODE_COMMAND && child->name && strcmp(child->name, "show") == 0)
        {
            result->has_show_prefix = TRUE;
        }

        g_array_append_val(path_nodes, child);
        g_array_append_val(path_values, value_token);

        result->module_id = child->module_id;
        result->group_id = child->group_id;

        current = child;
        token = strtok(NULL, " ");
        value_token = strtok_r(NULL, " ", &saveptr);
    }

    result->final_node = current;
    result->allow_auto_start = current ? current->allow_auto_start : FALSE;

    for (uint32_t i = 0; i < path_nodes->len && i < path_values->len; i++)
    {
        cli_tree_node_t *node = g_array_index(path_nodes, cli_tree_node_t *, i);
        const char *elem_value = g_array_index(path_values, char *, i);

        uint32_t cfg_id = cli_tree_node_find_cfg_binding(node, result->module_id, result->group_id);
        if (cfg_id == 0)
        {
            cfg_id = node->cfg_id;
        }
        if (cfg_id == 0)
        {
            continue;
        }

        cli_match_result_add_element(result, cfg_id, node->type, elem_value,
                                     (node->type == CLI_NODE_ARGUMENT) ? node->param_type : NULL);
    }

    g_array_free(path_nodes, TRUE);
    g_array_free(path_values, TRUE);
    g_free(cmd_copy);
    g_free(cmd_for_values);

    return result;
}
