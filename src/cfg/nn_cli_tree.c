#include "nn_cli_tree.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INITIAL_CHILDREN_CAPACITY 4

// Create a new CLI tree node
nn_cli_tree_node_t *nn_cli_tree_create_node(const char *name, const char *description, nn_cli_node_type_t type)
{
    nn_cli_tree_node_t *node = (nn_cli_tree_node_t *)malloc(sizeof(nn_cli_tree_node_t));
    if (!node)
    {
        return NULL;
    }

    node->name = name ? strdup(name) : NULL;
    node->description = description ? strdup(description) : NULL;
    node->type = type;
    node->callback = NULL;
    node->module_name = NULL;
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
        free(child->name);
        free(child->description);
        free(child->children);
        free(child);
        return;
    }

    // No existing child - add as new
    // Allocate or expand children array
    if (parent->num_children >= parent->children_capacity)
    {
        uint32_t new_capacity =
            parent->children_capacity == 0 ? INITIAL_CHILDREN_CAPACITY : parent->children_capacity * 2;
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
        if (parent->children[i]->name && strcmp(parent->children[i]->name, name) == 0)
        {
            return parent->children[i];
        }
    }

    return NULL;
}

// Find children by partial name match (for TAB completion)
uint32_t nn_cli_tree_find_partial_matches(nn_cli_tree_node_t *parent, const char *partial, nn_cli_tree_node_t **matches,
                                          uint32_t max_matches)
{
    if (!parent || !partial)
    {
        return 0;
    }

    uint32_t count = 0;
    size_t partial_len = strlen(partial);

    for (uint32_t i = 0; i < parent->num_children && count < max_matches; i++)
    {
        nn_cli_tree_node_t *child = parent->children[i];
        if (child->name && strncmp(child->name, partial, partial_len) == 0)
        {
            matches[count++] = child;
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
        free(node->module_name);
        node->module_name = module_name ? strdup(module_name) : NULL;
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

    free(root->children);
    free(root->name);
    free(root->description);
    free(root->module_name);
    free(root);
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
    if (*str == 0)
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
nn_cli_tree_node_t *nn_cli_tree_match_command(nn_cli_tree_node_t *root, const char *cmd_line, char **remaining_args)
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
        free(cmd_copy);
        if (remaining_args)
        {
            *remaining_args = strdup("");
        }
        return root;
    }

    nn_cli_tree_node_t *current = root;
    char *token = strtok(trimmed, " ");
    nn_cli_tree_node_t *last_match = NULL;

    while (token)
    {
        nn_cli_tree_node_t *child = nn_cli_tree_find_child(current, token);

        if (child)
        {
            last_match = child;
            current = child;
            token = strtok(NULL, " ");
        }
        else
        {
            // Check if current node has a parameter child (ARGUMENT type)
            // Parameter nodes accept any input
            nn_cli_tree_node_t *param_child = NULL;
            for (uint32_t i = 0; i < current->num_children; i++)
            {
                if (current->children[i]->type == NN_CLI_NODE_ARGUMENT)
                {
                    param_child = current->children[i];
                    break;
                }
            }

            if (param_child)
            {
                // Found a parameter node, it matches any input
                // Don't consume the token - it's the parameter value
                last_match = param_child;
                current = param_child;
                // Break here to keep token in remaining_args
                break;
            }
            else
            {
                // No exact match and no parameter node found
                break;
            }
        }
    }

    // Set remaining arguments
    if (remaining_args)
    {
        if (token)
        {
            // Reconstruct remaining string
            size_t offset = token - trimmed;
            *remaining_args = strdup(cmd_line + offset);
        }
        else
        {
            *remaining_args = strdup("");
        }
    }

    free(cmd_copy);
    return last_match ? last_match : root;
}

// Print help for a node
void nn_cli_tree_print_help(nn_cli_tree_node_t *node, uint32_t client_fd)
{
    if (!node)
    {
        return;
    }

    char buffer[256];

    if (node->num_children > 0)
    {
        snprintf(buffer, sizeof(buffer), "\r\nAvailable commands:\r\n");
        write(client_fd, buffer, strlen(buffer));

        for (uint32_t i = 0; i < node->num_children; i++)
        {
            nn_cli_tree_node_t *child = node->children[i];
            if (child->name && child->description)
            {
                snprintf(buffer, sizeof(buffer), "  %-15s - %s\r\n", child->name, child->description);
                write(client_fd, buffer, strlen(buffer));
            }
        }
        write(client_fd, "\r\n", 2);
    }
}

// Print tree structure recursively with indentation
void nn_cli_tree_print_structure(nn_cli_tree_node_t *node, uint32_t client_fd, uint32_t indent_level)
{
    if (!node)
    {
        return;
    }

    char buffer[512];
    char indent[128] = "";

    // Create indentation string
    for (uint32_t i = 0; i < indent_level && i < 60; i++)
    {
        strcat(indent, "  ");
    }

    // Print current node
    if (node->name)
    {
        if (node->callback)
        {
            snprintf(buffer, sizeof(buffer), "%s├─ %s (executable)\r\n", indent, node->name);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "%s├─ %s\r\n", indent, node->name);
        }
        write(client_fd, buffer, strlen(buffer));
    }

    // Print children recursively
    for (uint32_t i = 0; i < node->num_children; i++)
    {
        nn_cli_tree_print_structure(node->children[i], client_fd, indent_level + 1);
    }
}

// View to string conversion (for prompts)
const char *nn_cli_view_to_string(nn_cli_view_t view)
{
    switch (view)
    {
        case NN_CLI_VIEW_USER:
            return "user";
        case NN_CLI_VIEW_CONFIG:
            return "config";
        case NN_CLI_VIEW_BGP:
            return "config-bgp";
        case NN_CLI_VIEW_BMP:
            return "config-bmp";
        case NN_CLI_VIEW_RPKI:
            return "config-rpki";
        case NN_CLI_VIEW_ALL:
            return "all";
        default:
            return "unknown";
    }
}
