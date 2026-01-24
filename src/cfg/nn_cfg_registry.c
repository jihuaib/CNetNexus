#include "nn_cfg_registry.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

// Global XML registry using GLib's singly-linked list
GSList *g_xml_registry = NULL;

// Register module's XML path by module ID
void nn_cfg_register_module_xml(uint32_t module_id, const char *xml_path)
{
    if (!xml_path)
    {
        return;
    }

    nn_cfg_xml_entry_t *entry = g_malloc(sizeof(nn_cfg_xml_entry_t));
    entry->module_id = module_id;
    entry->xml_path = g_strdup(xml_path);

    g_xml_registry = g_slist_prepend(g_xml_registry, entry);

    printf("[cfg] Registered XML for module ID %u -> %s\n", module_id, xml_path);
}

// Find XML path for a module by its ID
const char *nn_cfg_find_xml_path(uint32_t module_id)
{
    for (GSList *node = g_xml_registry; node != NULL; node = node->next)
    {
        nn_cfg_xml_entry_t *entry = (nn_cfg_xml_entry_t *)node->data;
        if (entry->module_id == module_id)
        {
            return entry->xml_path;
        }
    }
    return NULL;
}

// Cleanup the XML registry
void nn_cfg_cleanup_xml_registry(void)
{
    for (GSList *node = g_xml_registry; node != NULL; node = node->next)
    {
        nn_cfg_xml_entry_t *entry = (nn_cfg_xml_entry_t *)node->data;
        g_free(entry->xml_path);
        g_free(entry);
    }
    g_slist_free(g_xml_registry);
    g_xml_registry = NULL;
}
