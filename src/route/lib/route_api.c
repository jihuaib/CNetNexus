/**
 * @file   route_api.c
 * @brief  Route 模块对外 API 实现
 * @author jhb
 * @date   2026/03/15
 */
#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "route.h"

#define ROUTE_RPC_DEFAULT_TIMEOUT_MS 3000u

int route_rpc_add(dev_ipc_context_t *ctx, const route_msg_entry_t *entry)
{
    if (!ctx || !entry)
    {
        return ERRCODE_FAIL;
    }

    route_msg_entry_t *payload = (route_msg_entry_t *)g_memdup2(entry, sizeof(route_msg_entry_t));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_INJECT, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_ROUTE, 0, payload, sizeof(route_msg_entry_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, msg);
    dev_ipc_message_free(msg);
    return (ret == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int route_rpc_add_wait(dev_ipc_context_t *ctx, const route_msg_entry_t *entry, uint32_t timeout_ms)
{
    if (!ctx || !entry)
    {
        return ERRCODE_FAIL;
    }

    route_msg_entry_t *payload = (route_msg_entry_t *)g_memdup2(entry, sizeof(route_msg_entry_t));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_INJECT, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_ROUTE, 0, payload, sizeof(route_msg_entry_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    uint32_t wait_ms = (timeout_ms == 0) ? ROUTE_RPC_DEFAULT_TIMEOUT_MS : timeout_ms;
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_ROUTE, msg, wait_ms);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    int result = ERRCODE_FAIL;
    if (resp->msg_type == ROUTE_MSG_TYPE_ACK && resp->payload && resp->payload_len >= sizeof(route_msg_ack_t))
    {
        const route_msg_ack_t *ack = (const route_msg_ack_t *)resp->payload;
        result = (ack->result == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }
    dev_ipc_message_free(resp);
    return result;
}

static int route_rpc_nh_iter_send(dev_ipc_context_t *ctx, uint32_t msg_type, const route_nh_iter_req_t *req)
{
    if (!ctx || !req)
    {
        return ERRCODE_FAIL;
    }

    route_nh_iter_req_t *payload = (route_nh_iter_req_t *)g_memdup2(req, sizeof(route_nh_iter_req_t));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_ROUTE, 0,
                                                    payload, sizeof(route_nh_iter_req_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, msg);
    dev_ipc_message_free(msg);
    return (ret == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int route_rpc_nh_register(dev_ipc_context_t *ctx, const route_nh_iter_req_t *req)
{
    return route_rpc_nh_iter_send(ctx, ROUTE_MSG_TYPE_NH_REGISTER, req);
}

int route_rpc_nhobj_acquire_wait(dev_ipc_context_t *ctx, const route_nhobj_msg_t *req, uint32_t timeout_ms,
                                 uint32_t *nexthop_id_out)
{
    if (!ctx || !req || !nexthop_id_out)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_msg_t *payload = (route_nhobj_msg_t *)g_memdup2(req, sizeof(route_nhobj_msg_t));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_NHOBJ_ACQUIRE, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_ROUTE, 0, payload, sizeof(route_nhobj_msg_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    uint32_t wait_ms = (timeout_ms == 0) ? ROUTE_RPC_DEFAULT_TIMEOUT_MS : timeout_ms;
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_ROUTE, msg, wait_ms);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    int result = ERRCODE_FAIL;
    if (resp->msg_type == ROUTE_MSG_TYPE_ACK && resp->payload && resp->payload_len >= sizeof(route_msg_ack_t))
    {
        const route_msg_ack_t *ack = (const route_msg_ack_t *)resp->payload;
        if (ack->result == ERRCODE_SUCCESS && ack->nexthop_id != 0u)
        {
            *nexthop_id_out = ack->nexthop_id;
            result = ERRCODE_SUCCESS;
        }
    }
    dev_ipc_message_free(resp);
    return result;
}

int route_rpc_nhobj_release(dev_ipc_context_t *ctx, uint32_t nexthop_id)
{
    if (!ctx || nexthop_id == 0u)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_release_req_t *payload = (route_nhobj_release_req_t *)g_malloc0(sizeof(*payload));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }
    payload->nexthop_id = nexthop_id;

    dev_ipc_message_t *msg =
        dev_ipc_message_create(ROUTE_MSG_TYPE_NHOBJ_RELEASE, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_ROUTE, 0,
                               payload, sizeof(route_nhobj_release_req_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, msg);
    dev_ipc_message_free(msg);
    return (ret == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int route_rpc_del(dev_ipc_context_t *ctx, const route_msg_entry_t *entry)
{
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    route_msg_entry_t withdraw_entry = *entry;
    withdraw_entry.metric = 0;
    withdraw_entry.preference = 0;
    withdraw_entry.is_withdraw = 1;
    withdraw_entry.nexthop_addr.family = withdraw_entry.source_addr.family;

    return route_rpc_add(ctx, &withdraw_entry);
}

int route_rpc_del_wait(dev_ipc_context_t *ctx, const route_msg_entry_t *entry, uint32_t timeout_ms)
{
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    route_msg_entry_t withdraw_entry = *entry;
    withdraw_entry.metric = 0;
    withdraw_entry.preference = 0;
    withdraw_entry.is_withdraw = 1;
    withdraw_entry.nexthop_addr.family = withdraw_entry.source_addr.family;

    return route_rpc_add_wait(ctx, &withdraw_entry, timeout_ms);
}

int route_rpc_flush_protocol_wait(dev_ipc_context_t *ctx, const route_protocol_flush_req_t *req, uint32_t timeout_ms)
{
    if (!ctx || !req || req->protocol == ROUTE_PROTOCOL_MAX || req->_pad != 0u ||
        (req->afi != ROUTE_AFI_IPV4 && req->afi != ROUTE_AFI_IPV6 && req->afi != ROUTE_AFI_ALL))
    {
        return ERRCODE_FAIL;
    }

    route_protocol_flush_req_t *payload = g_memdup2(req, sizeof(*req));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_PROTOCOL_FLUSH, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_ROUTE, 0, payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    uint32_t wait_ms = (timeout_ms == 0u) ? ROUTE_RPC_DEFAULT_TIMEOUT_MS : timeout_ms;
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_ROUTE, msg, wait_ms);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    int result = ERRCODE_FAIL;
    if (resp->msg_type == ROUTE_MSG_TYPE_ACK && resp->payload && resp->payload_len >= sizeof(route_msg_ack_t))
    {
        const route_msg_ack_t *ack = resp->payload;
        result = (ack->result == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }
    dev_ipc_message_free(resp);
    return result;
}

int route_rpc_nh_unregister(dev_ipc_context_t *ctx, const route_nh_iter_req_t *req)
{
    return route_rpc_nh_iter_send(ctx, ROUTE_MSG_TYPE_NH_UNREGISTER, req);
}
