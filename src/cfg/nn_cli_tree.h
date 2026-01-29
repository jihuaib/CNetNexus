#ifndef NN_CLI_TREE_H
#define NN_CLI_TREE_H

#include <stdint.h>
#include <glib.h>

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

// Forward declaration
typedef struct nn_cli_param_type nn_cli_param_type_t;

// CLI tree node structure
struct nn_cli_tree_node
{
    uint32_t cfg_id;    // Element ID from XML definition
    uint32_t module_id; // Associated module ID for message dispatch
    uint32_t group_id;
    char *name;                      // Node name/keyword
    char *description;               // Help text
    nn_cli_node_type_t type;         // Node type
    uint32_t view_id;                // Target view name to switch to after execution (optional)
    nn_cli_param_type_t *param_type; // Parameter type for validation (only for ARGUMENT nodes)
    gboolean is_end_node;                // 1 if this node is a valid command end point, 0 otherwise

    // Children nodes
    nn_cli_tree_node_t **children; // Array of child nodes
    uint32_t num_children;         // Number of children
    uint32_t children_capacity;    // Allocated capacity
};

// Command match element - stores matched element info with value
typedef struct nn_cli_match_element
{
    uint32_t cfg_id;                 // Cfg ID
    nn_cli_node_type_t type;         // COMMAND (keyword) or ARGUMENT
    char *value;                     // Argument value (NULL for keywords)
    uint32_t value_len;              // Value length (binary length for TLV)
    nn_cli_param_type_t *param_type; // Parameter type (for conversion during TLV packing)
} nn_cli_match_element_t;

// Command match result - stores all matched elements along the path
typedef struct nn_cli_match_result
{
    uint32_t module_id; // Target module ID
    uint32_t group_id;
    nn_cli_match_element_t *elements; // Array of matched elements
    uint32_t num_elements;            // Number of elements
    uint32_t capacity;                // Allocated capacity
    nn_cli_tree_node_t *final_node;   // Final matched node
} nn_cli_match_result_t;

// Match result functions
nn_cli_match_result_t *nn_cli_match_result_create(void);
void nn_cli_match_result_add_element(nn_cli_match_result_t *result, uint32_t element_id, nn_cli_node_type_t type,
                                     const char *value, nn_cli_param_type_t *param_type);
void nn_cli_match_result_free(nn_cli_match_result_t *result);

// Extended command matching - returns match result with all elements
nn_cli_match_result_t *nn_cli_tree_match_command_full(nn_cli_tree_node_t *root, const char *cmd_line);

// Function prototypes
nn_cli_tree_node_t *nn_cli_tree_create_node(uint32_t element_id, const char *name, const char *description,
                                            nn_cli_node_type_t type, uint32_t module_id, uint32_t group_id,
                                            uint32_t view_id);

void nn_cli_tree_add_child(nn_cli_tree_node_t *parent, nn_cli_tree_node_t *child);

nn_cli_tree_node_t *nn_cli_tree_find_child(nn_cli_tree_node_t *parent, const char *name);

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

#endif // nn_cli_TREE_H
