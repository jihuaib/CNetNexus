/**
 * @file   dev_api.c
 * @brief  Dev 模块对外 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "errcode.h"
#include "ipc.h"

int dev_get_module_name(ipc_context_t *ctx, uint32_t module_id, char *module_name)
{
    if (!ctx || !module_name)
    {
        return ERRCODE_FAIL;
    }

    module_name[0] = '\0';

    /* DEV 自查询直接走本地注册表，避免自环 IPC */
    if (ipc_get_module_id(ctx) == DEV_MODULE_ID_DEV)
    {
        return ERRCODE_SUCCESS;
    }

    ipc_dev_get_module_name_req_t *req_payload = g_malloc0(sizeof(ipc_dev_get_module_name_req_t));
    req_payload->module_id = module_id;

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DEV_GET_MODULE_NAME, ipc_get_module_id(ctx), DEV_MODULE_ID_DEV,
                                            0, req_payload, sizeof(*req_payload), g_free);
    if (!req)
    {
        g_free(req_payload);
        return ERRCODE_FAIL;
    }

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DEV, req, IPC_QUERY_TIMEOUT_DEFAULT);
    ipc_message_free(req);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    int ret = ERRCODE_FAIL;
    if (resp->msg_type == IPC_MSG_TYPE_DEV_GET_MODULE_NAME_RESP &&
        resp->payload_len == sizeof(ipc_dev_get_module_name_resp_t) && resp->payload)
    {
        const ipc_dev_get_module_name_resp_t *payload = (const ipc_dev_get_module_name_resp_t *)resp->payload;
        if (payload->result == ERRCODE_SUCCESS)
        {
            strlcpy(module_name, payload->name, DEV_MODULE_NAME_MAX_LEN);
            ret = ERRCODE_SUCCESS;
        }
    }

    ipc_message_free(resp);
    return ret;
}
