#include "nn_cli_element.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

// Create a CLI element
nn_cli_element_t *nn_cli_element_create(uint32_t id, element_type_t type, const char *name, const char *description,
                                        const char *range)
{
    nn_cli_element_t *element = (nn_cli_element_t *)g_malloc(sizeof(nn_cli_element_t));

    element->id = id;
    element->type = type;
    element->name = name ? strdup(name) : NULL;
    element->description = description ? strdup(description) : NULL;
    element->range = range ? strdup(range) : NULL;

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
    g_free(element);
}

// Create a command group
nn_cli_command_group_t *nn_cli_group_create(const char *name)
{
    nn_cli_command_group_t *group = (nn_cli_command_group_t *)g_malloc(sizeof(nn_cli_command_group_t));

    group->name = name ? strdup(name) : NULL;
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
nn_cli_element_t *nn_cli_group_find_element(nn_cli_command_group_t *group, uint32_t id)
{
    if (!group)
    {
        return NULL;
    }

    for (uint32_t i = 0; i < group->num_elements; i++)
    {
        if (group->elements[i]->id == id)
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
    g_free(group->name);
    g_free(group);
}
