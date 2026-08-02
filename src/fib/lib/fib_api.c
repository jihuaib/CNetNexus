#include <glib.h>

#include "errcode.h"
#include "fib.h"

static int fib_rpc_send_route(dev_ipc_context_t *ctx, uint32_t msg_type, const fib_route_entry_t *entry)
{
    if (!ctx || !entry)
    {
        return ERRCODE_FAIL;
    }

    fib_route_entry_t *payload = g_memdup2(entry, sizeof(*entry));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_FIB, 0, payload,
                                                    sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_FIB, msg);
    dev_ipc_message_free(msg);
    return (rc == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int fib_rpc_send_tunnel(dev_ipc_context_t *ctx, uint32_t msg_type, const fib_tunnel_entry_t *entry)
{
    if (!ctx || !entry)
    {
        return ERRCODE_FAIL;
    }

    fib_tunnel_entry_t *payload = g_memdup2(entry, sizeof(*entry));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_FIB, 0, payload,
                                                    sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_FIB, msg);
    dev_ipc_message_free(msg);
    return (rc == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int fib_rpc_send_ilm(dev_ipc_context_t *ctx, uint32_t msg_type, const fib_ilm_entry_t *entry)
{
    if (!ctx || !entry)
    {
        return ERRCODE_FAIL;
    }

    fib_ilm_entry_t *payload = g_memdup2(entry, sizeof(*entry));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_FIB, 0, payload,
                                                    sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_FIB, msg);
    dev_ipc_message_free(msg);
    return (rc == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int fib_rpc_send_nexthop(dev_ipc_context_t *ctx, uint32_t msg_type, const fib_nexthop_entry_t *entry)
{
    if (!ctx || !entry)
    {
        return ERRCODE_FAIL;
    }

    fib_nexthop_entry_t *payload = g_memdup2(entry, sizeof(*entry));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_FIB, 0, payload,
                                                    sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_FIB, msg);
    dev_ipc_message_free(msg);
    return (rc == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int fib_rpc_send_srv6_localsid(dev_ipc_context_t *ctx, uint32_t msg_type, const fib_srv6_localsid_entry_t *entry,
                                      uint32_t timeout_ms, gboolean wait)
{
    if (!ctx || !entry)
    {
        return ERRCODE_FAIL;
    }

    fib_srv6_localsid_entry_t *payload = g_memdup2(entry, sizeof(*entry));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_FIB, 0, payload,
                                                    sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    if (!wait)
    {
        int rc = dev_ipc_send(ctx, DEV_MODULE_ID_FIB, msg);
        dev_ipc_message_free(msg);
        return (rc == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_FIB, msg, timeout_ms ? timeout_ms : 3000u);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }
    int rc = ERRCODE_FAIL;
    if (resp->msg_type == FIB_MSG_TYPE_ACK && resp->payload && resp->payload_len >= sizeof(fib_msg_ack_t))
    {
        const fib_msg_ack_t *ack = (const fib_msg_ack_t *)resp->payload;
        rc = (ack->result == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }
    dev_ipc_message_free(resp);
    return rc;
}

int fib_rpc_route_upsert(dev_ipc_context_t *ctx, const fib_route_entry_t *entry)
{
    return fib_rpc_send_route(ctx, FIB_MSG_TYPE_ROUTE_UPSERT, entry);
}

int fib_rpc_route_delete(dev_ipc_context_t *ctx, const fib_route_entry_t *entry)
{
    return fib_rpc_send_route(ctx, FIB_MSG_TYPE_ROUTE_DELETE, entry);
}

int fib_rpc_tunnel_upsert(dev_ipc_context_t *ctx, const fib_tunnel_entry_t *entry)
{
    return fib_rpc_send_tunnel(ctx, FIB_MSG_TYPE_TUNNEL_UPSERT, entry);
}

int fib_rpc_tunnel_delete(dev_ipc_context_t *ctx, const fib_tunnel_entry_t *entry)
{
    return fib_rpc_send_tunnel(ctx, FIB_MSG_TYPE_TUNNEL_DELETE, entry);
}

int fib_rpc_ilm_upsert(dev_ipc_context_t *ctx, const fib_ilm_entry_t *entry)
{
    return fib_rpc_send_ilm(ctx, FIB_MSG_TYPE_ILM_UPSERT, entry);
}

int fib_rpc_ilm_delete(dev_ipc_context_t *ctx, const fib_ilm_entry_t *entry)
{
    return fib_rpc_send_ilm(ctx, FIB_MSG_TYPE_ILM_DELETE, entry);
}

int fib_rpc_nexthop_upsert(dev_ipc_context_t *ctx, const fib_nexthop_entry_t *entry)
{
    return fib_rpc_send_nexthop(ctx, FIB_MSG_TYPE_NEXTHOP_UPSERT, entry);
}

int fib_rpc_nexthop_delete(dev_ipc_context_t *ctx, const fib_nexthop_entry_t *entry)
{
    return fib_rpc_send_nexthop(ctx, FIB_MSG_TYPE_NEXTHOP_DELETE, entry);
}

int fib_rpc_srv6_localsid_upsert(dev_ipc_context_t *ctx, const fib_srv6_localsid_entry_t *entry)
{
    return fib_rpc_send_srv6_localsid(ctx, FIB_MSG_TYPE_SRV6_LOCALSID_UPSERT, entry, 0, FALSE);
}

int fib_rpc_srv6_localsid_delete(dev_ipc_context_t *ctx, const fib_srv6_localsid_entry_t *entry)
{
    return fib_rpc_send_srv6_localsid(ctx, FIB_MSG_TYPE_SRV6_LOCALSID_DELETE, entry, 0, FALSE);
}

int fib_rpc_srv6_localsid_upsert_wait(dev_ipc_context_t *ctx, const fib_srv6_localsid_entry_t *entry,
                                      uint32_t timeout_ms)
{
    return fib_rpc_send_srv6_localsid(ctx, FIB_MSG_TYPE_SRV6_LOCALSID_UPSERT, entry, timeout_ms, TRUE);
}

int fib_rpc_srv6_localsid_delete_wait(dev_ipc_context_t *ctx, const fib_srv6_localsid_entry_t *entry,
                                      uint32_t timeout_ms)
{
    return fib_rpc_send_srv6_localsid(ctx, FIB_MSG_TYPE_SRV6_LOCALSID_DELETE, entry, timeout_ms, TRUE);
}
