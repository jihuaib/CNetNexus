/**
 * @file   dev_module.h
 * @brief  Dev 模块注册头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef DEV_MODULE_H
#define DEV_MODULE_H

#include <glib.h>
#include <stdint.h>

#include "dev.h"

// Module descriptor structure
typedef struct module
{
    uint32_t module_id;
    char name[DEV_MODULE_NAME_MAX_LEN]; // Module name
    module_init_fn init;                // Module initialization function
    module_cleanup_fn cleanup;          // Module cleanup function
} dev_module_t;

int32_t dev_init_all_modules(void);

void cleanup_all_modules(void);

void dev_module_foreach(GTraverseFunc func, gpointer user_data);

void dev_register_module_inner(uint32_t id, const char *name, module_init_fn init, module_cleanup_fn cleanup);

int dev_get_module_name_inner(uint32_t module_id, char *module_name);

void dev_request_shutdown_inner(void);

int dev_shutdown_requested_inner(void);

#endif // DEV_MODULE_H
