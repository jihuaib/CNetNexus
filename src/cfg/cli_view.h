/**
 * @file   cli_view.h
 * @brief  CLI 视图层级管理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_VIEW_H
#define CLI_VIEW_H

#include "cli.h"
#include "cli_tree.h"

// Forward declaration
typedef struct cli_view_node cli_view_node_t;

// View node structure - represents a CLI view with its command tree
struct cli_view_node
{
    uint32_t view_id;
    char view_name[CLI_CLI_MAX_VIEW_LEN];
    char prompt_template[CFG_CLI_MAX_VIEW_LEN];
    cli_tree_node_t *cmd_tree; // Command tree for this view

    // View hierarchy
    cli_view_node_t *parent;    // Parent view (NULL for root)
    cli_view_node_t **children; // Child views
    uint32_t num_children;
    uint32_t children_capacity;
};

// View tree container
typedef struct
{
    cli_view_node_t *root;        // Root view (usually "user")
    cli_view_node_t *global_view; // Global commands view
} cli_view_tree_t;

// Function prototypes
cli_view_node_t *cli_view_create(uint32_t view_id, const char *view_name, const char *prompt_template);

void cli_view_add_child(cli_view_node_t *parent, cli_view_node_t *child);

cli_view_node_t *cli_view_find_by_id(cli_view_node_t *root, uint32_t view_id);

void cli_view_free(cli_view_node_t *view);

int cfg_get_view_prompt_template_inner(uint32_t view_id, char *view_name);

#endif // CLI_VIEW_H
