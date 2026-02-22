/**
 * @file   cfg_api.c
 * @brief  CFG 模块对外 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <stdio.h>

#include "cli.h"
#include "cli_param_type.h"
#include "cli_view.h"
#include "errcode.h"

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
