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
