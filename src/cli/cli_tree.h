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

/**
 * @brief 命令切换视图时自动写入的上下文条目（来自 XML <context-out>）
 */
typedef struct
{
    uint32_t ctx_id; /**< 上下文变量 ID（独立命名空间，与 XML cfg-id 无关） */
    uint32_t from_param; /**< 有效值: 从匹配命令参数中 cfg_id==from_param 的元素取值；0xFFFFFFFF: 使用 fixed_value */
    uint32_t fixed_value; /**< from_param==0xFFFFFFFF 时的固定无符号整数值 */
} cli_ctx_out_entry_t;

/**
 * @brief 同名共享节点下，不同命令来源（module/group）到 cfg-id 的绑定
 */
typedef struct
{
    uint32_t module_id;
    uint32_t group_id;
    uint32_t cfg_id;
} cli_cfg_binding_t;

// CLI tree node structure
struct cli_tree_node
{
    uint32_t cfg_id;    // Element ID from XML definition
    uint32_t module_id; // Associated module ID for message dispatch
    uint32_t group_id;
    char *name;                   // Node name/keyword
    char *description;            // Help text
    cli_node_type_t type;         // Node type
    char *target_view_name;       // 命令执行成功后切换到的视图名称（NULL 表示不切换）
    cli_param_type_t *param_type; // Parameter type for validation (only for ARGUMENT nodes)
    gboolean is_end_node;         // 1 if this node is a valid command end point, 0 otherwise

    /** XML <context-out> 条目数组：视图切换成功时由框架自动写入上下文（NULL 表示无） */
    cli_ctx_out_entry_t *context_out;
    uint32_t num_context_out;

    /** 同名节点合并后保留每个 (module_id, group_id) 对应的 cfg_id */
    cli_cfg_binding_t *cfg_bindings;
    uint32_t num_cfg_bindings;
    uint32_t cfg_bindings_capacity;

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
} cli_match_element_t;

// Command match result - stores all matched elements along the path
typedef struct cli_match_result
{
    uint32_t module_id; // Target module ID
    uint32_t group_id;
    cli_match_element_t *elements; // Array of matched elements
    uint32_t num_elements;         // Number of elements
    uint32_t capacity;             // Allocated capacity
    cli_tree_node_t *final_node;   // Final matched node
    gboolean has_no_prefix;        // 命令是否带有 "no" 前缀（无需 cfg-id 亦可检测）
    gboolean has_show_prefix;      // 命令是否带有 "show" 关键字
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
                                      cli_node_type_t type, uint32_t module_id, uint32_t group_id,
                                      const char *target_view_name);

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

// 双树匹配：同时在 view_root 和 global_root 两棵树中查找，结果合并去重
uint32_t cli_tree_match_command_get_matches_dual(cli_tree_node_t *view_root, cli_tree_node_t *global_root,
                                                 const char *cmd_line, cli_tree_node_t **matches, uint32_t max_matches);

// 双树全量匹配：先尝试 view_root，失败后回退 global_root
cli_match_result_t *cli_tree_match_command_full_dual(cli_tree_node_t *view_root, cli_tree_node_t *global_root,
                                                     const char *cmd_line);

#endif // cli_TREE_H
