/**
 * @file   cfg_registry.c
 * @brief  模块注册表实现，管理所有已注册模块
 * @author jhb
 * @date   2026/01/22
 */
#include "cfg_registry.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cli_engine.h"
#include "cli_view.h"
#include "errcode.h"

// Global XML registry using GLib's singly-linked list
GSList *g_xml_registry = NULL;

// Register module's XML path by module ID
void cfg_register_module_xml_inner(uint32_t module_id, const char *xml_path)
{
    cfg_xml_entry_t *entry = g_malloc0(sizeof(cfg_xml_entry_t));
    entry->module_id = module_id;
    entry->xml_path = g_strdup(xml_path);

    g_xml_registry = g_slist_prepend(g_xml_registry, entry);
}

// Find XML path for a module by its ID
const char *cfg_find_xml_path(uint32_t module_id)
{
    for (GSList *node = g_xml_registry; node != NULL; node = node->next)
    {
        cfg_xml_entry_t *entry = (cfg_xml_entry_t *)node->data;
        if (entry->module_id == module_id)
        {
            return entry->xml_path;
        }
    }
    return NULL;
}

// Cleanup the XML registry
void cfg_cleanup_xml_registry(void)
{
    for (GSList *node = g_xml_registry; node != NULL; node = node->next)
    {
        cfg_xml_entry_t *entry = (cfg_xml_entry_t *)node->data;
        g_free(entry->xml_path);
        g_free(entry);
    }
    g_slist_free(g_xml_registry);
    g_xml_registry = NULL;
}

// Get view prompt template by view ID
int cfg_get_view_prompt_template_inner(uint32_t view_id, char *view_name)
{
    extern cli_engine_context_t *g_cli_engine_ctx;
    
    if (!view_name || !g_cli_engine_ctx)
    {
        return ERRCODE_FAIL;
    }
    
    cli_view_node_t *view = cli_view_find_by_id(g_cli_engine_ctx->view_tree.root, view_id);
    if (!view || !view->prompt_template)
    {
        return ERRCODE_FAIL;
    }
    
    strncpy(view_name, view->prompt_template, CFG_CLI_MAX_PROMPT_LEN - 1);
    view_name[CFG_CLI_MAX_PROMPT_LEN - 1] = '\0';
    
    return ERRCODE_SUCCESS;
}
