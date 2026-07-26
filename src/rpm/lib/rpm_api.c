#include <arpa/inet.h>
#include <string.h>

#include "errcode.h"
#include "rpm.h"

int rpm_api_subscribe(dev_ipc_context_t *ctx, uint32_t interest_mask, uint32_t flags)
{
    if (!ctx || interest_mask == 0u)
    {
        return ERRCODE_FAIL;
    }
    rpm_subscribe_req_t req = {
        .interest_mask = htonl(interest_mask),
        .flags = htonl(flags),
    };
    dev_ipc_message_t *msg = dev_ipc_message_create(RPM_MSG_TYPE_SUBSCRIBE, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_RPM, 0, &req, sizeof(req), NULL);
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_RPM, msg, 5000);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }
    int rc = resp->msg_type == RPM_MSG_TYPE_ACK ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    dev_ipc_message_free(resp);
    return rc;
}

int rpm_api_unsubscribe(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return ERRCODE_FAIL;
    }
    dev_ipc_message_t *msg = dev_ipc_message_create(RPM_MSG_TYPE_UNSUBSCRIBE, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_RPM, 0, NULL, 0, NULL);
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_RPM, msg, 5000);
    dev_ipc_message_free(msg);
    if (!resp)
    {
        return ERRCODE_FAIL;
    }
    int rc = resp->msg_type == RPM_MSG_TYPE_ACK ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    dev_ipc_message_free(resp);
    return rc;
}

int rpm_api_policy_get(dev_ipc_context_t *ctx, const char *name, rpm_policy_get_resp_t *out)
{
    if (!ctx || !name || !out)
    {
        return ERRCODE_FAIL;
    }
    rpm_policy_get_req_t req;
    memset(&req, 0, sizeof(req));
    g_strlcpy(req.name, name, sizeof(req.name));

    dev_ipc_message_t *msg = dev_ipc_message_create(RPM_MSG_TYPE_POLICY_GET, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_RPM, 0, &req, sizeof(req), NULL);
    dev_ipc_message_t *resp = dev_ipc_query(ctx, DEV_MODULE_ID_RPM, msg, 5000);
    dev_ipc_message_free(msg);
    if (!resp || resp->msg_type != RPM_MSG_TYPE_POLICY_GET || !resp->payload ||
        resp->payload_len < sizeof(rpm_policy_get_resp_t))
    {
        if (resp)
        {
            dev_ipc_message_free(resp);
        }
        return ERRCODE_FAIL;
    }
    memcpy(out, resp->payload, sizeof(*out));
    dev_ipc_message_free(resp);
    return ERRCODE_SUCCESS;
}

static bool rpm_prefix_contains(const net_prefix_t *rule, const net_prefix_t *route)
{
    if (!rule || !route || rule->addr.family != route->addr.family || route->prefix_len < rule->prefix_len)
    {
        return false;
    }
    const uint8_t *a =
        rule->addr.family == AF_INET ? (const uint8_t *)&rule->addr.u.v4 : (const uint8_t *)&rule->addr.u.v6;
    const uint8_t *b =
        route->addr.family == AF_INET ? (const uint8_t *)&route->addr.u.v4 : (const uint8_t *)&route->addr.u.v6;
    uint8_t whole = rule->prefix_len / 8u;
    uint8_t rem = rule->prefix_len % 8u;
    if (whole > 0u && memcmp(a, b, whole) != 0)
    {
        return false;
    }
    if (rem > 0u)
    {
        uint8_t mask = (uint8_t)(0xFFu << (8u - rem));
        if ((a[whole] & mask) != (b[whole] & mask))
        {
            return false;
        }
    }
    return true;
}

rpm_policy_decision_t rpm_policy_evaluate(const rpm_policy_t *policy, const net_prefix_t *prefix,
                                          rpm_policy_result_t *result)
{
    if (result)
    {
        memset(result, 0, sizeof(*result));
    }
    if (!policy)
    {
        return RPM_POLICY_DECISION_DENY;
    }
    uint32_t count = policy->node_count;
    if (count > RPM_POLICY_MAX_NODES)
    {
        count = RPM_POLICY_MAX_NODES;
    }
    for (uint32_t i = 0; i < count; i++)
    {
        const rpm_policy_node_t *node = &policy->nodes[i];
        bool matched = true;
        if ((node->match_mask & RPM_MATCH_PREFIX) != 0u)
        {
            matched = rpm_prefix_contains(&node->prefix, prefix);
        }
        if (!matched)
        {
            continue;
        }
        if (!node->permit)
        {
            return RPM_POLICY_DECISION_DENY;
        }
        if (result)
        {
            result->apply_mask = node->apply_mask;
            result->local_pref = node->local_pref;
            result->med = node->med;
            g_strlcpy(result->community, node->community, sizeof(result->community));
        }
        return RPM_POLICY_DECISION_PERMIT;
    }
    return RPM_POLICY_DECISION_DENY;
}
