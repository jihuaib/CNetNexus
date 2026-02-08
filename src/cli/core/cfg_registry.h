/**
 * @file   cfg_registry.h
 * @brief  模块注册表头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CFG_MODULE_REGISTRY_H
#define CFG_MODULE_REGISTRY_H

#include <stdint.h>

#include "cfg.h"

// Module XML entry stored in GLib list
typedef struct
{
    uint32_t module_id;
    char *xml_path;
} cfg_xml_entry_t;

void cfg_register_module_xml_inner(uint32_t module_id, const char *xml_path);

/**
 * @brief 根据 view ID 获取 prompt template
 * @param view_id 视图ID
 * @param view_name 输出参数：存储 prompt template
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int cfg_get_view_prompt_template_inner(uint32_t view_id, char *view_name);

#endif // CFG_MODULE_REGISTRY_H
