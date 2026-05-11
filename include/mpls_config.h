/**
 * @file   mpls_config.h
 * @brief  Shared MPLS/tunnel configuration loader.
 */
#ifndef MPLS_CONFIG_H
#define MPLS_CONFIG_H

#include <glib.h>
#include <stdint.h>

#define NN_MPLS_LABEL_VALUE_MAX 0xFFFFFu
#define NN_MPLS_PLATFORM_LABELS_MAX 1048576u

typedef struct nn_mpls_config
{
    uint32_t label_dynamic_min;
    uint32_t label_dynamic_max;
    uint32_t linux_platform_labels;
    gboolean linux_mpls_input;
} nn_mpls_config_t;

int nn_mpls_config_load(nn_mpls_config_t *cfg);

#endif /* MPLS_CONFIG_H */
