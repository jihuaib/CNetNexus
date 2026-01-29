#include <stdio.h>
#include "nn_cfg.h"
#include "nn_cfg_registry.h"

#include "nn_errcode.h"
#include "nn_cli_view.h"
#include "nn_cli_param_type.h"

void nn_cfg_register_module_xml(uint32_t module_id, const char *xml_path)
{
    if (!xml_path)
    {
        return;
    }

    nn_cfg_register_module_xml_inner(module_id, xml_path);

    printf("[cfg] Registered XML for module ID %u -> %s\n", module_id, xml_path);
}

// Get view prompt template by view name (for modules to fill placeholders)
int nn_cfg_get_view_prompt_template(uint32_t view_id, char *view_name)
{
    if (view_name == NULL)
    {
        return NN_ERRCODE_FAIL;
    }
    return nn_cfg_get_view_prompt_template_inner(view_id, view_name);
}

nn_cli_param_type_t *nn_cfg_param_type_parse(const char *type_str)
{
    return nn_cli_param_type_parse(type_str);
}

void nn_cfg_param_type_free(nn_cli_param_type_t *param_type)
{
    if (!param_type)
    {
        return;
    }

    nn_cli_param_type_free(param_type);
}

gboolean nn_cfg_param_type_validate(const nn_cli_param_type_t *param_type, const char *value, char *error_msg,
                                    uint32_t error_msg_size)
{
    return nn_cli_param_type_validate(param_type, value, error_msg, error_msg_size);
}