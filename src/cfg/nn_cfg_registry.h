#ifndef NN_CFG_MODULE_REGISTRY_H
#define NN_CFG_MODULE_REGISTRY_H

#include <stdint.h>

// Module XML entry stored in GLib list
typedef struct
{
    uint32_t module_id;
    char *xml_path;
} nn_cfg_xml_entry_t;

#endif // NN_CFG_MODULE_REGISTRY_H
