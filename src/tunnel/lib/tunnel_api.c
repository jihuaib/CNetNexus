#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "tunnel.h"

#define TUNNEL_RPC_DEFAULT_TIMEOUT_MS 3000u

static int tunnel_rpc_send_candidate(dev_ipc_context_t *ctx, uint32_t msg_type, const tunnel_candidate_t *candidate)
{
    if (!ctx || !candidate)
    {
        return ERRCODE_FAIL;
    }

    tunnel_candidate_t *payload = g_memdup2(candidate, sizeof(*candidate));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_TUNNEL, 0,
                                                    payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_TUNNEL, msg);
    dev_ipc_message_free(msg);
    return (rc == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

static int tunnel_rpc_send_resolve(dev_ipc_context_t *ctx, uint32_t msg_type, const tunnel_resolve_req_t *req)
{
    if (!ctx || !req)
    {
        return ERRCODE_FAIL;
    }

    tunnel_resolve_req_t *payload = g_memdup2(req, sizeof(*req));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_TUNNEL, 0,
                                                    payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_TUNNEL, msg);
    dev_ipc_message_free(msg);
    return (rc == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int tunnel_rpc_candidate_add(dev_ipc_context_t *ctx, const tunnel_candidate_t *candidate)
{
    return tunnel_rpc_send_candidate(ctx, TUNNEL_MSG_TYPE_CANDIDATE_ADD, candidate);
}

int tunnel_rpc_candidate_del(dev_ipc_context_t *ctx, const tunnel_candidate_t *candidate)
{
    return tunnel_rpc_send_candidate(ctx, TUNNEL_MSG_TYPE_CANDIDATE_DEL, candidate);
}

int tunnel_rpc_resolve_register(dev_ipc_context_t *ctx, const tunnel_resolve_req_t *req)
{
    return tunnel_rpc_send_resolve(ctx, TUNNEL_MSG_TYPE_RESOLVE_REGISTER, req);
}

int tunnel_rpc_resolve_unregister(dev_ipc_context_t *ctx, const tunnel_resolve_req_t *req)
{
    return tunnel_rpc_send_resolve(ctx, TUNNEL_MSG_TYPE_RESOLVE_UNREGISTER, req);
}

int tunnel_rpc_label_alloc(dev_ipc_context_t *ctx, const tunnel_label_req_t *req, uint32_t *label_out,
                           uint32_t timeout_ms)
{
    if (!ctx || !req || !label_out)
    {
        return ERRCODE_FAIL;
    }

    tunnel_label_req_t *payload = g_memdup2(req, sizeof(*req));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(TUNNEL_MSG_TYPE_LABEL_ALLOC, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_TUNNEL, 0, payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    uint32_t wait_ms = (timeout_ms == 0) ? TUNNEL_RPC_DEFAULT_TIMEOUT_MS : timeout_ms;
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_TUNNEL, msg, wait_ms);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    if (resp->msg_type == TUNNEL_MSG_TYPE_ACK && resp->payload && resp->payload_len >= sizeof(tunnel_msg_ack_t))
    {
        const tunnel_msg_ack_t *ack = (const tunnel_msg_ack_t *)resp->payload;
        if (ack->result == ERRCODE_SUCCESS && ack->label > 0 && ack->label <= TUNNEL_LABEL_VALUE_MAX)
        {
            *label_out = ack->label;
            rc = ERRCODE_SUCCESS;
        }
    }
    dev_ipc_message_free(resp);
    return rc;
}

int tunnel_rpc_label_release(dev_ipc_context_t *ctx, const tunnel_label_req_t *req)
{
    if (!ctx || !req)
    {
        return ERRCODE_FAIL;
    }

    tunnel_label_req_t *payload = g_memdup2(req, sizeof(*req));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(TUNNEL_MSG_TYPE_LABEL_RELEASE, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_TUNNEL, 0, payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int rc = dev_ipc_send(ctx, DEV_MODULE_ID_TUNNEL, msg);
    dev_ipc_message_free(msg);
    return (rc == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}
