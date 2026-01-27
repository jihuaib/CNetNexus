#include "nn_cli_element.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nn_cli_param_type.h"

// Create a CLI element
nn_cli_element_t *nn_cli_element_create(uint32_t element_id, uint32_t cfg_id, element_type_t type, const char *name,
                                        const char *description, const char *range)
{
    nn_cli_element_t *element = (nn_cli_element_t *)g_malloc0(sizeof(nn_cli_element_t));

    element->element_id = element_id;
    element->cfg_id = cfg_id;
    element->type = type;
    element->name = name ? strdup(name) : NULL;
    element->description = description ? strdup(description) : NULL;
    element->range = range ? strdup(range) : NULL;
    element->param_type = NULL;

    return element;
}

// Create a CLI element with type string parsing
nn_cli_element_t *nn_cli_element_create_with_type(uint32_t element_id, uint32_t cfg_id, element_type_t type,
                                                  const char *name, const char *description, const char *type_str)
{
    nn_cli_element_t *element = (nn_cli_element_t *)g_malloc0(sizeof(nn_cli_element_t));

    element->element_id = element_id;
    element->cfg_id = cfg_id;
    element->type = type;
    element->name = name ? strdup(name) : NULL;
    element->description = description ? strdup(description) : NULL;
    element->range = NULL;

    // Parse type string to create param_type
    if (type == ELEMENT_TYPE_PARAMETER && type_str)
    {
        element->param_type = nn_cli_param_type_parse(type_str);
    }
    else
    {
        element->param_type = NULL;
    }

    return element;
}

// Free a CLI element
void nn_cli_element_free(nn_cli_element_t *element)
{
    if (!element)
    {
        return;
    }

    g_free(element->name);
    g_free(element->description);
    g_free(element->range);

    if (element->param_type)
    {
        nn_cli_param_type_free(element->param_type);
    }

    g_free(element);
}

// Create a command group
nn_cli_command_group_t *nn_cli_group_create(uint32_t group_id)
{
    nn_cli_command_group_t *group = (nn_cli_command_group_t *)g_malloc0(sizeof(nn_cli_command_group_t));

    group->group_id = group_id;
    group->elements = NULL;
    group->num_elements = 0;

    return group;
}

// Add element to group
void nn_cli_group_add_element(nn_cli_command_group_t *group, nn_cli_element_t *element)
{
    if (!group || !element)
    {
        return;
    }

    group->elements =
        (nn_cli_element_t **)realloc(group->elements, (group->num_elements + 1) * sizeof(nn_cli_element_t *));
    if (!group->elements)
    {
        return;
    }

    group->elements[group->num_elements++] = element;
}

// Find element by ID in group
nn_cli_element_t *nn_cli_group_find_element(nn_cli_command_group_t *group, uint32_t element_id)
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
void nn_cli_group_free(nn_cli_command_group_t *group)
{
    if (!group)
    {
        return;
    }

    for (uint32_t i = 0; i < group->num_elements; i++)
    {
        nn_cli_element_free(group->elements[i]);
    }

    g_free(group->elements);
    g_free(group);
}

// Validate a parameter value against element's type definition
bool nn_cli_element_validate_param(nn_cli_element_t *element, const char *value, char *error_msg,
                                   uint32_t error_msg_size)
{
    if (!element || !value)
    {
        if (error_msg && error_msg_size > 0)
        {
            snprintf(error_msg, error_msg_size, "Invalid element or value");
        }
        return false;
    }

    // Keywords don't need validation
    if (element->type == ELEMENT_TYPE_KEYWORD)
    {
        return true;
    }

    // If no param_type defined, accept any value
    if (!element->param_type)
    {
        return true;
    }

    return nn_cli_param_type_validate(element->param_type, value, error_msg, error_msg_size);
}
