#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "srv6.h"

const char *srv6_behavior_name(uint16_t behavior)
{
    switch (behavior)
    {
        case SRV6_BEHAVIOR_END_DT4:
            return "End.DT4";
        case SRV6_BEHAVIOR_END_DT6:
            return "End.DT6";
        default:
            return "Unknown";
    }
}

static int srv6_rpc_sid_query(dev_ipc_context_t *ctx, uint32_t msg_type, const srv6_sid_key_t *key,
                              srv6_sid_entry_t *entry_out, uint32_t timeout_ms)
{
    if (!ctx || !key || key->locator[0] == '\0')
    {
        return ERRCODE_FAIL;
    }

    srv6_sid_key_t *payload = g_memdup2(key, sizeof(*payload));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }
    payload->locator[sizeof(payload->locator) - 1u] = '\0';
    payload->owner_module_id = dev_ipc_get_module_id(ctx);

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_SRV6, 0,
                                                    payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    uint32_t wait_ms = timeout_ms ? timeout_ms : SRV6_RPC_DEFAULT_TIMEOUT_MS;
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_SRV6, msg, wait_ms);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    if (resp->msg_type == SRV6_MSG_TYPE_SID_RESULT && resp->payload && resp->payload_len >= sizeof(srv6_sid_result_t))
    {
        const srv6_sid_result_t *result = (const srv6_sid_result_t *)resp->payload;
        rc = result->result;
        if (rc == ERRCODE_SUCCESS && entry_out)
        {
            if (!result->found)
            {
                rc = ERRCODE_DEP_MISSING;
            }
            else
            {
                *entry_out = result->entry;
            }
        }
    }
    dev_ipc_message_free(resp);
    return rc;
}

int srv6_rpc_sid_alloc(dev_ipc_context_t *ctx, const srv6_sid_key_t *key, srv6_sid_entry_t *entry_out,
                       uint32_t timeout_ms)
{
    if (!entry_out)
    {
        return ERRCODE_FAIL;
    }
    memset(entry_out, 0, sizeof(*entry_out));
    return srv6_rpc_sid_query(ctx, SRV6_MSG_TYPE_SID_ALLOC, key, entry_out, timeout_ms);
}

int srv6_rpc_sid_release(dev_ipc_context_t *ctx, const srv6_sid_key_t *key, uint32_t timeout_ms)
{
    return srv6_rpc_sid_query(ctx, SRV6_MSG_TYPE_SID_RELEASE, key, NULL, timeout_ms);
}

int srv6_rpc_sid_release_owner(dev_ipc_context_t *ctx, const srv6_sid_owner_scope_t *scope, uint32_t timeout_ms)
{
    if (!ctx || !scope || scope->vrf_id == 0u || scope->behavior == 0u || scope->owner_id == 0u)
    {
        return ERRCODE_FAIL;
    }

    srv6_sid_owner_scope_t *payload = g_memdup2(scope, sizeof(*payload));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }
    payload->_pad0 = 0u;
    dev_ipc_message_t *msg = dev_ipc_message_create(SRV6_MSG_TYPE_SID_RELEASE_OWNER, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_SRV6, 0, payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }
    uint32_t wait_ms = timeout_ms ? timeout_ms : SRV6_RPC_DEFAULT_TIMEOUT_MS;
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_SRV6, msg, wait_ms);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }
    int rc = ERRCODE_FAIL;
    if (resp->msg_type == SRV6_MSG_TYPE_SID_RESULT && resp->payload && resp->payload_len >= sizeof(srv6_sid_result_t))
    {
        rc = ((const srv6_sid_result_t *)resp->payload)->result;
    }
    dev_ipc_message_free(resp);
    return rc;
}

int srv6_rpc_sid_get(dev_ipc_context_t *ctx, const srv6_sid_key_t *key, srv6_sid_entry_t *entry_out,
                     uint32_t timeout_ms)
{
    if (!entry_out)
    {
        return ERRCODE_FAIL;
    }
    memset(entry_out, 0, sizeof(*entry_out));
    return srv6_rpc_sid_query(ctx, SRV6_MSG_TYPE_SID_GET, key, entry_out, timeout_ms);
}

int srv6_rpc_locator_exists(dev_ipc_context_t *ctx, const char *name, uint32_t timeout_ms)
{
    if (!ctx || !name || name[0] == '\0')
    {
        return ERRCODE_FAIL;
    }
    srv6_locator_query_t *payload = g_new0(srv6_locator_query_t, 1);
    g_strlcpy(payload->name, name, sizeof(payload->name));
    dev_ipc_message_t *msg = dev_ipc_message_create(SRV6_MSG_TYPE_LOCATOR_GET, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_SRV6, 0, payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }
    uint32_t wait_ms = timeout_ms ? timeout_ms : SRV6_RPC_DEFAULT_TIMEOUT_MS;
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_SRV6, msg, wait_ms);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }
    int rc = ERRCODE_FAIL;
    if (resp->msg_type == SRV6_MSG_TYPE_LOCATOR_RESULT && resp->payload &&
        resp->payload_len >= sizeof(srv6_locator_result_t))
    {
        const srv6_locator_result_t *result = resp->payload;
        rc = (result->result == ERRCODE_SUCCESS && result->found) ? ERRCODE_SUCCESS
             : (result->result == ERRCODE_SUCCESS)                ? ERRCODE_DEP_MISSING
                                                                  : result->result;
    }
    dev_ipc_message_free(resp);
    return rc;
}
