#ifndef NN_CFG_MODULE_REGISTRY_H
#define NN_CFG_MODULE_REGISTRY_H

#include <stdint.h>

#include "nn_cfg.h"

// Module XML entry stored in GLib list
typedef struct
{
    uint32_t module_id;
    char *xml_path;
} nn_cfg_xml_entry_t;

void nn_cfg_register_module_xml_inner(uint32_t module_id, const char *xml_path);

#endif // NN_CFG_MODULE_REGISTRY_H
