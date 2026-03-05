/**
 * @file   vrf_api.c
 * @brief  VRF 模块对外 API 实现：提供其他模块通过 RPC 查询 VRF 名称的接口
 * @author jhb
 * @date   2026/03/05
 */
#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "vrf.h"

int vrf_get_name(dev_ipc_context_t *ctx, uint32_t vrf_id, char *name_out, size_t name_size)
{
    if (!ctx || !name_out || name_size == 0)
    {
        return ERRCODE_FAIL;
    }

    name_out[0] = '\0';

    /* 构建请求：payload = uint32_t vrf_id（本机字节序，同机 IPC） */
    dev_ipc_message_t *req = dev_ipc_message_create(VRF_MSG_TYPE_GET_NAME, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_VRF, 0, &vrf_id, sizeof(uint32_t), NULL);
    if (!req)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_VRF, req, 0);
    dev_ipc_message_free(req);

    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    if (resp->payload && resp->payload_len > 0)
    {
        strlcpy(name_out, (const char *)resp->payload, name_size);
        dev_ipc_message_free(resp);
        return ERRCODE_SUCCESS;
    }

    dev_ipc_message_free(resp);
    return ERRCODE_FAIL;
}
