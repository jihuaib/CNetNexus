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

#include "cli.h"
#include "cli_main.h"
#include "errcode.h"

enum
{
    INITIAL_CHILDREN_CAPACITY = 4
};

// Create a new view node
cli_view_node_t *cli_view_create(const char *view_name, const char *prompt_template)
{
    cli_view_node_t *view = (cli_view_node_t *)g_malloc0(sizeof(cli_view_node_t));

    strlcpy(view->view_name, view_name, CLI_CLI_MAX_VIEW_LEN);
    if (prompt_template != NULL)
    {
        strlcpy(view->prompt_template, prompt_template, CLI_MAX_VIEW_LEN);
    }
    view->cmd_tree = cli_tree_create_node(0, NULL, "Root", CLI_NODE_COMMAND, 0, 0, NULL);

    view->parent = NULL;
    view->children = NULL;
    view->num_children = 0;
    view->children_capacity = 0;

    return view;
}

void cli_view_add_show_this_module(cli_view_node_t *view, uint32_t module_id)
{
    if (!view || module_id == 0)
    {
        return;
    }

    for (uint32_t i = 0; i < view->num_show_this_modules; i++)
    {
        if (view->show_this_module_ids[i] == module_id)
        {
            return;
        }
    }

    if (view->num_show_this_modules >= view->show_this_modules_capacity)
    {
        uint32_t new_capacity =
            (view->show_this_modules_capacity == 0) ? INITIAL_CHILDREN_CAPACITY : view->show_this_modules_capacity * 2;
        uint32_t *new_modules = g_realloc(view->show_this_module_ids, new_capacity * sizeof(uint32_t));
        if (!new_modules)
        {
            return;
        }

        view->show_this_module_ids = new_modules;
        view->show_this_modules_capacity = new_capacity;
    }

    view->show_this_module_ids[view->num_show_this_modules++] = module_id;
}

gboolean cli_view_supports_show_this(const cli_view_node_t *view)
{
    return view && view->num_show_this_modules > 0;
}

const uint32_t *cli_view_get_show_this_modules(const cli_view_node_t *view, uint32_t *count_out)
{
    if (count_out)
    {
        *count_out = view ? view->num_show_this_modules : 0;
    }

    return view ? view->show_this_module_ids : NULL;
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

// 按名称递归查找视图
cli_view_node_t *cli_view_find_by_name(cli_view_node_t *root, const char *view_name)
{
    if (!root || !view_name)
    {
        return NULL;
    }

    if (strcmp(root->view_name, view_name) == 0)
    {
        return root;
    }

    for (uint32_t i = 0; i < root->num_children; i++)
    {
        cli_view_node_t *found = cli_view_find_by_name(root->children[i], view_name);
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
    g_free(view->show_this_module_ids);
    if (view->cmd_tree)
    {
        cli_tree_free(view->cmd_tree);
    }
    g_free(view);
}
