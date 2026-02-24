/**
 * @file   dev_api.c
 * @brief  Dev 模块对外 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <glib.h>
#include <string.h>

#include "dev.h"
#include "errcode.h"
#include "ipc.h"

int dev_get_module_name(ipc_context_t *ctx, uint32_t module_id, char *module_name)
{
    if (!ctx || !module_name)
    {
        return ERRCODE_FAIL;
    }

    module_name[0] = '\0';

    /* 构建请求 payload: 4 字节模块 ID */
    ipc_message_t *req =
        ipc_message_create(IPC_MSG_TYPE_DEV_GET_MODULE_NAME, ipc_get_module_id(ctx), DEV_MODULE_ID_DEV, 0, &module_id,
                           sizeof(uint32_t), NULL);
    if (!req)
    {
        return ERRCODE_FAIL;
    }

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DEV, req, 0);
    ipc_message_free(req);

    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    if (resp->payload && resp->payload_len > 0)
    {
        strlcpy(module_name, (const char *)resp->payload, DEV_MODULE_NAME_MAX_LEN);
    }

    ipc_message_free(resp);
    return ERRCODE_SUCCESS;
}
