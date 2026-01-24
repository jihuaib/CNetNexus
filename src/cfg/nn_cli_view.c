#include "nn_cli_view.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "nn_errcode.h"

enum
{
    INITIAL_CHILDREN_CAPACITY = 4
};

// Create a new view node
nn_cli_view_node_t *nn_cli_view_create(const char *name, const char *prompt_template)
{
    nn_cli_view_node_t *view = (nn_cli_view_node_t *)g_malloc(sizeof(nn_cli_view_node_t));

    view->name = name ? strdup(name) : NULL;
    view->prompt_template = prompt_template ? strdup(prompt_template) : NULL;
    view->cmd_tree = nn_cli_tree_create_node(NULL, "Root", NN_CLI_NODE_COMMAND);

    view->parent = NULL;
    view->children = NULL;
    view->num_children = 0;
    view->children_capacity = 0;

    return view;
}

// Add a child view to a parent view
void nn_cli_view_add_child(nn_cli_view_node_t *parent, nn_cli_view_node_t *child)
{
    if (!parent || !child)
    {
        return;
    }

    // Allocate or expand children array
    if (parent->num_children >= parent->children_capacity)
    {
        uint32_t new_capacity =
            parent->children_capacity == NN_ERRCODE_SUCCESS ? INITIAL_CHILDREN_CAPACITY : parent->children_capacity * 2;
        nn_cli_view_node_t **new_children =
            (nn_cli_view_node_t **)realloc(parent->children, new_capacity * sizeof(nn_cli_view_node_t *));
        if (!new_children)
        {
            return;
        }

        parent->children = new_children;
        parent->children_capacity = new_capacity;
    }

    parent->children[parent->num_children++] = child;
    child->parent = parent;
}

// Find a view by name (recursive search)
nn_cli_view_node_t *nn_cli_view_find_by_name(nn_cli_view_node_t *root, const char *name)
{
    if (!root || !name)
    {
        return NULL;
    }

    // Check current node
    if (root->name && strcmp(root->name, name) == NN_ERRCODE_SUCCESS)
    {
        return root;
    }

    // Search children recursively
    for (uint32_t i = 0; i < root->num_children; i++)
    {
        nn_cli_view_node_t *found = nn_cli_view_find_by_name(root->children[i], name);
        if (found)
        {
            return found;
        }
    }

    return NULL;
}

// Free a view node and all its children
void nn_cli_view_free(nn_cli_view_node_t *view)
{
    if (!view)
    {
        return;
    }

    // Free all children recursively
    for (uint32_t i = 0; i < view->num_children; i++)
    {
        nn_cli_view_free(view->children[i]);
    }

    g_free(view->children);
    g_free(view->name);
    g_free(view->prompt_template);
    if (view->cmd_tree)
    {
        nn_cli_tree_free(view->cmd_tree);
    }
    g_free(view);
}
