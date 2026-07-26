#include "rpm_cli.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "rpm.h"
#include "rpm_db.h"
#include "rpm_main.h"

typedef struct rpm_cli_ctx
{
    char name[RPM_POLICY_NAME_MAX];
    uint32_t sequence;
} rpm_cli_ctx_t;

void rpm_cli_send_response(dev_ipc_message_t *msg, const char *text)
{
    char *payload = g_strdup(text ? text : "");
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_RPM, msg->src_module_id,
                                                     msg->request_id, payload, strlen(payload) + 1u, g_free);
    if (resp)
    {
        dev_ipc_send_response(rpm_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static void rpm_cli_parse_context(rpm_cli_ctx_t *ctx, const cli_tlv_entry_t *entry)
{
    if (entry->cfg_id == CLI_CTX_ID_RPM_POLICY_NAME && entry->type == CLI_TLV_TYPE_CTX_STR)
    {
        uint16_t len = entry->length;
        if (len >= sizeof(ctx->name))
        {
            len = sizeof(ctx->name) - 1u;
        }
        memcpy(ctx->name, entry->value, len);
        ctx->name[len] = '\0';
    }
    else if (entry->cfg_id == CLI_CTX_ID_RPM_NODE)
    {
        ctx->sequence = cli_tlv_entry_get_ctx_uint32(entry);
    }
}

static rpm_policy_node_t *rpm_cli_find_node(rpm_policy_t *policy, uint32_t sequence)
{
    for (uint32_t i = 0; i < policy->node_count && i < RPM_POLICY_MAX_NODES; i++)
    {
        if (policy->nodes[i].sequence == sequence)
        {
            return &policy->nodes[i];
        }
    }
    return NULL;
}

static void rpm_cli_reload_publish(const char *name)
{
    rpm_policy_t policy;
    if (rpm_db_get_policy(name, &policy) == ERRCODE_SUCCESS)
    {
        rpm_policy_store(&policy);
        rpm_policy_publish(RPM_POLICY_EVENT_UPSERT, &policy);
    }
}

static int rpm_cli_handle_policy(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char name[RPM_POLICY_NAME_MAX] = "";
    uint32_t sequence = 0;
    gboolean has_sequence = FALSE;
    gboolean permit = TRUE;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == 1)
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(name, text, sizeof(name));
                }
            }
            else if (entry.cfg_id == 2)
            {
                permit = TRUE;
            }
            else if (entry.cfg_id == 3)
            {
                permit = FALSE;
            }
            else if (entry.cfg_id == 4)
            {
                sequence = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_sequence = TRUE;
            }
        }
        cli_tlv_entry_free(&entry);
    }
    if (name[0] == '\0')
    {
        rpm_cli_send_response(msg, "RPM Error: policy name is required.\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        rpm_policy_t old;
        int found = rpm_db_get_policy(name, &old);
        int rc = has_sequence ? rpm_db_delete_node(name, sequence) : rpm_db_delete_policy(name);
        if (rc < 0)
        {
            rpm_cli_send_response(msg, "RPM Error: database delete failed.\r\n");
            return ERRCODE_FAIL;
        }
        if (has_sequence)
        {
            rpm_cli_reload_publish(name);
        }
        else if (found == ERRCODE_SUCCESS)
        {
            rpm_policy_remove(name);
            rpm_policy_publish(RPM_POLICY_EVENT_DELETE, &old);
        }
        rpm_cli_send_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    rpm_policy_node_t node;
    memset(&node, 0, sizeof(node));
    node.sequence = sequence;
    node.permit = permit;
    if (!has_sequence || rpm_db_upsert_node(name, &node) != ERRCODE_SUCCESS)
    {
        rpm_cli_send_response(msg, "RPM Error: failed to create policy node.\r\n");
        return ERRCODE_FAIL;
    }
    rpm_cli_reload_publish(name);
    rpm_cli_send_response(msg, "");
    return ERRCODE_SUCCESS;
}

static int rpm_cli_load_context_node(dev_ipc_message_t *msg, cli_tlv_parser_t *parser, rpm_cli_ctx_t *ctx,
                                     rpm_policy_t *policy, rpm_policy_node_t **node, cli_tlv_entry_t **entries,
                                     guint *entry_count)
{
    memset(ctx, 0, sizeof(*ctx));
    *entries = NULL;
    *entry_count = 0;
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(cli_tlv_entry_t));
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            rpm_cli_parse_context(ctx, &entry);
            cli_tlv_entry_free(&entry);
        }
        else
        {
            g_array_append_val(arr, entry);
        }
    }
    if (ctx->name[0] == '\0' || rpm_db_get_policy(ctx->name, policy) != ERRCODE_SUCCESS ||
        !(*node = rpm_cli_find_node(policy, ctx->sequence)))
    {
        for (guint i = 0; i < arr->len; i++)
        {
            cli_tlv_entry_free(&g_array_index(arr, cli_tlv_entry_t, i));
        }
        g_array_free(arr, TRUE);
        rpm_cli_send_response(msg, "RPM Error: policy node does not exist.\r\n");
        return ERRCODE_FAIL;
    }
    *entry_count = arr->len;
    *entries = (cli_tlv_entry_t *)g_array_free(arr, FALSE);
    return ERRCODE_SUCCESS;
}

static void rpm_cli_free_entries(cli_tlv_entry_t *entries, guint count)
{
    for (guint i = 0; i < count; i++)
    {
        cli_tlv_entry_free(&entries[i]);
    }
    g_free(entries);
}

static int rpm_cli_commit_node(dev_ipc_message_t *msg, const rpm_cli_ctx_t *ctx, const rpm_policy_node_t *node)
{
    if (rpm_db_update_node(ctx->name, node) != ERRCODE_SUCCESS)
    {
        rpm_cli_send_response(msg, "RPM Error: database update failed.\r\n");
        return ERRCODE_FAIL;
    }
    rpm_cli_reload_publish(ctx->name);
    rpm_cli_send_response(msg, "");
    return ERRCODE_SUCCESS;
}

static int rpm_cli_handle_if_match(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    rpm_cli_ctx_t ctx;
    rpm_policy_t policy;
    rpm_policy_node_t *stored;
    cli_tlv_entry_t *entries;
    guint count;
    if (rpm_cli_load_context_node(msg, parser, &ctx, &policy, &stored, &entries, &count) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    rpm_policy_node_t node = *stored;
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    if (is_no)
    {
        node.match_mask &= ~RPM_MATCH_PREFIX;
        memset(&node.prefix, 0, sizeof(node.prefix));
    }
    else
    {
        char addr[64] = "";
        uint32_t prefix_len = UINT32_MAX;
        for (guint i = 0; i < count; i++)
        {
            if (entries[i].cfg_id == 1 || entries[i].cfg_id == 2)
            {
                const char *text = cli_tlv_entry_get_text(&entries[i]);
                if (text)
                {
                    g_strlcpy(addr, text, sizeof(addr));
                }
            }
            else if (entries[i].cfg_id == 3 || entries[i].cfg_id == 4)
            {
                prefix_len = (uint32_t)cli_tlv_entry_get_int(&entries[i]);
            }
        }
        if (net_addr_from_str(addr, &node.prefix.addr) != 0 ||
            (node.prefix.addr.family == AF_INET && prefix_len > 32u) ||
            (node.prefix.addr.family == AF_INET6 && prefix_len > 128u))
        {
            rpm_cli_free_entries(entries, count);
            rpm_cli_send_response(msg, "RPM Error: invalid match prefix.\r\n");
            return ERRCODE_FAIL;
        }
        node.prefix.prefix_len = (uint8_t)prefix_len;
        node.match_mask |= RPM_MATCH_PREFIX;
    }
    rpm_cli_free_entries(entries, count);
    return rpm_cli_commit_node(msg, &ctx, &node);
}

static int rpm_cli_handle_apply(dev_ipc_message_t *msg, cli_tlv_parser_t *parser, uint32_t action)
{
    rpm_cli_ctx_t ctx;
    rpm_policy_t policy;
    rpm_policy_node_t *stored;
    cli_tlv_entry_t *entries;
    guint count;
    if (rpm_cli_load_context_node(msg, parser, &ctx, &policy, &stored, &entries, &count) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    rpm_policy_node_t node = *stored;
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    if (is_no)
    {
        node.apply_mask &= ~action;
        if (action == RPM_APPLY_COMMUNITY)
        {
            node.community[0] = '\0';
        }
    }
    else
    {
        for (guint i = 0; i < count; i++)
        {
            if (action == RPM_APPLY_MED && entries[i].cfg_id == 1)
            {
                node.med = (uint32_t)cli_tlv_entry_get_int(&entries[i]);
            }
            else if (action == RPM_APPLY_LOCAL_PREF && entries[i].cfg_id == 1)
            {
                node.local_pref = (uint32_t)cli_tlv_entry_get_int(&entries[i]);
            }
            else if (action == RPM_APPLY_COMMUNITY && entries[i].cfg_id == 1)
            {
                const char *text = cli_tlv_entry_get_text(&entries[i]);
                if (text)
                {
                    unsigned long asn, value;
                    char tail;
                    if (sscanf(text, "%lu:%lu%c", &asn, &value, &tail) != 2 || asn > 65535ul || value > 65535ul)
                    {
                        rpm_cli_free_entries(entries, count);
                        rpm_cli_send_response(msg, "RPM Error: community must be ASN:VALUE (0-65535 each).\r\n");
                        return ERRCODE_FAIL;
                    }
                    g_strlcpy(node.community, text, sizeof(node.community));
                }
            }
        }
        node.apply_mask |= action;
    }
    rpm_cli_free_entries(entries, count);
    return rpm_cli_commit_node(msg, &ctx, &node);
}

int rpm_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }
    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, msg->payload, msg->payload_len) != 0)
    {
        rpm_cli_send_response(msg, "RPM Error: invalid command payload.\r\n");
        return ERRCODE_FAIL;
    }
    int rc;
    switch (parser.group_id)
    {
        case RPM_CLI_GROUP_POLICY:
            rc = rpm_cli_handle_policy(msg, &parser);
            break;
        case RPM_CLI_GROUP_IF_MATCH_PREFIX:
            rc = rpm_cli_handle_if_match(msg, &parser);
            break;
        case RPM_CLI_GROUP_APPLY_MED:
            rc = rpm_cli_handle_apply(msg, &parser, RPM_APPLY_MED);
            break;
        case RPM_CLI_GROUP_APPLY_LOCAL_PREF:
            rc = rpm_cli_handle_apply(msg, &parser, RPM_APPLY_LOCAL_PREF);
            break;
        case RPM_CLI_GROUP_APPLY_COMMUNITY:
            rc = rpm_cli_handle_apply(msg, &parser, RPM_APPLY_COMMUNITY);
            break;
        default:
            rpm_cli_send_response(msg, "RPM Error: unknown command.\r\n");
            rc = ERRCODE_FAIL;
            break;
    }
    cli_tlv_cleanup(&parser);
    return rc;
}

void rpm_cli_handle_candidates(dev_ipc_message_t *msg)
{
    uint32_t query_id = 0;
    if (msg->payload && msg->payload_len >= sizeof(uint32_t))
    {
        memcpy(&query_id, msg->payload, sizeof(query_id));
        query_id = ntohl(query_id);
    }
    GByteArray *buf = g_byte_array_new();
    g_mutex_lock(&g_rpm_local->lock);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, g_rpm_local->policies);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const rpm_policy_t *policy = value;
        if (query_id != RPM_CANDIDATE_QUERY_ROUTE_POLICY)
        {
            continue;
        }
        g_byte_array_append(buf, (const guint8 *)policy->name, strlen(policy->name) + 1u);
    }
    g_mutex_unlock(&g_rpm_local->lock);
    guint8 nul = 0;
    g_byte_array_append(buf, &nul, 1);
    guint len = buf->len;
    uint8_t *payload = g_byte_array_free(buf, FALSE);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_QUERY_CANDIDATES_RESP, DEV_MODULE_ID_RPM,
                                                     msg->src_module_id, msg->request_id, payload, len, g_free);
    if (resp)
    {
        dev_ipc_send_response(rpm_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(payload);
    }
}
