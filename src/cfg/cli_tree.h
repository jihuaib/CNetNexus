/**
 * @file   cli_tree.h
 * @brief  CLI 命令树匹配和补全头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_TREE_H
#define CLI_TREE_H

#include <glib.h>
#include <stdint.h>

// Forward declaration
typedef struct cli_tree_node cli_tree_node_t;

// Node types
typedef enum
{
    CLI_NODE_COMMAND,  // Command keyword (e.g., "show", "exit")
    CLI_NODE_ARGUMENT, // Command argument (e.g., IP address, number)
} cli_node_type_t;

// Forward declaration
typedef struct cli_param_type cli_param_type_t;

// CLI tree node structure
struct cli_tree_node
{
    uint32_t cfg_id;    // Element ID from XML definition
    uint32_t module_id; // Associated module ID for message dispatch
    uint32_t group_id;
    char *name;                   // Node name/keyword
    char *description;            // Help text
    cli_node_type_t type;         // Node type
    uint32_t view_id;             // Target view name to switch to after execution (optional)
    cli_param_type_t *param_type; // Parameter type for validation (only for ARGUMENT nodes)
    char *db_name;                // 关联数据库名称（从 group 继承到 end_node）
    char *table_name;             // 关联表名称（从 group 继承到 end_node）
    char *show_template;          // Show 命令显示模板名称（可选）
    char *field_name;             // DB 字段名（仅参数节点）
    gboolean is_end_node;         // 1 if this node is a valid command end point, 0 otherwise

    // Children nodes
    cli_tree_node_t **children; // Array of child nodes
    uint32_t num_children;      // Number of children
    uint32_t children_capacity; // Allocated capacity
};

// Command match element - stores matched element info with value
typedef struct cli_match_element
{
    uint32_t cfg_id;              // Cfg ID
    cli_node_type_t type;         // COMMAND (keyword) or ARGUMENT
    char *value;                  // Argument value (NULL for keywords)
    uint32_t value_len;           // Value length (binary length for TLV)
    cli_param_type_t *param_type; // Parameter type (for conversion during TLV packing)
    char *field_name;             // DB 字段名（仅参数节点）
} cli_match_element_t;

// Command match result - stores all matched elements along the path
typedef struct cli_match_result
{
    uint32_t module_id; // Target module ID
    uint32_t group_id;
    char *db_name;                 // 关联数据库名称
    char *table_name;              // 关联表名称
    char *show_template;           // Show 命令显示模板名称（可选）
    cli_match_element_t *elements; // Array of matched elements
    uint32_t num_elements;         // Number of elements
    uint32_t capacity;             // Allocated capacity
    cli_tree_node_t *final_node;   // Final matched node
} cli_match_result_t;

// Match result functions
cli_match_result_t *cli_match_result_create(void);
void cli_match_result_add_element(cli_match_result_t *result, uint32_t element_id, cli_node_type_t type,
                                  const char *value, cli_param_type_t *param_type);
void cli_match_result_free(cli_match_result_t *result);

// Extended command matching - returns match result with all elements
cli_match_result_t *cli_tree_match_command_full(cli_tree_node_t *root, const char *cmd_line);

// Function prototypes
cli_tree_node_t *cli_tree_create_node(uint32_t element_id, const char *name, const char *description,
                                      cli_node_type_t type, uint32_t module_id, uint32_t group_id, uint32_t view_id);

void cli_tree_add_child(cli_tree_node_t *parent, cli_tree_node_t *child);

cli_tree_node_t *cli_tree_find_child(cli_tree_node_t *parent, const char *name);

void cli_tree_set_param_type(cli_tree_node_t *node, cli_param_type_t *param_type);

void cli_tree_free(cli_tree_node_t *root);

cli_tree_node_t *cli_tree_clone(cli_tree_node_t *node); // Clone a tree node and its children

// Command matching
cli_tree_node_t *cli_tree_find_child_input_token(cli_tree_node_t *parent, const char *token);

uint32_t cli_tree_find_children_input_token(cli_tree_node_t *parent, const char *token, cli_tree_node_t **matches,
                                            uint32_t max_matches);

cli_tree_node_t *cli_tree_match_command(cli_tree_node_t *root, const char *cmd_line);

uint32_t cli_tree_match_command_get_matches(cli_tree_node_t *root, const char *cmd_line, cli_tree_node_t **matches,
                                            uint32_t max_matches);

#endif // cli_TREE_H
