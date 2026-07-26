#include "rpm_bdr.h"

#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "rpm_cli.h"
#include "rpm_db.h"

static void rpm_bdr_append_policy(GString *out, const rpm_policy_t *policy, uint32_t only_sequence,
                                  gboolean sequence_filter)
{
    for (uint32_t i = 0; i < policy->node_count && i < RPM_POLICY_MAX_NODES; i++)
    {
        const rpm_policy_node_t *node = &policy->nodes[i];
        if (sequence_filter && node->sequence != only_sequence)
        {
            continue;
        }
        g_string_append_printf(out, "route-policy %s %s node %u\r\n", policy->name, node->permit ? "permit" : "deny",
                               node->sequence);
        if ((node->match_mask & RPM_MATCH_PREFIX) != 0u)
        {
            char addr[64];
            net_addr_to_str(&node->prefix.addr, addr, sizeof(addr));
            g_string_append_printf(out, " if-match network %s %s %u\r\n",
                                   node->prefix.addr.family == AF_INET ? "ipv4" : "ipv6", addr,
                                   (unsigned)node->prefix.prefix_len);
        }
        if ((node->apply_mask & RPM_APPLY_MED) != 0u)
        {
            g_string_append_printf(out, " apply med %u\r\n", node->med);
        }
        if ((node->apply_mask & RPM_APPLY_LOCAL_PREF) != 0u)
        {
            g_string_append_printf(out, " apply local-preference %u\r\n", node->local_pref);
        }
        if ((node->apply_mask & RPM_APPLY_COMMUNITY) != 0u)
        {
            g_string_append_printf(out, " apply community %s\r\n", node->community);
        }
    }
}

int rpm_bdr_handle_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse(msg->payload, msg->payload_len, &scope) != 0)
    {
        rpm_cli_send_response(msg, "");
        return ERRCODE_FAIL;
    }

    GString *out = g_string_new(NULL);
    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS && strcmp(scope.view_name, CLI_VIEW_ROUTE_POLICY) == 0)
    {
        char name[RPM_POLICY_NAME_MAX] = "";
        uint32_t sequence = 0;
        if (cli_ctx_lookup_text(scope.ctx_data, scope.ctx_len, CLI_CTX_ID_RPM_POLICY_NAME, name, sizeof(name)) == 0 &&
            cli_ctx_lookup_uint32(scope.ctx_data, scope.ctx_len, CLI_CTX_ID_RPM_NODE, &sequence) == 0)
        {
            rpm_policy_t policy;
            if (rpm_db_get_policy(name, &policy) == ERRCODE_SUCCESS)
            {
                rpm_bdr_append_policy(out, &policy, sequence, TRUE);
            }
        }
    }
    else if (scope.mode == CLI_SHOW_SCOPE_MODE_FULL)
    {
        GPtrArray *policies = NULL;
        if (rpm_db_list_policies(&policies) == ERRCODE_SUCCESS)
        {
            for (guint i = 0; i < policies->len; i++)
            {
                if (out->len == 0)
                {
                    g_string_append(out, "!\r\n");
                }
                rpm_bdr_append_policy(out, g_ptr_array_index(policies, i), 0, FALSE);
            }
            g_ptr_array_free(policies, TRUE);
        }
    }
    rpm_cli_send_response(msg, out->str);
    g_string_free(out, TRUE);
    return ERRCODE_SUCCESS;
}
