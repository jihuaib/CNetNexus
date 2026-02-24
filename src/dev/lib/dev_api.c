/**
 * @file   dev_api.c
 * @brief  Dev 模块对外 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "errcode.h"
#include "ipc.h"

int dev_get_module_name(ipc_context_t *ctx, uint32_t module_id, char *module_name)
{
    if (!ctx || !module_name)
    {
        return ERRCODE_FAIL;
    }

    module_name[0] = '\0';

    const ipc_module_table_t *table = ipc_get_module_table(ctx);
    if (!table)
    {
        return ERRCODE_FAIL;
    }

    for (uint32_t i = 0; i < table->count; i++)
    {
        if (table->entries[i].module_id == module_id)
        {
            strlcpy(module_name, table->entries[i].name, DEV_MODULE_NAME_MAX_LEN);
            return ERRCODE_SUCCESS;
        }
    }

    return ERRCODE_FAIL;
}
