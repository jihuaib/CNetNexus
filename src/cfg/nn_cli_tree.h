#ifndef NN_CLI_TREE_H
#define NN_CLI_TREE_H

#include "basetype.h"

// Forward declaration
typedef struct nn_cli_tree_node nn_cli_tree_node_t;

// View types
typedef enum
{
    NN_CLI_VIEW_USER,   // Default user view
    NN_CLI_VIEW_CONFIG, // Configuration view
    NN_CLI_VIEW_BGP,    // BGP configuration view
    NN_CLI_VIEW_BMP,    // BMP configuration view
    NN_CLI_VIEW_RPKI,   // RPKI configuration view
    NN_CLI_VIEW_ALL     // Available in all views (global commands)
} nn_cli_view_t;

// Node types
typedef enum
{
    NN_CLI_NODE_COMMAND,  // Command keyword (e.g., "show", "exit")
    NN_CLI_NODE_ARGUMENT, // Command argument (e.g., IP address, number)
} nn_cli_node_type_t;

// Command callback function type
typedef void (*nn_cli_callback_t)(uint32_t client_fd, const char *args);

// Forward declaration
typedef struct nn_cli_param_type nn_cli_param_type_t;

// CLI tree node structure
struct nn_cli_tree_node
{
    char *name;                      // Node name/keyword
    char *description;               // Help text
    nn_cli_node_type_t type;         // Node type
    nn_cli_callback_t callback;      // Command callback (NULL for intermediate nodes)
    char *module_name;               // Associated module name for message dispatch (optional)
    nn_cli_param_type_t *param_type; // Parameter type for validation (only for ARGUMENT nodes)

    // Children nodes
    nn_cli_tree_node_t **children; // Array of child nodes
    uint32_t num_children;         // Number of children
    uint32_t children_capacity;    // Allocated capacity
};

// Function prototypes
nn_cli_tree_node_t *nn_cli_tree_create_node(const char *name, const char *description, nn_cli_node_type_t type);

void nn_cli_tree_add_child(nn_cli_tree_node_t *parent, nn_cli_tree_node_t *child);

nn_cli_tree_node_t *nn_cli_tree_find_child(nn_cli_tree_node_t *parent, const char *name);

void nn_cli_tree_set_callback(nn_cli_tree_node_t *node, nn_cli_callback_t callback);

void nn_cli_tree_set_module_name(nn_cli_tree_node_t *node, const char *module_name);

void nn_cli_tree_set_param_type(nn_cli_tree_node_t *node, nn_cli_param_type_t *param_type);

void nn_cli_tree_free(nn_cli_tree_node_t *root);

nn_cli_tree_node_t *nn_cli_tree_clone(nn_cli_tree_node_t *node); // Clone a tree node and its children

// Command matching
nn_cli_tree_node_t *nn_cli_tree_find_child_input_token(nn_cli_tree_node_t *parent, const char *token);

uint32_t nn_cli_tree_find_children_input_token(nn_cli_tree_node_t *parent, const char *token,
                                               nn_cli_tree_node_t **matches, uint32_t max_matches);

nn_cli_tree_node_t *nn_cli_tree_match_command(nn_cli_tree_node_t *root, const char *cmd_line);

uint32_t nn_cli_tree_match_command_get_matches(nn_cli_tree_node_t *root, const char *cmd_line,
                                               nn_cli_tree_node_t **matches, uint32_t max_matches);
void nn_cli_tree_print_help(nn_cli_tree_node_t *node, uint32_t client_fd);

#endif // nn_cli_TREE_H
