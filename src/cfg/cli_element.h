/**
 * @file   cli_element.h
 * @brief  CLI 元素处理头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_ELEMENT_H
#define CLI_ELEMENT_H

#include <stdint.h>

#include "cli_param_type.h"

// Element types
typedef enum
{
    ELEMENT_TYPE_KEYWORD,  // Fixed keyword (e.g., "show", "config")
    ELEMENT_TYPE_PARAMETER // Variable parameter (e.g., <as-number>, <ip-address>)
} element_type_t;

// CLI element definition
typedef struct
{
    uint32_t element_id; // Unique element ID
    uint32_t cfg_id;
    element_type_t type;          // Keyword or parameter
    char *name;                   // Element name
    char *description;            // Help text
    char *range;                  // For parameters: validation range (e.g., "1-65535") - deprecated
    cli_param_type_t *param_type; // Parameter type with validation (for ELEMENT_TYPE_PARAMETER)
    char *field_name;             // DB 字段名称（仅参数类型），如 "as_number"
} cli_element_t;

// Command group
typedef struct
{
    uint32_t group_id;        // Group id
    cli_element_t **elements; // Array of elements
    uint32_t num_elements;    // Number of elements
    char *db_name;            // 关联数据库名称，如 "bgp_db"
    char *table_name;         // 关联表名称，如 "bgp_protocol"
} cli_command_group_t;

// Function prototypes
cli_element_t *cli_element_create(uint32_t element_id, uint32_t cfg_id, element_type_t type, const char *name,
                                  const char *description, const char *range);
cli_element_t *cli_element_create_with_type(uint32_t element_id, uint32_t cfg_id, element_type_t type, const char *name,
                                            const char *description, const char *type_str);
void cli_element_free(cli_element_t *element);

// Parameter validation
gboolean cli_element_validate_param(cli_element_t *element, const char *value, char *error_msg,
                                    uint32_t error_msg_size);

cli_command_group_t *cli_group_create(uint32_t group_id);

void cli_group_add_element(cli_command_group_t *group, cli_element_t *element);

cli_element_t *cli_group_find_element(cli_command_group_t *group, uint32_t id);

void cli_group_free(cli_command_group_t *group);

#endif // CLI_ELEMENT_H
