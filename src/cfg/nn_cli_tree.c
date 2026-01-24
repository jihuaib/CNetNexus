#include "nn_cli_tree.h"

#include <ctype.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nn_cli_param_type.h"
#include "nn_errcode.h"

enum
{
    INITIAL_CHILDREN_CAPACITY = 4
};

// Create a new CLI tree node
nn_cli_tree_node_t *nn_cli_tree_create_node(const char *name, const char *description, nn_cli_node_type_t type)
{
    nn_cli_tree_node_t *node = (nn_cli_tree_node_t *)g_malloc(sizeof(nn_cli_tree_node_t));

    node->name = name ? strdup(name) : NULL;
    node->description = description ? strdup(description) : NULL;
    node->type = type;
    node->callback = NULL;
    node->module_name = NULL;
    node->param_type = NULL;
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

        // Update callback if new node has one
        if (child->callback)
        {
            existing->callback = child->callback;
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

// Set callback for a node
void nn_cli_tree_set_callback(nn_cli_tree_node_t *node, nn_cli_callback_t callback)
{
    if (node)
    {
        node->callback = callback;
    }
}

// Set module name for a node
void nn_cli_tree_set_module_name(nn_cli_tree_node_t *node, const char *module_name)
{
    if (node)
    {
        g_free(node->module_name);
        node->module_name = module_name ? strdup(module_name) : NULL;
    }
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
    g_free(root->module_name);
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
    nn_cli_tree_node_t *clone = nn_cli_tree_create_node(node->name, node->description, node->type);
    if (!clone)
    {
        return NULL;
    }

    // Copy callback and module name
    clone->callback = node->callback;
    clone->module_name = node->module_name ? strdup(node->module_name) : NULL;

    // Clone param_type if exists
    if (node->param_type && node->param_type->type_str)
    {
        clone->param_type = nn_cli_param_type_parse(node->param_type->type_str);
    }

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

// Print help for a node
void nn_cli_tree_print_help(nn_cli_tree_node_t *node, uint32_t client_fd)
{
    if (!node)
    {
        return;
    }

    char buffer[512];

    if (node->num_children > 0)
    {
        for (uint32_t i = 0; i < node->num_children; i++)
        {
            nn_cli_tree_node_t *child = node->children[i];
            if (child->description)
            {
                char name_display[128];

                if (child->type == NN_CLI_NODE_ARGUMENT && child->param_type && child->param_type->type_str)
                {
                    // ARGUMENT: Display as <type(range)>
                    snprintf(name_display, sizeof(name_display), "<%s>", child->param_type->type_str);
                }
                else if (child->name)
                {
                    // COMMAND or ARGUMENT without param_type: Display name as-is
                    strncpy(name_display, child->name, sizeof(name_display) - 1);
                    name_display[sizeof(name_display) - 1] = '\0';
                }
                else
                {
                    // No name, skip this child
                    continue;
                }

                snprintf(buffer, sizeof(buffer), "  %-25s - %s\r\n", name_display, child->description);
                write(client_fd, buffer, strlen(buffer));
            }
        }
        write(client_fd, "\r\n", 2);
    }
}
