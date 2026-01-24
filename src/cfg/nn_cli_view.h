#ifndef NN_CLI_VIEW_H
#define NN_CLI_VIEW_H

#include "basetype.h"
#include "nn_cli_tree.h"

// Forward declaration
typedef struct nn_cli_view_node nn_cli_view_node_t;

// View node structure - represents a CLI view with its command tree
struct nn_cli_view_node
{
    char *name;                   // View name (e.g., "user", "config")
    char *prompt_template;        // Prompt template (e.g., "<NetNexus>")
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
nn_cli_view_node_t *nn_cli_view_create(const char *name, const char *prompt_template);
void nn_cli_view_add_child(nn_cli_view_node_t *parent, nn_cli_view_node_t *child);
nn_cli_view_node_t *nn_cli_view_find_by_name(nn_cli_view_node_t *root, const char *name);
void nn_cli_view_free(nn_cli_view_node_t *view);

#endif // NN_CLI_VIEW_H
