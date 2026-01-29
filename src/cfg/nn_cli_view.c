#include "nn_cli_view.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "nn_cfg.h"
#include "nn_cfg_main.h"
#include "nn_errcode.h"

enum
{
    INITIAL_CHILDREN_CAPACITY = 4
};

// Create a new view node
nn_cli_view_node_t *nn_cli_view_create(uint32_t view_id, const char *view_name, const char *prompt_template)
{
    nn_cli_view_node_t *view = (nn_cli_view_node_t *)g_malloc0(sizeof(nn_cli_view_node_t));

    view->view_id = view_id;
    strlcpy(view->view_name, view_name, NN_CFG_CLI_MAX_VIEW_NAME_LEN);
    if (prompt_template != NULL)
    {
        strlcpy(view->prompt_template, prompt_template, NN_CFG_CLI_MAX_VIEW_LEN);
    }
    view->cmd_tree = nn_cli_tree_create_node(0, NULL, "Root", NN_CLI_NODE_COMMAND, 0, 0, 0);

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
nn_cli_view_node_t *nn_cli_view_find_by_id(nn_cli_view_node_t *root, uint32_t view_id)
{
    if (!root)
    {
        return NULL;
    }

    // Check current node
    if (root->view_id == view_id)
    {
        return root;
    }

    // Search children recursively
    for (uint32_t i = 0; i < root->num_children; i++)
    {
        nn_cli_view_node_t *found = nn_cli_view_find_by_id(root->children[i], view_id);
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
    if (view->cmd_tree)
    {
        nn_cli_tree_free(view->cmd_tree);
    }
    g_free(view);
}

// Get view prompt template by view name (for modules to fill placeholders)
int nn_cfg_get_view_prompt_template_inner(uint32_t view_id, char *view_name)
{
    view_name[0] = '\0';

    if (!g_nn_cfg_local->view_tree.root)
    {
        return NN_ERRCODE_FAIL;
        ;
    }

    nn_cli_view_node_t *view = nn_cli_view_find_by_id(g_nn_cfg_local->view_tree.root, view_id);
    if (view)
    {
        strlcpy(view_name, view->prompt_template, NN_CFG_CLI_MAX_VIEW_LEN);
        return NN_ERRCODE_SUCCESS;
    }

    return NN_ERRCODE_FAIL;
}
