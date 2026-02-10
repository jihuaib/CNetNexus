/**
 * @file   cfg_registry.h
 * @brief  模块注册表头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CFG_MODULE_REGISTRY_H
#define CFG_MODULE_REGISTRY_H

#include <stdint.h>

#include "cli.h"

// Module XML entry stored in GLib list
typedef struct
{
    uint32_t module_id;
    char *xml_path;
} cfg_xml_entry_t;

void cfg_register_module_xml_inner(uint32_t module_id, const char *xml_path);

#endif // CFG_MODULE_REGISTRY_H
