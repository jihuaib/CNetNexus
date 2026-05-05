#ifndef TUNNEL_CONFIG_H
#define TUNNEL_CONFIG_H

#include <glib.h>
#include <stdint.h>

typedef struct tunnel_config
{
    uint32_t label_dynamic_min;
    uint32_t label_dynamic_max;
    uint32_t linux_platform_labels;
    gboolean linux_mpls_input;
} tunnel_config_t;

int tunnel_config_load(tunnel_config_t *cfg);

#endif /* TUNNEL_CONFIG_H */
