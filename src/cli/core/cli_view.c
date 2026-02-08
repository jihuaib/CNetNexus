/**
 * @file   cli_view.c
 * @brief  CLI 视图层级管理
 * @author jhb
 * @date   2026/01/22
 */
#include "cli_view.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "errcode.h"

enum
{
    INITIAL_CHILDREN_CAPACITY = 4
};

// Create a new view node
cli_view_node_t *cli_view_create(uint32_t view_id, const char *view_name, const char *prompt_template)
{
    cli_view_node_t *view = (cli_view_node_t *)g_malloc0(sizeof(cli_view_node_t));

    view->view_id = view_id;
    strlcpy(view->view_name, view_name, CFG_CLI_MAX_VIEW_NAME_LEN);
    if (prompt_template != NULL)
    {
        strlcpy(view->prompt_template, prompt_template, CFG_CLI_MAX_VIEW_LEN);
    }
    view->cmd_tree = cli_tree_create_node(0, NULL, "Root", CLI_NODE_COMMAND, 0, 0, 0);

    view->parent = NULL;
    view->children = NULL;
    view->num_children = 0;
    view->children_capacity = 0;

    return view;
}

// Add a child view to a parent view
void cli_view_add_child(cli_view_node_t *parent, cli_view_node_t *child)
{
    if (!parent || !child)
    {
        return;
    }

    // Allocate or expand children array
    if (parent->num_children >= parent->children_capacity)
    {
        uint32_t new_capacity =
            parent->children_capacity == ERRCODE_SUCCESS ? INITIAL_CHILDREN_CAPACITY : parent->children_capacity * 2;
        cli_view_node_t **new_children =
            (cli_view_node_t **)realloc(parent->children, new_capacity * sizeof(cli_view_node_t *));
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
cli_view_node_t *cli_view_find_by_id(cli_view_node_t *root, uint32_t view_id)
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
        cli_view_node_t *found = cli_view_find_by_id(root->children[i], view_id);
        if (found)
        {
            return found;
        }
    }

    return NULL;
}

// Free a view node and all its children
void cli_view_free(cli_view_node_t *view)
{
    if (!view)
    {
        return;
    }

    // Free all children recursively
    for (uint32_t i = 0; i < view->num_children; i++)
    {
        cli_view_free(view->children[i]);
    }

    g_free(view->children);
    if (view->cmd_tree)
    {
        cli_tree_free(view->cmd_tree);
    }
    g_free(view);
}
