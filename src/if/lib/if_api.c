/**
 * @file   if_api.c
 * @brief  IF 模块对外 API 实现
 * @author jhb
 * @date   2026/02/14
 */
#include <glib.h>
#include <string.h>

#include "dev.h"
#include "if_event.h"

if_intf_map_resp_t *if_rpc_get_intf_map(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return NULL;
    }

    dev_ipc_message_t *req = dev_ipc_message_create(IF_MSG_TYPE_GET_INTF_MAP, 0, DEV_MODULE_ID_IF, 0, NULL, 0, NULL);
    if (!req)
    {
        return NULL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_IF, req, 2000);
    dev_ipc_message_free(req);
    if (!resp)
    {
        return NULL;
    }

    if_intf_map_resp_t *result = NULL;
    if (resp->payload && resp->payload_len >= sizeof(if_intf_map_resp_t))
    {
        result = (if_intf_map_resp_t *)g_malloc(resp->payload_len);
        memcpy(result, resp->payload, resp->payload_len);
    }

    dev_ipc_message_free(resp);
    return result;
}

const char *if_intf_map_lookup(const if_intf_map_resp_t *resp, uint32_t ifindex)
{
    if (!resp || ifindex == 0)
    {
        return NULL;
    }
    for (uint32_t i = 0; i < resp->count; i++)
    {
        if (resp->items[i].ifindex == ifindex)
        {
            return resp->items[i].logical_name;
        }
    }
    return NULL;
}
