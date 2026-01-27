#ifndef NN_CLI_ELEMENT_H
#define NN_CLI_ELEMENT_H

#include <stdbool.h>
#include <stdint.h>

#include "nn_cli_param_type.h"

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
    element_type_t type;             // Keyword or parameter
    char *name;                      // Element name
    char *description;               // Help text
    char *range;                     // For parameters: validation range (e.g., "1-65535") - deprecated
    nn_cli_param_type_t *param_type; // Parameter type with validation (for ELEMENT_TYPE_PARAMETER)
} nn_cli_element_t;

// Command group
typedef struct
{
    uint32_t group_id;           // Group id
    nn_cli_element_t **elements; // Array of elements
    uint32_t num_elements;       // Number of elements
} nn_cli_command_group_t;

// Function prototypes
nn_cli_element_t *nn_cli_element_create(uint32_t element_id, uint32_t cfg_id, element_type_t type, const char *name,
                                        const char *description, const char *range);
nn_cli_element_t *nn_cli_element_create_with_type(uint32_t element_id, uint32_t cfg_id, element_type_t type,
                                                  const char *name, const char *description, const char *type_str);
void nn_cli_element_free(nn_cli_element_t *element);

// Parameter validation
bool nn_cli_element_validate_param(nn_cli_element_t *element, const char *value, char *error_msg,
                                   uint32_t error_msg_size);

nn_cli_command_group_t *nn_cli_group_create(uint32_t group_id);

void nn_cli_group_add_element(nn_cli_command_group_t *group, nn_cli_element_t *element);

nn_cli_element_t *nn_cli_group_find_element(nn_cli_command_group_t *group, uint32_t id);

void nn_cli_group_free(nn_cli_command_group_t *group);

#endif // NN_CLI_ELEMENT_H
