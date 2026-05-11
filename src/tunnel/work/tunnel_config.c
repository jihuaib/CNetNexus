#include "tunnel_config.h"

#include <string.h>

#include "errcode.h"
#include "mpls_config.h"

int tunnel_config_load(tunnel_config_t *cfg)
{
    if (!cfg)
    {
        return ERRCODE_FAIL;
    }

    nn_mpls_config_t shared_cfg;
    if (nn_mpls_config_load(&shared_cfg) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->label_dynamic_min = shared_cfg.label_dynamic_min;
    cfg->label_dynamic_max = shared_cfg.label_dynamic_max;
    cfg->linux_platform_labels = shared_cfg.linux_platform_labels;
    cfg->linux_mpls_input = shared_cfg.linux_mpls_input;
    return ERRCODE_SUCCESS;
}
