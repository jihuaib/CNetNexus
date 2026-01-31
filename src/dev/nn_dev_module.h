/**
 * @file   nn_dev_module.h
 * @brief  Dev 模块注册头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef NN_DEV_MODULE_H
#define NN_DEV_MODULE_H

#include <glib.h>
#include <stdint.h>

#include "nn_dev.h"

// Module descriptor structure
typedef struct nn_module
{
    uint32_t module_id;
    char name[NN_DEV_MODULE_NAME_MAX_LEN]; // Module name
    nn_module_init_fn init;                // Module initialization function
    nn_module_cleanup_fn cleanup;          // Module cleanup function
    // Message queue for inter-module communication
    nn_dev_module_mq_t *mq; // Module message queue (NULL if not initialized)
} nn_dev_module_t;

int32_t nn_dev_init_all_modules(void);

void nn_cleanup_all_modules(void);

void nn_dev_module_foreach(GTraverseFunc func, gpointer user_data);

void nn_dev_register_module_inner(uint32_t id, const char *name, nn_module_init_fn init, nn_module_cleanup_fn cleanup);

int nn_dev_get_module_name_inner(uint32_t module_id, char *module_name);

void nn_dev_request_shutdown_inner(void);

int nn_dev_shutdown_requested_inner(void);

#endif // NN_DEV_MODULE_H