#ifndef nn_cli_ELEMENT_H
#define nn_cli_ELEMENT_H

#include <stdint.h>

// Element types
typedef enum
{
    ELEMENT_TYPE_KEYWORD,  // Fixed keyword (e.g., "show", "configure")
    ELEMENT_TYPE_PARAMETER // Variable parameter (e.g., <as-number>, <ip-address>)
} element_type_t;

// CLI element definition
typedef struct
{
    uint32_t id;         // Unique element ID
    element_type_t type; // Keyword or parameter
    char *name;          // Element name
    char *description;   // Help text
    char *range;         // For parameters: validation range (e.g., "1-65535")
} nn_cli_element_t;

// Command group
typedef struct
{
    char *name;                  // Group name (e.g., "basic", "bgp")
    nn_cli_element_t **elements; // Array of elements
    uint32_t num_elements;       // Number of elements
} nn_cli_command_group_t;

// Function prototypes
nn_cli_element_t *nn_cli_element_create(uint32_t id, element_type_t type, const char *name, const char *description,
                                        const char *range);
void nn_cli_element_free(nn_cli_element_t *element);

nn_cli_command_group_t *nn_cli_group_create(const char *name);
void nn_cli_group_add_element(nn_cli_command_group_t *group, nn_cli_element_t *element);
nn_cli_element_t *nn_cli_group_find_element(nn_cli_command_group_t *group, uint32_t id);
void nn_cli_group_free(nn_cli_command_group_t *group);

#endif // nn_cli_ELEMENT_H
