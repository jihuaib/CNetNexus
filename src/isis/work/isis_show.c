/**
 * @file   isis_show.c
 * @brief  ISIS show 命令处理（worker 线程）
 * @author jhb
 * @date   2026/04/12
 */
#include "isis_show.h"

#include <arpa/inet.h>

#include "cli.h"
#include "errcode.h"
#include "isis.h"
#include "isis_cli.h"
#include "isis_main.h"
#include "isis_worker.h"
#include "route.h"

static cli_chunk_stream_t g_isis_show_stream;

static const char *is_type_name(uint8_t is_type)
{
    switch (is_type)
    {
        case ISIS_IS_TYPE_LEVEL_1:
            return "level-1";
        case ISIS_IS_TYPE_LEVEL_2:
            return "level-2";
        case ISIS_IS_TYPE_LEVEL_1_2:
            return "level-1-2";
        default:
            return "unknown";
    }
}

typedef struct
{
    GString *buf;
    uint32_t tag_filter;
    int has_filter;
    uint16_t afi_filter;
    uint32_t count;
} show_instance_ctx_t;

typedef struct
{
    const isis_instance_cfg_t *inst;
    show_instance_ctx_t *show_ctx;
    uint32_t line_count;
} show_if_walk_ctx_t;

static int show_match_instance_filter(const isis_instance_cfg_t *inst, const show_instance_ctx_t *ctx)
{
    if (!inst || !ctx)
    {
        return 0;
    }
    if (ctx->has_filter && inst->tag != ctx->tag_filter)
    {
        return 0;
    }
    if (ctx->afi_filter == ROUTE_AFI_IPV4 && !inst->af_ipv4)
    {
        return 0;
    }
    if (ctx->afi_filter == ROUTE_AFI_IPV6 && !inst->af_ipv6)
    {
        return 0;
    }
    return 1;
}

static void show_summary_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    isis_instance_cfg_t *inst = (isis_instance_cfg_t *)value;
    show_instance_ctx_t *ctx = (show_instance_ctx_t *)user_data;
    if (!inst || !ctx || !ctx->buf)
    {
        return;
    }
    if (!show_match_instance_filter(inst, ctx))
    {
        return;
    }

    uint32_t route_count = 0u;
    if (inst->route_states)
    {
        route_count += (uint32_t)g_hash_table_size(inst->route_states);
    }
    if (inst->learned_routes)
    {
        route_count += (uint32_t)g_hash_table_size(inst->learned_routes);
    }

    g_string_append_printf(ctx->buf, "%-8u %-10s %-6u %-6u %-8u %-4u %-6u\r\n", inst->tag, is_type_name(inst->is_type),
                           inst->af_ipv4 ? 1u : 0u, inst->af_ipv6 ? 1u : 0u, inst->admin_up ? 1u : 0u,
                           g_hash_table_size(inst->if_cfgs), route_count);
    ctx->count++;
}

static void show_if_cfg_item(gpointer key, gpointer value, gpointer user_data)
{
    const char *ifname = (const char *)key;
    const isis_if_cfg_t *cfg = (const isis_if_cfg_t *)value;
    show_if_walk_ctx_t *walk = (show_if_walk_ctx_t *)user_data;

    if (!ifname || !cfg || !walk || !walk->show_ctx || !walk->show_ctx->buf || !walk->inst)
    {
        return;
    }

    GString *buf = walk->show_ctx->buf;
    if (cfg->v4.enabled && walk->inst->af_ipv4 &&
        (walk->show_ctx->afi_filter == 0u || walk->show_ctx->afi_filter == ROUTE_AFI_IPV4))
    {
        g_string_append_printf(buf, "  %-16s %-5s metric=%-8u hello=%-4u hold-mult=%-3u passive=%u\r\n", ifname, "ipv4",
                               cfg->v4.metric, (unsigned)cfg->v4.hello_interval, (unsigned)cfg->v4.hold_multiplier,
                               (unsigned)cfg->v4.passive);
        walk->line_count++;
    }
    if (cfg->v6.enabled && walk->inst->af_ipv6 &&
        (walk->show_ctx->afi_filter == 0u || walk->show_ctx->afi_filter == ROUTE_AFI_IPV6))
    {
        g_string_append_printf(buf, "  %-16s %-5s metric=%-8u hello=%-4u hold-mult=%-3u passive=%u\r\n", ifname, "ipv6",
                               cfg->v6.metric, (unsigned)cfg->v6.hello_interval, (unsigned)cfg->v6.hold_multiplier,
                               (unsigned)cfg->v6.passive);
        walk->line_count++;
    }
}

static void show_interface_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    isis_instance_cfg_t *inst = (isis_instance_cfg_t *)value;
    show_instance_ctx_t *ctx = (show_instance_ctx_t *)user_data;
    if (!inst || !ctx || !ctx->buf)
    {
        return;
    }
    if (!show_match_instance_filter(inst, ctx))
    {
        return;
    }

    g_string_append_printf(ctx->buf, "Tag %u (%s)\r\n", inst->tag, is_type_name(inst->is_type));
    if (g_hash_table_size(inst->if_cfgs) == 0)
    {
        g_string_append(ctx->buf, "  (no interfaces)\r\n");
    }
    else
    {
        show_if_walk_ctx_t walk = {
            .inst = inst,
            .show_ctx = ctx,
            .line_count = 0u,
        };
        g_hash_table_foreach(inst->if_cfgs, show_if_cfg_item, &walk);
        if (walk.line_count == 0u)
        {
            g_string_append(ctx->buf, "  (no interfaces)\r\n");
        }
    }
    ctx->count++;
}

static void parse_show_filter(cli_tlv_parser_t *parser, uint32_t *tag_out, int *has_tag_out, uint16_t *afi_out)
{
    *tag_out = 0u;
    *has_tag_out = 0;
    *afi_out = 0u;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry) && entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
        {
            uint32_t tag = cli_tlv_entry_get_ctx_uint32(&entry);
            if (tag != 0u)
            {
                *tag_out = tag;
                *has_tag_out = 1;
            }
        }
        else if (!CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == 1)
            {
                *afi_out = ROUTE_AFI_IPV4;
            }
            else if (entry.cfg_id == 2)
            {
                *afi_out = ROUTE_AFI_IPV6;
            }
            else if (entry.cfg_id == 3)
            {
                int64_t v = cli_tlv_entry_get_int(&entry);
                if (v > 0 && v <= 0xFFFFFFFFll)
                {
                    *tag_out = (uint32_t)v;
                    *has_tag_out = 1;
                }
            }
        }
        cli_tlv_entry_free(&entry);
    }
}

static int handle_show_summary(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t tag_filter = 0u;
    int has_filter = 0;
    uint16_t afi_filter = 0u;
    parse_show_filter(parser, &tag_filter, &has_filter, &afi_filter);

    GString *buf = g_string_new("");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    g_string_append(buf, "\r\nISIS Summary\r\n"
                         "Tag      IS-Type    IPv4   IPv6   AdminUp  Ifs  Routes\r\n"
                         "-------- ---------- ------ ------ -------- ---- ------\r\n");

    show_instance_ctx_t ctx = {
        .buf = buf,
        .tag_filter = tag_filter,
        .has_filter = has_filter,
        .afi_filter = afi_filter,
        .count = 0u,
    };

    if (g_isis_work_local && g_isis_work_local->instances)
    {
        g_hash_table_foreach(g_isis_work_local->instances, show_summary_cb, &ctx);
    }

    if (ctx.count == 0u)
    {
        g_string_append(buf, "(no entries)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u instance(s)\r\n", ctx.count);

    return cli_chunk_stream_start(&g_isis_show_stream, isis_local_ipc_ctx(), DEV_MODULE_ID_ISIS, msg, buf);
}

static int handle_show_interface(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t tag_filter = 0u;
    int has_filter = 0;
    uint16_t afi_filter = 0u;
    parse_show_filter(parser, &tag_filter, &has_filter, &afi_filter);

    GString *buf = g_string_new("\r\nISIS Interfaces\r\n");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    show_instance_ctx_t ctx = {
        .buf = buf,
        .tag_filter = tag_filter,
        .has_filter = has_filter,
        .afi_filter = afi_filter,
        .count = 0u,
    };

    if (g_isis_work_local && g_isis_work_local->instances)
    {
        g_hash_table_foreach(g_isis_work_local->instances, show_interface_cb, &ctx);
    }

    if (ctx.count == 0u)
    {
        g_string_append(buf, "(no entries)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u instance(s)\r\n", ctx.count);

    return cli_chunk_stream_start(&g_isis_show_stream, isis_local_ipc_ctx(), DEV_MODULE_ID_ISIS, msg, buf);
}

typedef struct show_neighbor_ctx
{
    GString *buf;
    uint32_t tag_filter;
    int has_filter;
    uint16_t afi_filter;
    uint32_t count;
    uint64_t now_msec;
} show_neighbor_ctx_t;

typedef struct show_neighbor_inst_ctx
{
    show_neighbor_ctx_t *ctx;
    const isis_instance_cfg_t *inst;
} show_neighbor_inst_ctx_t;

static const char *adj_state_name(uint8_t state)
{
    switch (state)
    {
        case ISIS_ADJ_STATE_INIT:
            return "Init";
        case ISIS_ADJ_STATE_UP:
            return "Up";
        case ISIS_ADJ_STATE_DOWN:
        default:
            return "Down";
    }
}

static void format_sysid(const uint8_t sysid[6], char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    if (!sysid)
    {
        g_strlcpy(buf, "0000.0000.0000", sz);
        return;
    }
    g_snprintf(buf, sz, "%02x%02x.%02x%02x.%02x%02x", sysid[0], sysid[1], sysid[2], sysid[3], sysid[4], sysid[5]);
}

static void show_neighbor_item_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_neighbor_t *nbr = (const isis_neighbor_t *)value;
    show_neighbor_inst_ctx_t *inst_ctx = (show_neighbor_inst_ctx_t *)user_data;
    if (!nbr || !inst_ctx || !inst_ctx->ctx || !inst_ctx->ctx->buf || !inst_ctx->inst)
    {
        return;
    }
    if (inst_ctx->ctx->afi_filter == ROUTE_AFI_IPV4 && nbr->ipv4_addr.family != AF_INET)
    {
        return;
    }
    if (inst_ctx->ctx->afi_filter == ROUTE_AFI_IPV6 && nbr->ipv6_addr.family != AF_INET6)
    {
        return;
    }

    char sysid[20] = {0};
    format_sysid(nbr->system_id, sysid, sizeof(sysid));
    uint64_t age_sec = 0u;
    if (inst_ctx->ctx->now_msec >= nbr->last_seen_msec)
    {
        age_sec = (inst_ctx->ctx->now_msec - nbr->last_seen_msec) / 1000u;
    }

    char ipv4[64] = "-";
    char ipv6[64] = "-";
    if (nbr->ipv4_addr.family == AF_INET)
    {
        net_addr_to_str(&nbr->ipv4_addr, ipv4, sizeof(ipv4));
    }
    if (nbr->ipv6_addr.family == AF_INET6)
    {
        net_addr_to_str(&nbr->ipv6_addr, ipv6, sizeof(ipv6));
    }

    g_string_append_printf(inst_ctx->ctx->buf, "%-8u %-16s L%-5u %-14s %-6s %-5u %-8llu %-15s %-39s\r\n",
                           inst_ctx->inst->tag, nbr->ifname, (unsigned)nbr->level, sysid, adj_state_name(nbr->state),
                           (unsigned)nbr->hold_time_sec, (unsigned long long)age_sec, ipv4, ipv6);
    inst_ctx->ctx->count++;
}

static void show_neighbor_instance_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_instance_cfg_t *inst = (const isis_instance_cfg_t *)value;
    show_neighbor_ctx_t *ctx = (show_neighbor_ctx_t *)user_data;
    if (!inst || !ctx || !ctx->buf)
    {
        return;
    }
    if ((ctx->has_filter && inst->tag != ctx->tag_filter) || (ctx->afi_filter == ROUTE_AFI_IPV4 && !inst->af_ipv4) ||
        (ctx->afi_filter == ROUTE_AFI_IPV6 && !inst->af_ipv6))
    {
        return;
    }
    if (!inst->neighbors || g_hash_table_size(inst->neighbors) == 0)
    {
        return;
    }

    show_neighbor_inst_ctx_t inst_ctx = {
        .ctx = ctx,
        .inst = inst,
    };
    g_hash_table_foreach(inst->neighbors, show_neighbor_item_cb, &inst_ctx);
}

static int handle_show_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t tag_filter = 0u;
    int has_filter = 0;
    uint16_t afi_filter = 0u;
    parse_show_filter(parser, &tag_filter, &has_filter, &afi_filter);

    GString *buf = g_string_new("");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    g_string_append(buf,
                    "\r\nISIS Neighbors\r\n"
                    "Tag      Interface        Level  System-ID      State  Hold  LastSeen IPv4            IPv6\r\n"
                    "-------- ---------------- ------ -------------- ------ ----- -------- --------------- "
                    "---------------------------------------\r\n");

    show_neighbor_ctx_t ctx = {
        .buf = buf,
        .tag_filter = tag_filter,
        .has_filter = has_filter,
        .afi_filter = afi_filter,
        .count = 0u,
        .now_msec = (uint64_t)(g_get_monotonic_time() / 1000u),
    };

    if (g_isis_work_local && g_isis_work_local->instances)
    {
        g_hash_table_foreach(g_isis_work_local->instances, show_neighbor_instance_cb, &ctx);
    }

    if (ctx.count == 0u)
    {
        g_string_append(buf, "(no entries)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u neighbor(s)\r\n", ctx.count);

    return cli_chunk_stream_start(&g_isis_show_stream, isis_local_ipc_ctx(), DEV_MODULE_ID_ISIS, msg, buf);
}

typedef struct show_lsdb_ctx
{
    GString *buf;
    uint32_t tag_filter;
    int has_filter;
    uint16_t afi_filter;
    uint32_t count;
    uint64_t now_msec;
} show_lsdb_ctx_t;

typedef struct show_lsdb_inst_ctx
{
    show_lsdb_ctx_t *ctx;
    const isis_instance_cfg_t *inst;
} show_lsdb_inst_ctx_t;

static void show_lsdb_item_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_lsdb_entry_t *entry = (const isis_lsdb_entry_t *)value;
    show_lsdb_inst_ctx_t *inst_ctx = (show_lsdb_inst_ctx_t *)user_data;
    if (!entry || !inst_ctx || !inst_ctx->ctx || !inst_ctx->ctx->buf || !inst_ctx->inst)
    {
        return;
    }
    if (inst_ctx->ctx->afi_filter == ROUTE_AFI_IPV4 && entry->ipv4_prefix_count == 0u)
    {
        return;
    }
    if (inst_ctx->ctx->afi_filter == ROUTE_AFI_IPV6 && entry->ipv6_prefix_count == 0u)
    {
        return;
    }

    char sysid[20] = {0};
    format_sysid(entry->system_id, sysid, sizeof(sysid));

    uint64_t age_sec = 0u;
    if (inst_ctx->ctx->now_msec >= entry->last_rx_msec)
    {
        age_sec = (inst_ctx->ctx->now_msec - entry->last_rx_msec) / 1000u;
    }

    g_string_append_printf(inst_ctx->ctx->buf, "%-8u %-16s L%-5u %-14s 0x%08x %-6u 0x%04x %-5u %-5u %-7llu\r\n",
                           inst_ctx->inst->tag, entry->rx_ifname, (unsigned)entry->level, sysid, entry->seq,
                           (unsigned)entry->lifetime_sec, (unsigned)entry->checksum, (unsigned)entry->ipv4_prefix_count,
                           (unsigned)entry->ipv6_prefix_count, (unsigned long long)age_sec);
    inst_ctx->ctx->count++;
}

static void show_lsdb_instance_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_instance_cfg_t *inst = (const isis_instance_cfg_t *)value;
    show_lsdb_ctx_t *ctx = (show_lsdb_ctx_t *)user_data;
    if (!inst || !ctx || !ctx->buf)
    {
        return;
    }
    if ((ctx->has_filter && inst->tag != ctx->tag_filter) || (ctx->afi_filter == ROUTE_AFI_IPV4 && !inst->af_ipv4) ||
        (ctx->afi_filter == ROUTE_AFI_IPV6 && !inst->af_ipv6))
    {
        return;
    }
    if (!inst->lsdb_entries || g_hash_table_size(inst->lsdb_entries) == 0)
    {
        return;
    }

    show_lsdb_inst_ctx_t inst_ctx = {
        .ctx = ctx,
        .inst = inst,
    };
    g_hash_table_foreach(inst->lsdb_entries, show_lsdb_item_cb, &inst_ctx);
}

static int handle_show_lsdb(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t tag_filter = 0u;
    int has_filter = 0;
    uint16_t afi_filter = 0u;
    parse_show_filter(parser, &tag_filter, &has_filter, &afi_filter);

    GString *buf = g_string_new("");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    g_string_append(buf,
                    "\r\nISIS LSDB\r\n"
                    "Tag      Rx-If            Level  System-ID      Seq        Life   Cksm   IPv4  IPv6  LastRx\r\n"
                    "-------- ---------------- ------ -------------- ---------- ------ ------ ----- ----- -------\r\n");

    show_lsdb_ctx_t ctx = {
        .buf = buf,
        .tag_filter = tag_filter,
        .has_filter = has_filter,
        .afi_filter = afi_filter,
        .count = 0u,
        .now_msec = (uint64_t)(g_get_monotonic_time() / 1000u),
    };

    if (g_isis_work_local && g_isis_work_local->instances)
    {
        g_hash_table_foreach(g_isis_work_local->instances, show_lsdb_instance_cb, &ctx);
    }

    if (ctx.count == 0u)
    {
        g_string_append(buf, "(no entries)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u LSP entry(s)\r\n", ctx.count);

    return cli_chunk_stream_start(&g_isis_show_stream, isis_local_ipc_ctx(), DEV_MODULE_ID_ISIS, msg, buf);
}

int isis_show_handle_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        int rc = cli_chunk_stream_continue(&g_isis_show_stream, isis_local_ipc_ctx(), DEV_MODULE_ID_ISIS, msg);
        dev_ipc_message_free(msg);
        return rc;
    }

    if (msg->msg_type != CLI_MSG_TYPE || !msg->payload)
    {
        dev_ipc_message_free(msg);
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        dev_ipc_message_free(msg);
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_SUCCESS;
    switch (parser.group_id)
    {
        case ISIS_CLI_GROUP_ID_SHOW_SUMMARY:
            rc = handle_show_summary(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_SHOW_IF:
            rc = handle_show_interface(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_SHOW_NEIGHBOR:
            rc = handle_show_neighbor(msg, &parser);
            break;
        case ISIS_CLI_GROUP_ID_SHOW_LSDB:
            rc = handle_show_lsdb(msg, &parser);
            break;
        default:
            rc = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    dev_ipc_message_free(msg);
    return rc;
}

void isis_show_cleanup(void)
{
    cli_chunk_stream_reset(&g_isis_show_stream);
}
