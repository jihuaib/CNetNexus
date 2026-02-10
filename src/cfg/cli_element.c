/**
 * @file   cli_element.c
 * @brief  CLI 元素处理，管理关键字和参数元素
 * @author jhb
 * @date   2026/01/22
 */
#include "cli_element.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_param_type.h"

// Create a CLI element
cli_element_t *cli_element_create(uint32_t element_id, uint32_t cfg_id, element_type_t type, const char *name,
                                  const char *description, const char *range)
{
    cli_element_t *element = (cli_element_t *)g_malloc0(sizeof(cli_element_t));

    element->element_id = element_id;
    element->cfg_id = cfg_id;
    element->type = type;
    element->name = name ? g_strdup(name) : NULL;
    element->description = description ? g_strdup(description) : NULL;
    element->range = range ? g_strdup(range) : NULL;
    element->param_type = NULL;

    return element;
}

// Create a CLI element with type string parsing
cli_element_t *cli_element_create_with_type(uint32_t element_id, uint32_t cfg_id, element_type_t type, const char *name,
                                            const char *description, const char *type_str)
{
    cli_element_t *element = (cli_element_t *)g_malloc0(sizeof(cli_element_t));

    element->element_id = element_id;
    element->cfg_id = cfg_id;
    element->type = type;
    element->name = name ? g_strdup(name) : NULL;
    element->description = description ? g_strdup(description) : NULL;
    element->range = NULL;

    // Parse type string to create param_type
    if (type == ELEMENT_TYPE_PARAMETER && type_str)
    {
        element->param_type = cli_param_type_parse(type_str);
    }
    else
    {
        element->param_type = NULL;
    }

    return element;
}

// Free a CLI element
void cli_element_free(cli_element_t *element)
{
    if (!element)
    {
        return;
    }

    g_free(element->name);
    g_free(element->description);
    g_free(element->range);
    g_free(element->field_name);

    if (element->param_type)
    {
        cli_param_type_free(element->param_type);
    }

    g_free(element);
}

// Create a command group
cli_command_group_t *cli_group_create(uint32_t group_id)
{
    cli_command_group_t *group = (cli_command_group_t *)g_malloc0(sizeof(cli_command_group_t));

    group->group_id = group_id;
    group->elements = NULL;
    group->num_elements = 0;

    return group;
}

// Add element to group
void cli_group_add_element(cli_command_group_t *group, cli_element_t *element)
{
    if (!group || !element)
    {
        return;
    }

    group->elements = (cli_element_t **)realloc(group->elements, (group->num_elements + 1) * sizeof(cli_element_t *));
    if (!group->elements)
    {
        return;
    }

    group->elements[group->num_elements++] = element;
}

// Find element by ID in group
cli_element_t *cli_group_find_element(cli_command_group_t *group, uint32_t element_id)
{
    if (!group)
    {
        return NULL;
    }

    for (uint32_t i = 0; i < group->num_elements; i++)
    {
        if (group->elements[i]->element_id == element_id)
        {
            return group->elements[i];
        }
    }

    return NULL;
}

// Free a command group
void cli_group_free(cli_command_group_t *group)
{
    if (!group)
    {
        return;
    }

    for (uint32_t i = 0; i < group->num_elements; i++)
    {
        cli_element_free(group->elements[i]);
    }

    g_free(group->elements);
    g_free(group->db_name);
    g_free(group->table_name);
    g_free(group);
}

// Validate a parameter value against element's type definition
gboolean cli_element_validate_param(cli_element_t *element, const char *value, char *error_msg, uint32_t error_msg_size)
{
    if (!element || !value)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Invalid element or value");
        }
        return FALSE;
    }

    // Keywords don't need validation
    if (element->type == ELEMENT_TYPE_KEYWORD)
    {
        return TRUE;
    }

    // If no param_type defined, accept any value
    if (!element->param_type)
    {
        return TRUE;
    }

    return cli_param_type_validate(element->param_type, value, error_msg, error_msg_size);
}
