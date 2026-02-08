/**
 * @file   cfg_api.c
 * @brief  CFG 模块对外 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <stdio.h>

#include "cfg.h"
#include "cfg_registry.h"
#include "cli_param_type.h"
#include "cli_view.h"
#include "config_template.h"
#include "errcode.h"

void cfg_register_module_xml(uint32_t module_id, const char *xml_path)
{
    if (!xml_path)
    {
        return;
    }

    cfg_register_module_xml_inner(module_id, xml_path);

    printf("[cfg] Registered XML for module ID %u -> %s\n", module_id, xml_path);
}

// Get view prompt template by view name (for modules to fill placeholders)
int cfg_get_view_prompt_template(uint32_t view_id, char *view_name)
{
    if (view_name == NULL)
    {
        return ERRCODE_FAIL;
    }
    return cfg_get_view_prompt_template_inner(view_id, view_name);
}

cli_param_type_t *cfg_param_type_parse(const char *type_str)
{
    return cli_param_type_parse(type_str);
}

void cfg_param_type_free(cli_param_type_t *param_type)
{
    if (!param_type)
    {
        return;
    }

    cli_param_type_free(param_type);
}

gboolean cfg_param_type_validate(const cli_param_type_t *param_type, const char *value, char *error_msg,
                                    uint32_t error_msg_size)
{
    return cli_param_type_validate(param_type, value, error_msg, error_msg_size);
}

// ============================================================================
// 配置模板 API
// ============================================================================

struct config_template *cfg_get_config_template(const char *template_name)
{
    if (!template_name)
    {
        return NULL;
    }

    return config_template_find_by_name(template_name);
}

char *cfg_render_template(const char *template_name, GHashTable *var_values)
{
    if (!template_name)
    {
        return NULL;
    }

    struct config_template *template = config_template_find_by_name(template_name);
    if (!template)
    {
        return NULL;
    }

    // 直接使用提供的变量映射表渲染模板
    char *rendered = config_template_render(template, var_values);

    return rendered;
}