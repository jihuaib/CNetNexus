/**
 * @file   isis_show.c
 * @brief  ISIS show 命令处理（worker 线程）
 * @author jhb
 * @date   2026/04/12
 */
#include "isis_show.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "if_api.h"
#include "isis.h"
#include "isis_cli.h"
#include "isis_main.h"
#include "isis_route.h"
#include "isis_worker.h"
#include "route.h"

/* TLV 类型（与 lsp/spf 模块保持一致） */
#define ISIS_TLV_EXT_IS_REACH 22u
#define ISIS_TLV_EXT_IP_REACH 135u
#define ISIS_TLV_IPV6_REACH 236u

static cli_chunk_stream_t g_isis_show_stream;
static int show_send_simple_resp(dev_ipc_message_t *msg, const char *text);

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

static void show_summary_head_count_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const isis_route_head_t *head = (const isis_route_head_t *)value;
    uint32_t *total = (uint32_t *)user_data;
    if (!head || !total)
    {
        return;
    }
    *total += head->path_count;
}

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
    if (inst->learned_route_heads)
    {
        g_hash_table_foreach(inst->learned_route_heads, show_summary_head_count_cb, &route_count);
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
    int verbose;
    uint32_t count;
    uint64_t now_msec;
} show_neighbor_ctx_t;

typedef struct show_neighbor_inst_ctx
{
    show_neighbor_ctx_t *ctx;
    const isis_instance_cfg_t *inst;
} show_neighbor_inst_ctx_t;

typedef struct show_neighbor_filter
{
    uint32_t tag_filter;
    int has_filter;
    int verbose;
} show_neighbor_filter_t;

static void parse_show_neighbor_filter(cli_tlv_parser_t *parser, show_neighbor_filter_t *filter)
{
    if (!parser || !filter)
    {
        return;
    }

    memset(filter, 0, sizeof(*filter));
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
            {
                uint32_t tag = cli_tlv_entry_get_ctx_uint32(&entry);
                if (tag != 0u)
                {
                    filter->tag_filter = tag;
                    filter->has_filter = 1;
                }
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id == 3u)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v > 0 && v <= 0xFFFFFFFFll)
            {
                filter->tag_filter = (uint32_t)v;
                filter->has_filter = 1;
            }
        }
        else if (entry.cfg_id == 4u)
        {
            filter->verbose = 1;
        }
        cli_tlv_entry_free(&entry);
    }
}

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

static const char *afi_name(uint16_t afi)
{
    switch (afi)
    {
        case ROUTE_AFI_IPV4:
            return "ipv4";
        case ROUTE_AFI_IPV6:
            return "ipv6";
        default:
            return "unknown";
    }
}

static const char *bool_name(int value)
{
    return value ? "yes" : "no";
}

static const char *circuit_type_name(uint8_t circuit_type)
{
    switch (circuit_type)
    {
        case 1u:
            return "L1";
        case 2u:
            return "L2";
        case 3u:
            return "L1L2";
        default:
            return "unknown";
    }
}

static void format_mac_or_dash(const uint8_t *mac, size_t len, char *buf, size_t sz)
{
    if (!buf || sz == 0u)
    {
        return;
    }

    g_strlcpy(buf, "-", sz);
    if (!mac || len < 6u)
    {
        return;
    }

    int non_zero = 0;
    for (size_t i = 0u; i < 6u; ++i)
    {
        if (mac[i] != 0u)
        {
            non_zero = 1;
            break;
        }
    }
    if (!non_zero)
    {
        return;
    }

    g_snprintf(buf, sz, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int show_level_enabled(const isis_instance_cfg_t *inst, uint8_t level)
{
    if (!inst)
    {
        return 0;
    }
    if (level == 1u)
    {
        return (inst->is_type == ISIS_IS_TYPE_LEVEL_1 || inst->is_type == ISIS_IS_TYPE_LEVEL_1_2) ? 1 : 0;
    }
    if (level == 2u)
    {
        return (inst->is_type == ISIS_IS_TYPE_LEVEL_2 || inst->is_type == ISIS_IS_TYPE_LEVEL_1_2) ? 1 : 0;
    }
    return 0;
}

static void format_addr_or_dash(const net_addr_t *addr, char *buf, size_t sz)
{
    if (!buf || sz == 0u)
    {
        return;
    }

    g_strlcpy(buf, "-", sz);
    if (!addr)
    {
        return;
    }

    if (addr->family == AF_INET || addr->family == AF_INET6)
    {
        net_addr_to_str(addr, buf, sz);
    }
}

static void ifindex_to_name(uint32_t ifindex, char *buf, size_t sz)
{
    if (!buf || sz == 0u)
    {
        return;
    }

    g_strlcpy(buf, "-", sz);
    if (ifindex == 0u)
    {
        return;
    }

    const char *logical = if_api_cache_get_logical_name(ifindex);
    if (logical && logical[0] != '\0')
    {
        g_strlcpy(buf, logical, sz);
        return;
    }

    char os_name[IFNAMSIZ] = {0};
    if (if_indextoname(ifindex, os_name))
    {
        g_strlcpy(buf, os_name, sz);
    }
}

static void format_oif(uint32_t ifindex, char *buf, size_t sz)
{
    if (!buf || sz == 0u)
    {
        return;
    }
    if (ifindex == 0u)
    {
        g_strlcpy(buf, "-", sz);
        return;
    }

    char ifname[IFNAMSIZ] = {0};
    ifindex_to_name(ifindex, ifname, sizeof(ifname));
    g_snprintf(buf, sz, "%s(%u)", ifname, ifindex);
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

    char sysid[20] = {0};
    format_sysid(nbr->system_id, sysid, sizeof(sysid));
    uint64_t age_sec = 0u;
    if (inst_ctx->ctx->now_msec >= nbr->last_seen_msec)
    {
        age_sec = (inst_ctx->ctx->now_msec - nbr->last_seen_msec) / 1000u;
    }

    char ipv4[64] = "-";
    char ipv6[64] = "-";
    format_addr_or_dash(&nbr->ipv4_addr, ipv4, sizeof(ipv4));
    format_addr_or_dash(&nbr->ipv6_addr, ipv6, sizeof(ipv6));

    if (!inst_ctx->ctx->verbose)
    {
        g_string_append_printf(inst_ctx->ctx->buf, "%-8u %-16s L%-5u %-14s %-6s %-5s %-8s %-5u %-8llu %-15s %-39s\r\n",
                               inst_ctx->inst->tag, nbr->ifname, (unsigned)nbr->level, sysid,
                               adj_state_name(nbr->state), bool_name(nbr->hello_valid), bool_name(nbr->seen_self),
                               (unsigned)nbr->hold_time_sec, (unsigned long long)age_sec, ipv4, ipv6);
        inst_ctx->ctx->count++;
        return;
    }

    const isis_if_cfg_t *if_cfg = NULL;
    if (inst_ctx->inst->if_cfgs)
    {
        if_cfg = (const isis_if_cfg_t *)g_hash_table_lookup(inst_ctx->inst->if_cfgs, nbr->ifname);
    }
    const isis_if_af_cfg_t *cfg_v4 = isis_if_cfg_af_const(if_cfg, ROUTE_AFI_IPV4);
    const isis_if_af_cfg_t *cfg_v6 = isis_if_cfg_af_const(if_cfg, ROUTE_AFI_IPV6);
    const if_api_cache_entry_t *if_entry = if_api_cache_lookup(nbr->ifname);

    int inst_admin = (inst_ctx->inst->admin_up != 0u);
    int level_enabled = show_level_enabled(inst_ctx->inst, nbr->level);
    int if_admin = (if_entry && if_entry->proto_up != 0u && if_entry->ifindex != 0u) ? 1 : 0;

    int inst_v4 = (inst_ctx->inst->af_ipv4 != 0u);
    int if_v4 = (cfg_v4 && cfg_v4->enabled) ? 1 : 0;
    int passive_v4 = (cfg_v4 && cfg_v4->passive) ? 1 : 0;
    int remote_nlpid_v4 = (nbr->remote_ipv4_nlpid != 0u);
    int remote_v4 = (nbr->ipv4_addr.family == AF_INET) ? 1 : 0;
    int negotiated_v4 = (nbr->state == ISIS_ADJ_STATE_UP && nbr->hello_valid && inst_admin && level_enabled &&
                         if_admin && inst_v4 && if_v4 && !passive_v4 && remote_nlpid_v4 && remote_v4);

    int inst_v6 = (inst_ctx->inst->af_ipv6 != 0u);
    int if_v6 = (cfg_v6 && cfg_v6->enabled) ? 1 : 0;
    int passive_v6 = (cfg_v6 && cfg_v6->passive) ? 1 : 0;
    int remote_nlpid_v6 = (nbr->remote_ipv6_nlpid != 0u);
    int remote_v6 = (nbr->ipv6_addr.family == AF_INET6) ? 1 : 0;
    int negotiated_v6 = (nbr->state == ISIS_ADJ_STATE_UP && nbr->hello_valid && inst_admin && level_enabled &&
                         if_admin && inst_v6 && if_v6 && !passive_v6 && remote_nlpid_v6 && remote_v6);

    char local_v4[64] = "-";
    char local_v6[64] = "-";
    if (if_entry)
    {
        format_addr_or_dash(&if_entry->ipv4_addr, local_v4, sizeof(local_v4));
        format_addr_or_dash(&if_entry->ipv6_addr, local_v6, sizeof(local_v6));
    }

    char ifname[IFNAMSIZ] = "-";
    uint32_t ifindex = 0u;
    if (if_entry)
    {
        ifindex = if_entry->ifindex;
        ifindex_to_name(ifindex, ifname, sizeof(ifname));
    }

    uint64_t lsp_age_sec = 0u;
    if (nbr->last_lsp_rx_msec > 0u && inst_ctx->ctx->now_msec >= nbr->last_lsp_rx_msec)
    {
        lsp_age_sec = (inst_ctx->ctx->now_msec - nbr->last_lsp_rx_msec) / 1000u;
    }

    char local_snpa[32] = "-";
    char remote_snpa[32] = "-";
    format_mac_or_dash(nbr->local_snpa, sizeof(nbr->local_snpa), local_snpa, sizeof(local_snpa));
    format_mac_or_dash(nbr->remote_snpa, sizeof(nbr->remote_snpa), remote_snpa, sizeof(remote_snpa));

    inst_ctx->ctx->count++;
    g_string_append_printf(
        inst_ctx->ctx->buf,
        "Neighbor %u\r\n"
        "  Tag            : %u\r\n"
        "  Interface      : %s (ifindex=%u, resolved=%s)\r\n"
        "  Level          : L%u (enabled=%s)\r\n"
        "  System-ID      : %s\r\n"
        "  State          : %s\r\n"
        "  Hold(sec)      : %u\r\n"
        "  LastSeen(sec)  : %llu\r\n"
        "  Priority       : %u\r\n"
        "  Hello Valid    : %s\r\n"
        "  Seen Self      : %s\r\n"
        "  SNPA           : local=%s, remote=%s\r\n"
        "  Circuit-Type   : remote=%s, level-ok=%s\r\n"
        "  Area Match     : %s\r\n"
        "  Hold Valid     : %s\r\n"
        "  NLPIDs         : remote-v4=%s, remote-v6=%s, compatible=%s\r\n"
        "  LastLSP        : seq=0x%08x, age=%llus\r\n"
        "  Instance Admin : %s\r\n"
        "  Interface Up   : %s\r\n"
        "  IPv4           : inst-af=%s, if-enable=%s, passive=%s, remote-nlpid=%s, remote-adv=%s, negotiated=%s\r\n"
        "                   local=%s, remote=%s\r\n"
        "  IPv6           : inst-af=%s, if-enable=%s, passive=%s, remote-nlpid=%s, remote-adv=%s, negotiated=%s\r\n"
        "                   local=%s, remote=%s\r\n\r\n",
        inst_ctx->ctx->count, inst_ctx->inst->tag, ifname, ifindex, bool_name(if_entry != NULL), (unsigned)nbr->level,
        bool_name(level_enabled), sysid, adj_state_name(nbr->state), (unsigned)nbr->hold_time_sec,
        (unsigned long long)age_sec, (unsigned)nbr->priority, bool_name(nbr->hello_valid), bool_name(nbr->seen_self),
        local_snpa, remote_snpa, circuit_type_name(nbr->remote_circuit_type), bool_name(nbr->circuit_ok),
        bool_name(nbr->area_match), bool_name(nbr->hold_ok), bool_name(remote_nlpid_v4), bool_name(remote_nlpid_v6),
        bool_name(nbr->nlpids_ok), (unsigned)nbr->last_lsp_seq, (unsigned long long)lsp_age_sec, bool_name(inst_admin),
        bool_name(if_admin), bool_name(inst_v4), bool_name(if_v4), bool_name(passive_v4), bool_name(remote_nlpid_v4),
        bool_name(remote_v4), bool_name(negotiated_v4), local_v4, ipv4, bool_name(inst_v6), bool_name(if_v6),
        bool_name(passive_v6), bool_name(remote_nlpid_v6), bool_name(remote_v6), bool_name(negotiated_v6), local_v6,
        ipv6);
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
    if (ctx->has_filter && inst->tag != ctx->tag_filter)
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
    show_neighbor_filter_t filter;
    parse_show_neighbor_filter(parser, &filter);
    if (!filter.has_filter)
    {
        return show_send_simple_resp(msg, "Error: tag is required\r\n");
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    if (filter.verbose)
    {
        g_string_append(buf, "\r\nISIS Neighbors Verbose\r\n");
    }
    else
    {
        g_string_append(
            buf, "\r\nISIS Neighbors\r\n"
                 "Tag      Interface        Level  System-ID      State  Valid SeenSelf Hold  LastSeen IPv4            "
                 "IPv6\r\n"
                 "-------- ---------------- ------ -------------- ------ ----- -------- ----- -------- --------------- "
                 "---------------------------------------\r\n");
    }

    show_neighbor_ctx_t ctx = {
        .buf = buf,
        .tag_filter = filter.tag_filter,
        .has_filter = filter.has_filter,
        .verbose = filter.verbose,
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

typedef struct isis_lsdb_parse_stats
{
    uint32_t is_count;
    uint32_t v4_count;
    uint32_t v6_count;
    uint32_t tlv_count;
} isis_lsdb_parse_stats_t;

typedef struct show_lsdb_filter
{
    uint32_t tag_filter;
    int has_filter;
    uint16_t afi_filter;
} show_lsdb_filter_t;

static const char *lsdb_tlv_name(uint8_t tlv_type)
{
    switch (tlv_type)
    {
        case ISIS_TLV_EXT_IS_REACH:
            return "Extended-IS-Reachability";
        case ISIS_TLV_EXT_IP_REACH:
            return "Extended-IP-Reachability(IPv4)";
        case ISIS_TLV_IPV6_REACH:
            return "IPv6-Reachability";
        default:
            return "Unknown";
    }
}

static void lsdb_append_hex_bytes(GString *buf, const uint8_t *data, size_t len, size_t max_bytes)
{
    if (!buf)
    {
        return;
    }
    if (!data || len == 0u)
    {
        g_string_append(buf, "-");
        return;
    }

    size_t shown = (len > max_bytes) ? max_bytes : len;
    for (size_t i = 0u; i < shown; ++i)
    {
        g_string_append_printf(buf, "%02x", data[i]);
        if (i + 1u < shown)
        {
            g_string_append_c(buf, ' ');
        }
    }
    if (len > shown)
    {
        size_t remain = len - shown;
        g_string_append_printf(buf, " ...(+%zu byte%s)", remain, (remain == 1u) ? "" : "s");
    }
}

static int lsdb_format_prefix(uint16_t afi, const uint8_t *bytes, uint8_t prefix_len, char *buf, size_t sz)
{
    if (!bytes || !buf || sz == 0u)
    {
        return -1;
    }

    net_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    uint8_t pfx_bytes = (uint8_t)((prefix_len + 7u) / 8u);
    if (afi == ROUTE_AFI_IPV4)
    {
        if (prefix_len > 32u || pfx_bytes > 4u)
        {
            return -1;
        }
        addr.family = AF_INET;
        if (pfx_bytes > 0u)
        {
            memcpy(&addr.u.v4.s_addr, bytes, pfx_bytes);
        }
    }
    else if (afi == ROUTE_AFI_IPV6)
    {
        if (prefix_len > 128u || pfx_bytes > 16u)
        {
            return -1;
        }
        addr.family = AF_INET6;
        if (pfx_bytes > 0u)
        {
            memcpy(addr.u.v6.s6_addr, bytes, pfx_bytes);
        }
    }
    else
    {
        return -1;
    }

    if (net_addr_prefix_normalize(&addr, prefix_len) != 0)
    {
        return -1;
    }

    char ip_buf[64] = {0};
    net_addr_to_str(&addr, ip_buf, sizeof(ip_buf));
    g_snprintf(buf, sz, "%s/%u", ip_buf, (unsigned)prefix_len);
    return 0;
}

static void lsdb_parse_sub_tlvs(GString *buf, const uint8_t *data, size_t data_len)
{
    if (!buf || !data || data_len == 0u)
    {
        return;
    }

    size_t pos = 0u;
    uint32_t sub_idx = 0u;
    while (pos + 2u <= data_len)
    {
        uint8_t sub_tlv_type = data[pos];
        uint8_t sub_tlv_len = data[pos + 1u];
        pos += 2u;
        if (pos + sub_tlv_len > data_len)
        {
            g_string_append_printf(buf, "      SubTLV       : parse-error (type=%u len=%u truncated)\r\n",
                                   (unsigned)sub_tlv_type, (unsigned)sub_tlv_len);
            break;
        }

        sub_idx++;
        g_string_append_printf(buf, "      SubTLV[%u]    : type=%u len=%u value=", sub_idx, (unsigned)sub_tlv_type,
                               (unsigned)sub_tlv_len);
        lsdb_append_hex_bytes(buf, &data[pos], sub_tlv_len, 24u);
        g_string_append(buf, "\r\n");
        pos += sub_tlv_len;
    }

    if (pos < data_len)
    {
        g_string_append_printf(buf, "      SubTLV       : trailing-bytes=%zu\r\n", data_len - pos);
    }
}

static void lsdb_parse_ext_is_tlv(GString *buf, const uint8_t *val, size_t val_len, isis_lsdb_parse_stats_t *stats)
{
    if (!buf || !val || val_len == 0u || !stats)
    {
        return;
    }

    size_t pos = 0u;
    uint32_t idx = 0u;
    while (pos + 11u <= val_len)
    {
        const uint8_t *neighbor_id = &val[pos];
        uint8_t pseudo = val[pos + 6u];
        uint32_t metric = ((uint32_t)val[pos + 7u] << 16) | ((uint32_t)val[pos + 8u] << 8) | (uint32_t)val[pos + 9u];
        uint8_t sub_tlv_len = val[pos + 10u];
        size_t entry_len = 11u + (size_t)sub_tlv_len;
        if (pos + entry_len > val_len)
        {
            g_string_append_printf(buf, "    IS            : parse-error (entry len=%zu truncated)\r\n", entry_len);
            break;
        }

        idx++;
        stats->is_count++;

        char nbr_sysid[20] = {0};
        format_sysid(neighbor_id, nbr_sysid, sizeof(nbr_sysid));
        g_string_append_printf(buf, "    IS[%u]         : neighbor=%s pseudo=%u metric=%u subtlv-len=%u\r\n", idx,
                               nbr_sysid, (unsigned)pseudo, (unsigned)metric, (unsigned)sub_tlv_len);
        if (sub_tlv_len > 0u)
        {
            lsdb_parse_sub_tlvs(buf, &val[pos + 11u], sub_tlv_len);
        }
        pos += entry_len;
    }

    if (idx == 0u)
    {
        g_string_append(buf, "    (no IS reach entries)\r\n");
    }
    if (pos < val_len)
    {
        g_string_append_printf(buf, "    IS            : trailing-bytes=%zu\r\n", val_len - pos);
    }
}

static void lsdb_parse_prefix_tlv(GString *buf, const uint8_t *val, size_t val_len, uint16_t afi,
                                  isis_lsdb_parse_stats_t *stats)
{
    if (!buf || !val || val_len == 0u || !stats)
    {
        return;
    }

    const char *label = (afi == ROUTE_AFI_IPV4) ? "IPv4" : "IPv6";
    size_t pos = 0u;
    uint32_t idx = 0u;
    while (pos + 6u <= val_len)
    {
        uint32_t metric = ((uint32_t)val[pos] << 24) | ((uint32_t)val[pos + 1u] << 16) |
                          ((uint32_t)val[pos + 2u] << 8) | (uint32_t)val[pos + 3u];
        uint8_t ctrl = val[pos + 4u];
        uint8_t prefix_len = val[pos + 5u];
        uint8_t max_prefix = (afi == ROUTE_AFI_IPV4) ? 32u : 128u;
        uint8_t pfx_bytes = (uint8_t)((prefix_len + 7u) / 8u);
        pos += 6u;

        if (prefix_len > max_prefix || (afi == ROUTE_AFI_IPV4 && pfx_bytes > 4u) ||
            (afi == ROUTE_AFI_IPV6 && pfx_bytes > 16u) || pos + pfx_bytes > val_len)
        {
            g_string_append_printf(buf, "    %s          : parse-error (prefix-len=%u pfx-bytes=%u)\r\n", label,
                                   (unsigned)prefix_len, (unsigned)pfx_bytes);
            break;
        }

        char prefix[96] = {0};
        int has_prefix = (lsdb_format_prefix(afi, &val[pos], prefix_len, prefix, sizeof(prefix)) == 0);

        idx++;
        if (afi == ROUTE_AFI_IPV4)
        {
            stats->v4_count++;
        }
        else
        {
            stats->v6_count++;
        }

        if (has_prefix)
        {
            g_string_append_printf(buf, "    %s[%u]       : prefix=%s metric=%u ctrl=0x%02x\r\n", label, idx, prefix,
                                   (unsigned)metric, (unsigned)ctrl);
        }
        else
        {
            g_string_append_printf(buf,
                                   "    %s[%u]       : prefix=<decode-failed/%u> metric=%u ctrl=0x%02x raw=", label,
                                   idx, (unsigned)prefix_len, (unsigned)metric, (unsigned)ctrl);
            lsdb_append_hex_bytes(buf, &val[pos], pfx_bytes, 16u);
            g_string_append(buf, "\r\n");
        }

        pos += pfx_bytes;
    }

    if (idx == 0u)
    {
        g_string_append_printf(buf, "    (no %s reach entries)\r\n", label);
    }
    if (pos < val_len)
    {
        g_string_append_printf(buf, "    %s          : trailing-bytes=%zu\r\n", label, val_len - pos);
    }
}

static void lsdb_parse_all_tlvs(GString *buf, const isis_lsdb_entry_t *entry, isis_lsdb_parse_stats_t *stats)
{
    if (!buf || !entry || !stats)
    {
        return;
    }
    if (!entry->tlvs || entry->tlvs->len == 0u)
    {
        g_string_append(buf, "  TLV             : (none)\r\n");
        return;
    }

    const uint8_t *tlvs = entry->tlvs->data;
    size_t tlv_len = entry->tlvs->len;
    size_t pos = 0u;
    uint32_t tlv_idx = 0u;
    while (pos + 2u <= tlv_len)
    {
        uint8_t tlv_type = tlvs[pos];
        uint8_t len = tlvs[pos + 1u];
        pos += 2u;
        if (pos + len > tlv_len)
        {
            g_string_append_printf(buf, "  TLV             : parse-error (type=%u len=%u truncated)\r\n",
                                   (unsigned)tlv_type, (unsigned)len);
            break;
        }

        tlv_idx++;
        stats->tlv_count++;
        g_string_append_printf(buf, "  TLV[%u]         : type=%u (%s) len=%u\r\n", tlv_idx, (unsigned)tlv_type,
                               lsdb_tlv_name(tlv_type), (unsigned)len);

        if (tlv_type == ISIS_TLV_EXT_IS_REACH)
        {
            lsdb_parse_ext_is_tlv(buf, &tlvs[pos], len, stats);
        }
        else if (tlv_type == ISIS_TLV_EXT_IP_REACH)
        {
            lsdb_parse_prefix_tlv(buf, &tlvs[pos], len, ROUTE_AFI_IPV4, stats);
        }
        else if (tlv_type == ISIS_TLV_IPV6_REACH)
        {
            lsdb_parse_prefix_tlv(buf, &tlvs[pos], len, ROUTE_AFI_IPV6, stats);
        }
        else
        {
            g_string_append(buf, "    Raw           : ");
            lsdb_append_hex_bytes(buf, &tlvs[pos], len, 32u);
            g_string_append(buf, "\r\n");
        }
        pos += len;
    }

    if (pos < tlv_len)
    {
        g_string_append_printf(buf, "  TLV             : trailing-bytes=%zu\r\n", tlv_len - pos);
    }
}

static void parse_show_lsdb_filter(cli_tlv_parser_t *parser, show_lsdb_filter_t *filter)
{
    if (!filter)
    {
        return;
    }
    memset(filter, 0, sizeof(*filter));

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry) && entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
        {
            uint32_t tag = cli_tlv_entry_get_ctx_uint32(&entry);
            if (tag != 0u)
            {
                filter->tag_filter = tag;
                filter->has_filter = 1;
            }
        }
        else if (!CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == 1)
            {
                filter->afi_filter = ROUTE_AFI_IPV4;
            }
            else if (entry.cfg_id == 2)
            {
                filter->afi_filter = ROUTE_AFI_IPV6;
            }
            else if (entry.cfg_id == 3)
            {
                int64_t v = cli_tlv_entry_get_int(&entry);
                if (v > 0 && v <= 0xFFFFFFFFll)
                {
                    filter->tag_filter = (uint32_t)v;
                    filter->has_filter = 1;
                }
            }
        }
        cli_tlv_entry_free(&entry);
    }
}

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

    inst_ctx->ctx->count++;

    const char *rx_if = (entry->rx_ifname[0] != '\0') ? entry->rx_ifname : "-";
    uint32_t tlv_bytes = (entry->tlvs) ? (uint32_t)entry->tlvs->len : 0u;
    g_string_append_printf(inst_ctx->ctx->buf,
                           "LSP Entry %u\r\n"
                           "  Tag             : %u\r\n"
                           "  Rx-If           : %s\r\n"
                           "  Level           : L%u\r\n"
                           "  System-ID       : %s\r\n"
                           "  Seq             : 0x%08x\r\n"
                           "  Lifetime(sec)   : %u\r\n"
                           "  Checksum        : 0x%04x\r\n"
                           "  Prefix Count    : ipv4=%u ipv6=%u\r\n"
                           "  LastRx(sec)     : %llu\r\n"
                           "  TLV Bytes       : %u\r\n",
                           inst_ctx->ctx->count, inst_ctx->inst->tag, rx_if, (unsigned)entry->level, sysid, entry->seq,
                           (unsigned)entry->lifetime_sec, (unsigned)entry->checksum, (unsigned)entry->ipv4_prefix_count,
                           (unsigned)entry->ipv6_prefix_count, (unsigned long long)age_sec, (unsigned)tlv_bytes);

    isis_lsdb_parse_stats_t parse_stats = {0};
    lsdb_parse_all_tlvs(inst_ctx->ctx->buf, entry, &parse_stats);
    g_string_append_printf(inst_ctx->ctx->buf,
                           "  Parsed Count    : links=%u ipv4-prefix=%u ipv6-prefix=%u tlv=%u\r\n\r\n",
                           parse_stats.is_count, parse_stats.v4_count, parse_stats.v6_count, parse_stats.tlv_count);
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
    show_lsdb_filter_t filter;
    parse_show_lsdb_filter(parser, &filter);

    GString *buf = g_string_new("");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    g_string_append(buf, "\r\nISIS LSDB Detail\r\n");

    show_lsdb_ctx_t ctx = {
        .buf = buf,
        .tag_filter = filter.tag_filter,
        .has_filter = filter.has_filter,
        .afi_filter = filter.afi_filter,
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

typedef struct show_route_query
{
    uint32_t tag;
    int has_tag;
    int has_dest;
    uint16_t afi;
    net_addr_t dest_addr;
    uint8_t prefix_len;
} show_route_query_t;

typedef struct show_route_ctx
{
    GString *buf;
    const show_route_query_t *query;
    uint32_t count;
    int detail;
} show_route_ctx_t;

typedef struct show_route_table_ctx
{
    show_route_ctx_t *ctx;
    const char *table_name;
} show_route_table_ctx_t;

static int show_send_simple_resp(dev_ipc_message_t *msg, const char *text)
{
    GString *buf = g_string_new(text ? text : "");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }
    return cli_chunk_stream_start(&g_isis_show_stream, isis_local_ipc_ctx(), DEV_MODULE_ID_ISIS, msg, buf);
}

static int parse_show_route_query(cli_tlv_parser_t *parser, show_route_query_t *out)
{
    if (!parser || !out)
    {
        return ERRCODE_FAIL;
    }

    memset(out, 0, sizeof(*out));
    int has_afi = 0;
    int parse_err = 0;
    int has_v4_dest = 0;
    int has_v4_mask = 0;
    int has_v6_dest = 0;
    int has_v6_mask = 0;
    net_addr_t v4_dest;
    net_addr_t v6_dest;
    memset(&v4_dest, 0, sizeof(v4_dest));
    memset(&v6_dest, 0, sizeof(v6_dest));
    uint8_t v4_mask = 0u;
    uint8_t v6_mask = 0u;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_ISIS_TAG)
            {
                uint32_t tag = cli_tlv_entry_get_ctx_uint32(&entry);
                if (tag != 0u)
                {
                    out->tag = tag;
                    out->has_tag = 1;
                }
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1u:
            {
                if (has_afi && out->afi != ROUTE_AFI_IPV4)
                {
                    parse_err = 1;
                }
                else
                {
                    out->afi = ROUTE_AFI_IPV4;
                    has_afi = 1;
                }
                break;
            }
            case 2u:
            {
                if (has_afi && out->afi != ROUTE_AFI_IPV6)
                {
                    parse_err = 1;
                }
                else
                {
                    out->afi = ROUTE_AFI_IPV6;
                    has_afi = 1;
                }
                break;
            }
            case 3u:
            {
                int64_t v = cli_tlv_entry_get_int(&entry);
                if (v > 0 && v <= 0xFFFFFFFFll)
                {
                    out->tag = (uint32_t)v;
                    out->has_tag = 1;
                }
                else
                {
                    parse_err = 1;
                }
                break;
            }
            case 4u:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (!text || net_addr_from_str(text, &v4_dest) != 0 || v4_dest.family != AF_INET)
                {
                    parse_err = 1;
                }
                else
                {
                    has_v4_dest = 1;
                }
                break;
            }
            case 5u:
            {
                int64_t v = cli_tlv_entry_get_int(&entry);
                if (v < 0 || v > 32)
                {
                    parse_err = 1;
                }
                else
                {
                    v4_mask = (uint8_t)v;
                    has_v4_mask = 1;
                }
                break;
            }
            case 6u:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (!text || net_addr_from_str(text, &v6_dest) != 0 || v6_dest.family != AF_INET6)
                {
                    parse_err = 1;
                }
                else
                {
                    has_v6_dest = 1;
                }
                break;
            }
            case 7u:
            {
                int64_t v = cli_tlv_entry_get_int(&entry);
                if (v < 0 || v > 128)
                {
                    parse_err = 1;
                }
                else
                {
                    v6_mask = (uint8_t)v;
                    has_v6_mask = 1;
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (parse_err || !out->has_tag || !has_afi)
    {
        return ERRCODE_FAIL;
    }

    if (!has_v4_dest && !has_v4_mask && !has_v6_dest && !has_v6_mask)
    {
        return ERRCODE_SUCCESS;
    }

    if (out->afi == ROUTE_AFI_IPV4 && has_v4_dest && has_v4_mask && !has_v6_dest && !has_v6_mask)
    {
        out->has_dest = 1;
        out->dest_addr = v4_dest;
        out->prefix_len = v4_mask;
    }
    else if (out->afi == ROUTE_AFI_IPV6 && has_v6_dest && has_v6_mask && !has_v4_dest && !has_v4_mask)
    {
        out->has_dest = 1;
        out->dest_addr = v6_dest;
        out->prefix_len = v6_mask;
    }
    else
    {
        return ERRCODE_FAIL;
    }

    if (net_addr_prefix_normalize(&out->dest_addr, out->prefix_len) != 0)
    {
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

static const char *route_source_name(const char *table_name, const char *route_key)
{
    if (table_name && strcmp(table_name, "route_states") == 0)
    {
        return "local-if";
    }
    if (route_key && g_str_has_prefix(route_key, "host|"))
    {
        return "neighbor-host";
    }
    if (route_key && g_str_has_prefix(route_key, "lsp|"))
    {
        return "lsp-spf";
    }
    return "learned";
}

static int route_key_extract_level(const char *route_key, uint8_t *level_out)
{
    if (!route_key || !level_out)
    {
        return 0;
    }

    const char *start = NULL;
    if (g_str_has_prefix(route_key, "lsp|"))
    {
        start = route_key + 4;
    }
    else if (g_str_has_prefix(route_key, "host|"))
    {
        const char *p = route_key + 5;
        const char *sep = strchr(p, '|');
        if (!sep || sep[1] == '\0')
        {
            return 0;
        }
        start = sep + 1;
    }
    else
    {
        return 0;
    }

    char *end = NULL;
    unsigned long level = strtoul(start, &end, 10);
    if (!end || end == start || *end != '|')
    {
        return 0;
    }
    if (level != 1ul && level != 2ul)
    {
        return 0;
    }
    *level_out = (uint8_t)level;
    return 1;
}

static const char *route_level_name(const char *route_key)
{
    uint8_t level = 0u;
    if (!route_key_extract_level(route_key, &level))
    {
        return "-";
    }
    return (level == 1u) ? "L1" : "L2";
}

static int route_state_match_filter(const isis_route_state_t *state, const show_route_query_t *query)
{
    if (!state || !query)
    {
        return 0;
    }
    if (state->afi != query->afi)
    {
        return 0;
    }
    if (!query->has_dest)
    {
        return 1;
    }
    if (state->prefix_len != query->prefix_len)
    {
        return 0;
    }
    return net_addr_equal(&state->prefix_addr, &query->dest_addr) ? 1 : 0;
}

static void show_route_emit_item(const char *route_key, const isis_route_state_t *state,
                                 show_route_table_ctx_t *table_ctx)
{
    if (!route_key || !state || !table_ctx || !table_ctx->ctx || !table_ctx->ctx->buf || !table_ctx->ctx->query)
    {
        return;
    }

    if (!route_state_match_filter(state, table_ctx->ctx->query))
    {
        return;
    }

    char prefix_addr[64] = "-";
    char prefix[80] = "-";
    format_addr_or_dash(&state->prefix_addr, prefix_addr, sizeof(prefix_addr));
    g_snprintf(prefix, sizeof(prefix), "%s/%u", prefix_addr, (unsigned)state->prefix_len);

    char nexthop[64] = "-";
    if ((state->nexthop_addr.family == AF_INET || state->nexthop_addr.family == AF_INET6) &&
        !net_addr_is_zero(&state->nexthop_addr))
    {
        net_addr_to_str(&state->nexthop_addr, nexthop, sizeof(nexthop));
    }

    char src_addr[64] = "-";
    format_addr_or_dash(&state->source_addr, src_addr, sizeof(src_addr));

    char oif[64] = "-";
    format_oif(state->out_ifindex, oif, sizeof(oif));

    table_ctx->ctx->count++;
    if (table_ctx->ctx->detail)
    {
        g_string_append_printf(table_ctx->ctx->buf,
                               "Route %u\r\n"
                               "  Source       : %s\r\n"
                               "  Table        : %s\r\n"
                               "  Key          : %s\r\n"
                               "  Level        : %s\r\n"
                               "  AF           : %s\r\n"
                               "  Prefix       : %s\r\n"
                               "  Nexthop      : %s\r\n"
                               "  Source-Addr  : %s\r\n"
                               "  Out-If       : %s\r\n"
                               "  Metric       : %u\r\n\r\n",
                               table_ctx->ctx->count, route_source_name(table_ctx->table_name, route_key),
                               table_ctx->table_name ? table_ctx->table_name : "-", route_key,
                               route_level_name(route_key), afi_name(state->afi), prefix, nexthop, src_addr, oif,
                               (unsigned)state->metric);
    }
    else
    {
        g_string_append_printf(table_ctx->ctx->buf, "%-13s %-5s %-6s %-40s %-40s %-16s %-6u\r\n",
                               route_source_name(table_ctx->table_name, route_key), afi_name(state->afi),
                               route_level_name(route_key), prefix, nexthop, oif, (unsigned)state->metric);
    }
}

static void show_route_item_cb(gpointer key, gpointer value, gpointer user_data)
{
    const char *route_key = (const char *)key;
    const isis_route_state_t *state = (const isis_route_state_t *)value;
    show_route_table_ctx_t *table_ctx = (show_route_table_ctx_t *)user_data;
    if (!route_key || !state || !table_ctx)
    {
        return;
    }
    show_route_emit_item(route_key, state, table_ctx);
}

static void show_route_head_item_cb(gpointer key, gpointer value, gpointer user_data)
{
    const char *route_key = (const char *)key;
    const isis_route_head_t *head = (const isis_route_head_t *)value;
    show_route_table_ctx_t *table_ctx = (show_route_table_ctx_t *)user_data;
    if (!route_key || !head || !table_ctx)
    {
        return;
    }

    for (GList *cur = head->path_list; cur; cur = cur->next)
    {
        const isis_route_path_t *path = (const isis_route_path_t *)cur->data;
        if (!path)
        {
            continue;
        }
        show_route_emit_item(route_key, &path->state, table_ctx);
    }
}

static int handle_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    show_route_query_t query;
    if (parse_show_route_query(parser, &query) != ERRCODE_SUCCESS)
    {
        return show_send_simple_resp(
            msg, "Error: invalid route query, expected ipv4|ipv6 route <tag> [destination mask]\r\n");
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    char filter_str[64] = {0};
    if (query.has_dest)
    {
        net_addr_to_str(&query.dest_addr, filter_str, sizeof(filter_str));
        g_string_append_printf(buf, "\r\nISIS %s Routes Detail (tag %u, %s/%u)\r\n", afi_name(query.afi), query.tag,
                               filter_str, (unsigned)query.prefix_len);
    }
    else
    {
        g_string_append_printf(buf, "\r\nISIS %s Routes (tag %u)\r\n", afi_name(query.afi), query.tag);
        g_string_append(buf, "Source        AF    Level  Prefix                                   Nexthop              "
                             "                    "
                             "OIF              Metric\r\n"
                             "------------- ----- ------ ---------------------------------------- "
                             "---------------------------------------- "
                             "---------------- ------\r\n");
    }

    const isis_instance_cfg_t *inst = NULL;
    if (g_isis_work_local && g_isis_work_local->instances)
    {
        inst =
            (const isis_instance_cfg_t *)g_hash_table_lookup(g_isis_work_local->instances, GUINT_TO_POINTER(query.tag));
    }
    if (!inst)
    {
        g_string_append(buf, "(instance not found)\r\n");
        return cli_chunk_stream_start(&g_isis_show_stream, isis_local_ipc_ctx(), DEV_MODULE_ID_ISIS, msg, buf);
    }

    show_route_ctx_t ctx = {
        .buf = buf,
        .query = &query,
        .count = 0u,
        .detail = query.has_dest,
    };

    show_route_table_ctx_t local_ctx = {
        .ctx = &ctx,
        .table_name = "route_states",
    };
    if (inst->route_states)
    {
        g_hash_table_foreach(inst->route_states, show_route_item_cb, &local_ctx);
    }

    show_route_table_ctx_t learned_ctx = {
        .ctx = &ctx,
        .table_name = "learned_route_heads",
    };
    if (inst->learned_route_heads)
    {
        g_hash_table_foreach(inst->learned_route_heads, show_route_head_item_cb, &learned_ctx);
    }

    if (ctx.count == 0u)
    {
        g_string_append(buf, query.has_dest ? "(no matching routes)\r\n" : "(no routes)\r\n");
    }
    else
    {
        g_string_append_printf(buf, "\r\nTotal %u route(s)\r\n", ctx.count);
    }

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
        case ISIS_CLI_GROUP_ID_SHOW_ROUTE:
            rc = handle_show_route(msg, &parser);
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
