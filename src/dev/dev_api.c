/**
 * @file   dev_api.c
 * @brief  Dev 模块对外 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <stdio.h>

#include "dev_module.h"
#include "errcode.h"

void dev_register_module(uint32_t id, const char *name, ipc_msg_handler_fn handler)
{
    if (!name)
    {
        return;
    }

    dev_register_module_inner(id, name, handler);

    printf("[dev] Registered module: %s\n", name);
}

int dev_get_module_name(uint32_t module_id, char *module_name)
{
    if (!module_name)
    {
        return ERRCODE_FAIL;
    }

    return dev_get_module_name_inner(module_id, module_name);
}

void dev_request_shutdown(void)
{
    dev_request_shutdown_inner();
}

int dev_shutdown_requested(void)
{
    return dev_shutdown_requested_inner();
}
