/**
 * @file   nn_cli_view.h
 * @brief  CLI 视图层级管理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef NN_CLI_VIEW_H
#define NN_CLI_VIEW_H

#include "nn_cfg.h"
#include "nn_cli_tree.h"

// Forward declaration
typedef struct nn_cli_view_node nn_cli_view_node_t;

// View node structure - represents a CLI view with its command tree
struct nn_cli_view_node
{
    uint32_t view_id;
    char view_name[NN_CFG_CLI_MAX_VIEW_NAME_LEN];
    char prompt_template[NN_CFG_CLI_MAX_VIEW_LEN];
    nn_cli_tree_node_t *cmd_tree; // Command tree for this view

    // View hierarchy
    nn_cli_view_node_t *parent;    // Parent view (NULL for root)
    nn_cli_view_node_t **children; // Child views
    uint32_t num_children;
    uint32_t children_capacity;
};

// View tree container
typedef struct
{
    nn_cli_view_node_t *root;        // Root view (usually "user")
    nn_cli_view_node_t *global_view; // Global commands view
} nn_cli_view_tree_t;

// Function prototypes
nn_cli_view_node_t *nn_cli_view_create(uint32_t view_id, const char *view_name, const char *prompt_template);

void nn_cli_view_add_child(nn_cli_view_node_t *parent, nn_cli_view_node_t *child);

nn_cli_view_node_t *nn_cli_view_find_by_id(nn_cli_view_node_t *root, uint32_t view_id);

void nn_cli_view_free(nn_cli_view_node_t *view);

int nn_cfg_get_view_prompt_template_inner(uint32_t view_id, char *view_name);

#endif // NN_CLI_VIEW_H
