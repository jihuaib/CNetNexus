#include "nn_cli_tree.h"

#include <ctype.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nn_cli_handler.h"
#include "nn_cli_param_type.h"
#include "nn_errcode.h"

enum
{
    INITIAL_CHILDREN_CAPACITY = 4
};

// Create a new CLI tree node
nn_cli_tree_node_t *nn_cli_tree_create_node(uint32_t element_id, const char *name, const char *description,
                                            nn_cli_node_type_t type, uint32_t module_id, uint32_t group_id,
                                            uint32_t view_id)
{
    nn_cli_tree_node_t *node = (nn_cli_tree_node_t *)g_malloc0(sizeof(nn_cli_tree_node_t));

    node->element_id = element_id;
    node->module_id = module_id;
    node->group_id = group_id;
    node->name = name ? strdup(name) : NULL;
    node->description = description ? strdup(description) : NULL;
    node->type = type;
    node->view_id = view_id;
    node->param_type = NULL;
    node->is_end_node = false; // Default: not an end node
    node->children = NULL;
    node->num_children = 0;
    node->children_capacity = 0;

    return node;
}

// Add a child node to a parent
void nn_cli_tree_add_child(nn_cli_tree_node_t *parent, nn_cli_tree_node_t *child)
{
    if (!parent || !child)
    {
        return;
    }

    // Check if a child with the same name already exists
    nn_cli_tree_node_t *existing = nn_cli_tree_find_child(parent, child->name);

    if (existing)
    {
        // Merge children from new node into existing node
        for (uint32_t i = 0; i < child->num_children; i++)
        {
            nn_cli_tree_add_child(existing, child->children[i]);
        }

        // Free the new node (but not its children, as they were moved)
        g_free(child->name);
        g_free(child->description);
        g_free(child->children);
        g_free(child);
        return;
    }

    // No existing child - add as new
    // Allocate or expand children array
    if (parent->num_children >= parent->children_capacity)
    {
        uint32_t new_capacity =
            parent->children_capacity == NN_ERRCODE_SUCCESS ? INITIAL_CHILDREN_CAPACITY : parent->children_capacity * 2;
        nn_cli_tree_node_t **new_children =
            (nn_cli_tree_node_t **)realloc(parent->children, new_capacity * sizeof(nn_cli_tree_node_t *));
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
nn_cli_tree_node_t *nn_cli_tree_find_child(nn_cli_tree_node_t *parent, const char *name)
{
    if (!parent || !name)
    {
        return NULL;
    }

    for (uint32_t i = 0; i < parent->num_children; i++)
    {
        if ((parent->children[i]->name) &&
            (strncmp(parent->children[i]->name, name, strlen(name)) == NN_ERRCODE_SUCCESS))
        {
            return parent->children[i];
        }
    }

    return NULL;
}

// Find a child node by input token (returns first match)
// COMMAND nodes: exact full-word match (strcmp)
// ARGUMENT nodes: validates against param_type
nn_cli_tree_node_t *nn_cli_tree_find_child_input_token(nn_cli_tree_node_t *parent, const char *token)
{
    if (!parent || !token)
    {
        return NULL;
    }

    // First, try to find exact match in COMMAND nodes
    for (uint32_t i = 0; i < parent->num_children; i++)
    {
        nn_cli_tree_node_t *child = parent->children[i];
        if (child->type == NN_CLI_NODE_COMMAND && child->name &&
            strncmp(child->name, token, strlen(token)) == NN_ERRCODE_SUCCESS)
        {
            return child;
        }
    }

    // If no COMMAND match, try ARGUMENT nodes with validation
    for (uint32_t i = 0; i < parent->num_children; i++)
    {
        nn_cli_tree_node_t *child = parent->children[i];
        if (child->type == NN_CLI_NODE_ARGUMENT)
        {
            // If param_type exists, validate the token
            if (child->param_type)
            {
                char error_msg[256];
                if (nn_cli_param_type_validate(child->param_type, token, error_msg, sizeof(error_msg)))
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
uint32_t nn_cli_tree_find_children_input_token(nn_cli_tree_node_t *parent, const char *token,
                                               nn_cli_tree_node_t **matches, uint32_t max_matches)
{
    if (!parent || !token || !matches)
    {
        return NN_ERRCODE_SUCCESS;
    }

    uint32_t count = 0;
    size_t token_len = strlen(token);

    // First, find all matching COMMAND nodes
    for (uint32_t i = 0; i < parent->num_children && count < max_matches; i++)
    {
        if (parent->children[i]->name && parent->children[i]->type == NN_CLI_NODE_COMMAND &&
            strncmp(parent->children[i]->name, token, token_len) == NN_ERRCODE_SUCCESS)
        {
            matches[count++] = parent->children[i];
        }
    }

    for (uint32_t i = 0; i < parent->num_children && count < max_matches; i++)
    {
        if (parent->children[i]->type == NN_CLI_NODE_ARGUMENT)
        {
            // If param_type exists, validate the token
            if (parent->children[i]->param_type)
            {
                char error_msg[256];
                if (nn_cli_param_type_validate(parent->children[i]->param_type, token, error_msg, sizeof(error_msg)))
                {
                    matches[count++] = parent->children[i];
                }
            }
        }
    }

    return count;
}

// Set parameter type for a node
void nn_cli_tree_set_param_type(nn_cli_tree_node_t *node, nn_cli_param_type_t *param_type)
{
    if (node)
    {
        if (node->param_type)
        {
            nn_cli_param_type_free(node->param_type);
        }
        node->param_type = param_type;
    }
}

// Free a tree node and all its children
void nn_cli_tree_free(nn_cli_tree_node_t *root)
{
    if (!root)
    {
        return;
    }

    // Free all children recursively
    for (uint32_t i = 0; i < root->num_children; i++)
    {
        nn_cli_tree_free(root->children[i]);
    }

    g_free(root->children);
    g_free(root->name);
    g_free(root->description);
    if (root->param_type)
    {
        nn_cli_param_type_free(root->param_type);
    }
    g_free(root);
}

// Clone a tree node and all its children
nn_cli_tree_node_t *nn_cli_tree_clone(nn_cli_tree_node_t *node)
{
    if (!node)
    {
        return NULL;
    }

    // Create new node with same properties
    nn_cli_tree_node_t *clone = nn_cli_tree_create_node(node->element_id, node->name, node->description, node->type,
                                                        node->module_id, node->group_id, node->view_id);
    if (!clone)
    {
        return NULL;
    }

    // Clone param_type if exists
    if (node->param_type && node->param_type->type_str)
    {
        clone->param_type = nn_cli_param_type_parse(node->param_type->type_str);
    }

    // Clone is_end_node flag
    clone->is_end_node = node->is_end_node;

    // Clone all children recursively
    for (uint32_t i = 0; i < node->num_children; i++)
    {
        nn_cli_tree_node_t *child_clone = nn_cli_tree_clone(node->children[i]);
        if (child_clone)
        {
            nn_cli_tree_add_child(clone, child_clone);
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
    if (*str == NN_ERRCODE_SUCCESS)
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
nn_cli_tree_node_t *nn_cli_tree_match_command(nn_cli_tree_node_t *root, const char *cmd_line)
{
    if (!root || !cmd_line)
    {
        return NULL;
    }

    char *cmd_copy = strdup(cmd_line);
    if (!cmd_copy)
    {
        return NULL;
    }

    char *trimmed = trim_whitespace(cmd_copy);
    if (strlen(trimmed) == NN_ERRCODE_SUCCESS)
    {
        g_free(cmd_copy);
        return root;
    }

    nn_cli_tree_node_t *current = root;
    char *token = strtok(trimmed, " ");
    nn_cli_tree_node_t *last_match = NULL;

    while (token)
    {
        nn_cli_tree_node_t *child = nn_cli_tree_find_child_input_token(current, token);

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
uint32_t nn_cli_tree_match_command_get_matches(nn_cli_tree_node_t *root, const char *cmd_line,
                                               nn_cli_tree_node_t **matches, uint32_t max_matches)
{
    if (!root || !cmd_line || !matches)
    {
        return NN_ERRCODE_SUCCESS;
    }

    char *cmd_copy = strdup(cmd_line);
    if (!cmd_copy)
    {
        return NN_ERRCODE_SUCCESS;
    }

    char *trimmed = trim_whitespace(cmd_copy);
    if (strlen(trimmed) == NN_ERRCODE_SUCCESS)
    {
        g_free(cmd_copy);
        return NN_ERRCODE_SUCCESS;
    }

    // Find the last token
    char *last_token = NULL;
    char *token = strtok(trimmed, " ");
    nn_cli_tree_node_t *current = root;

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
        nn_cli_tree_node_t *child = nn_cli_tree_find_child_input_token(current, token);
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
    uint32_t count = NN_ERRCODE_SUCCESS;
    if (last_token)
    {
        count = nn_cli_tree_find_children_input_token(current, last_token, matches, max_matches);
    }

    g_free(cmd_copy);
    return count;
}

// ============================================================================
// Command Match Result Functions
// ============================================================================

#define MATCH_RESULT_INITIAL_CAPACITY 8

// Create a new match result
nn_cli_match_result_t *nn_cli_match_result_create(void)
{
    nn_cli_match_result_t *result = g_malloc0(sizeof(nn_cli_match_result_t));

    result->module_id = 0;
    result->group_id = 0;
    result->elements = g_malloc0(MATCH_RESULT_INITIAL_CAPACITY * sizeof(nn_cli_match_element_t));
    result->num_elements = 0;
    result->capacity = MATCH_RESULT_INITIAL_CAPACITY;
    result->final_node = NULL;

    return result;
}

// Add an element to match result
void nn_cli_match_result_add_element(nn_cli_match_result_t *result, uint32_t element_id, nn_cli_node_type_t type,
                                     const char *value, nn_cli_param_type_t *param_type)
{
    if (!result)
    {
        return;
    }

    // Expand array if needed
    if (result->num_elements >= result->capacity)
    {
        result->capacity *= 2;
        result->elements = g_realloc(result->elements, result->capacity * sizeof(nn_cli_match_element_t));
    }

    nn_cli_match_element_t *elem = &result->elements[result->num_elements++];
    elem->element_id = element_id;
    elem->type = type;

    // Clone param_type to avoid sharing pointers
    if (param_type && param_type->type_str)
    {
        elem->param_type = nn_cli_param_type_parse(param_type->type_str);
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
            elem->value_len = nn_cli_param_type_get_value_length(param_type, value);
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
void nn_cli_match_result_free(nn_cli_match_result_t *result)
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
            nn_cli_param_type_free(result->elements[i].param_type);
        }
    }

    g_free(result->elements);
    g_free(result);
}

// Match command and return full result with all matched elements
nn_cli_match_result_t *nn_cli_tree_match_command_full(nn_cli_tree_node_t *root, const char *cmd_line)
{
    if (!root || !cmd_line)
    {
        return NULL;
    }

    char *cmd_copy = strdup(cmd_line);
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

    nn_cli_match_result_t *result = nn_cli_match_result_create();
    nn_cli_tree_node_t *current = root;

    // Save original string for extracting values
    char *cmd_for_values = strdup(cmd_line);
    char *trimmed_values = trim_whitespace(cmd_for_values);
    char *saveptr = NULL;
    char *value_token = strtok_r(trimmed_values, " ", &saveptr);

    char *token = strtok(trimmed, " ");

    while (token && value_token)
    {
        nn_cli_tree_node_t *child = nn_cli_tree_find_child_input_token(current, token);

        if (child)
        {
            // Add matched element to result
            if (child->type == NN_CLI_NODE_ARGUMENT)
            {
                // ARGUMENT: include the value
                nn_cli_match_result_add_element(result, child->element_id, child->type, value_token, child->param_type);
            }
            else
            {
                // COMMAND/KEYWORD: no value
                nn_cli_match_result_add_element(result, child->element_id, child->type, NULL, NULL);
            }

            result->module_id = child->module_id;
            result->group_id = child->group_id;

            current = child;
            token = strtok(NULL, " ");
            value_token = strtok_r(NULL, " ", &saveptr);
        }
        else
        {
            // No match - free result and return NULL
            nn_cli_match_result_free(result);
            g_free(cmd_copy);
            g_free(cmd_for_values);
            return NULL;
        }
    }

    result->final_node = current;

    g_free(cmd_copy);
    g_free(cmd_for_values);

    return result;
}
