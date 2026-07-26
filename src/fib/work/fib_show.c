#include "fib_show.h"

#include <glib.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "fib_main.h"
#include "fib_os.h"
#include "fib_worker.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "tunnel.h"
#include "vrf.h"

static cli_chunk_stream_t g_fib_show_stream;

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_FIB, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(fib_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static int send_chunked(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_fib_show_stream, fib_local_ipc_ctx(), DEV_MODULE_ID_FIB, msg, full_text);
}

typedef struct fib_show_route_ctx
{
    GString *buf;
    uint16_t afi;
    uint32_t vrf_id;
    const char *vrf_name;
    gboolean has_filter;
    net_addr_t filter_addr;
    uint8_t filter_prefix_len;
    uint32_t count;
} fib_show_route_ctx_t;

typedef struct fib_show_ilm_ctx
{
    GString *buf;
    uint32_t vrf_id;
    gboolean has_filter;
    uint32_t filter_label;
    uint32_t count;
} fib_show_ilm_ctx_t;

typedef struct fib_show_vrf_filter
{
    char name[VRF_NAME_MAX_LEN];
    uint32_t vrf_id;
    uint32_t l3vrf_table_id;
} fib_show_vrf_filter_t;

static void fib_show_vrf_filter_default(fib_show_vrf_filter_t *filter)
{
    if (!filter)
    {
        return;
    }
    memset(filter, 0, sizeof(*filter));
    g_strlcpy(filter->name, VRF_PUBLIC_VRF_NAME, sizeof(filter->name));
    filter->vrf_id = VRF_PUBLIC_VRF_ID;
    filter->l3vrf_table_id = RT_TABLE_MAIN;
}

static int fib_show_vrf_filter_resolve(const char *vrf_name, fib_show_vrf_filter_t *filter)
{
    fib_show_vrf_filter_default(filter);
    if (!filter || !vrf_name || vrf_name[0] == '\0' || strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    const vrf_api_cache_entry_t *vrf = vrf_api_cache_lookup_by_name(vrf_name);
    if (!vrf)
    {
        return ERRCODE_FAIL;
    }

    g_strlcpy(filter->name, vrf->name, sizeof(filter->name));
    filter->vrf_id = vrf->vrf_id;
    filter->l3vrf_table_id = vrf->l3vrf_table_id;
    return ERRCODE_SUCCESS;
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

static const char *proto_name_long(uint32_t protocol)
{
    switch (protocol)
    {
        case ROUTE_PROTOCOL_CONNECTED:
            return "connected";
        case ROUTE_PROTOCOL_STATIC:
            return "static";
        case ROUTE_PROTOCOL_BGP:
            return "bgp";
        case ROUTE_PROTOCOL_OSPF:
            return "ospf";
        case ROUTE_PROTOCOL_OSPFV3:
            return "ospfv3";
        case ROUTE_PROTOCOL_ISIS:
            return "isis";
        default:
            return "unknown";
    }
}

static const char *nh_type_name(uint8_t nh_type)
{
    switch (nh_type)
    {
        case FIB_NH_TYPE_IP:
            return "ip";
        case FIB_NH_TYPE_TUNNEL:
            return "tunnel";
        case FIB_NH_TYPE_BLACKHOLE:
            return "blackhole";
        default:
            return "unknown";
    }
}

static const char *action_name(uint8_t action)
{
    switch (action)
    {
        case TUNNEL_ACTION_DROP:
            return "drop";
        case TUNNEL_ACTION_PUSH:
            return "push";
        case TUNNEL_ACTION_SWAP:
            return "swap";
        case TUNNEL_ACTION_POP:
            return "pop";
        case TUNNEL_ACTION_PHP:
            return "php";
        default:
            return "unknown";
    }
}

static void prefix_to_str(const fib_route_entry_t *route, char *buf, size_t sz)
{
    char addr[64] = "-";
    if (!route || !buf || sz == 0)
    {
        return;
    }
    net_addr_to_str(&route->prefix_addr, addr, sizeof(addr));
    snprintf(buf, sz, "%s/%u", addr, route->prefix_len);
}

static void nexthop_to_str(const fib_route_entry_t *route, char *buf, size_t sz)
{
    if (!route || !buf || sz == 0)
    {
        return;
    }

    if (route->nh_type == FIB_NH_TYPE_TUNNEL)
    {
        snprintf(buf, sz, "tunnel:%u", route->tunnel_id);
        return;
    }
    if (route->nh_type == FIB_NH_TYPE_BLACKHOLE)
    {
        snprintf(buf, sz, "blackhole");
        return;
    }
    if (route->nexthop_addr.family == AF_INET || route->nexthop_addr.family == AF_INET6)
    {
        net_addr_to_str(&route->nexthop_addr, buf, sz);
        if (!net_addr_is_zero(&route->nexthop_addr))
        {
            return;
        }
    }
    snprintf(buf, sz, "-");
}

static void oif_to_str(uint32_t out_ifindex, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    if (out_ifindex == ROUTE_INLOOP_IFINDEX)
    {
        g_strlcpy(buf, ROUTE_INLOOP_IFNAME, sz);
        return;
    }
    snprintf(buf, sz, "%u", out_ifindex);
}

static void labels_to_str(uint8_t count, const uint32_t *labels, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    buf[0] = '\0';
    if (!labels || count == 0)
    {
        snprintf(buf, sz, "-");
        return;
    }

    gsize used = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        int n = snprintf(buf + used, sz - used, "%s%u", (i == 0) ? "" : ",", labels[i]);
        if (n < 0 || (size_t)n >= sz - used)
        {
            buf[sz - 1] = '\0';
            return;
        }
        used += (gsize)n;
    }
}

static void append_route_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    fib_route_state_t *state = (fib_route_state_t *)value;
    fib_show_route_ctx_t *ctx = (fib_show_route_ctx_t *)user_data;
    if (!state || !ctx || !ctx->buf || state->entry.afi != ctx->afi || state->entry.vrf_id != ctx->vrf_id)
    {
        return;
    }
    if (ctx->has_filter && (state->entry.prefix_len != ctx->filter_prefix_len ||
                            !net_addr_equal(&state->entry.prefix_addr, &ctx->filter_addr)))
    {
        return;
    }

    char prefix[96] = "-";
    char nexthop[64] = "-";
    char oif[32] = "-";
    prefix_to_str(&state->entry, prefix, sizeof(prefix));
    nexthop_to_str(&state->entry, nexthop, sizeof(nexthop));
    oif_to_str(state->entry.out_ifindex, oif, sizeof(oif));

    g_string_append_printf(ctx->buf, "%-7s %-26s %-20s %-10s %-8s %-8d %-8d %-9s %-7s\r\n", afi_name(state->entry.afi),
                           prefix, nexthop, nh_type_name(state->entry.nh_type), oif, state->entry.metric,
                           state->entry.preference, state->installed ? "yes" : "no",
                           (state->entry.flags & FIB_ROUTE_FLAG_SKIP_OS) ? "yes" : "no");
    ctx->count++;
}

static void append_route_detail_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    fib_route_state_t *state = (fib_route_state_t *)value;
    fib_show_route_ctx_t *ctx = (fib_show_route_ctx_t *)user_data;
    if (!state || !ctx || !ctx->buf || state->entry.afi != ctx->afi || state->entry.vrf_id != ctx->vrf_id)
    {
        return;
    }
    if (ctx->has_filter && (state->entry.prefix_len != ctx->filter_prefix_len ||
                            !net_addr_equal(&state->entry.prefix_addr, &ctx->filter_addr)))
    {
        return;
    }

    char prefix[96] = "-";
    char nexthop[64] = "-";
    char oif[32] = "-";
    char iter_oif_str[32] = "-";
    prefix_to_str(&state->entry, prefix, sizeof(prefix));
    nexthop_to_str(&state->entry, nexthop, sizeof(nexthop));
    oif_to_str(state->entry.out_ifindex, oif, sizeof(oif));

    /* 取关联 nexthop 对象（按 nexthop_id），展示解析后的网关/出接口（Iter NH / Iter OIF） */
    char iter_nh[64] = "-";
    uint32_t iter_oif = state->entry.out_ifindex;
    fib_nexthop_state_t *nh =
        fib_rib_nexthop_lookup(g_fib_work_local ? g_fib_work_local->rib : NULL, state->entry.nexthop_id);
    if (nh)
    {
        if (nh->entry.gateway_addr.family == AF_INET || nh->entry.gateway_addr.family == AF_INET6)
        {
            net_addr_to_str(&nh->entry.gateway_addr, iter_nh, sizeof(iter_nh));
        }
        iter_oif = nh->entry.out_ifindex;
    }
    oif_to_str(iter_oif, iter_oif_str, sizeof(iter_oif_str));

    ctx->count++;
    /* 与 `show route <prefix> <len>` 详情格式对齐，并新增 NH-ID 显示 */
    g_string_append_printf(ctx->buf,
                           "\r\nRouting entry for %s (VRF: %s)\r\n"
                           "  Path [%u]: %s\r\n"
                           "    Nexthop   : %s\r\n"
                           "    NH-ID     : %u\r\n"
                           "    Interface : %s\r\n"
                           "    Iter NH   : %s\r\n"
                           "    Iter OIF  : %s\r\n"
                           "    NH-Type   : %s\r\n"
                           "    Tunnel-ID : %u\r\n"
                           "    Metric    : %d\r\n"
                           "    Preference: %d\r\n"
                           "    Installed : %s\r\n"
                           "    Skip OS   : %s\r\n",
                           prefix, ctx->vrf_name ? ctx->vrf_name : "-", ctx->count,
                           proto_name_long(state->entry.protocol), nexthop, state->entry.nexthop_id, oif, iter_nh,
                           iter_oif_str, nh_type_name(state->entry.nh_type), state->entry.tunnel_id,
                           state->entry.metric, state->entry.preference, state->installed ? "yes" : "no",
                           (state->entry.flags & FIB_ROUTE_FLAG_SKIP_OS) ? "yes" : "no");

    if (state->entry.out_label != 0u)
    {
        /* 路由自带的出标签（L3VPN 私网/VPN 标签）：转发时压在隧道传输标签内层 */
        g_string_append_printf(ctx->buf, "    Out-Label : %u\r\n", state->entry.out_label);
    }

    if (state->entry.nh_type == FIB_NH_TYPE_TUNNEL && state->entry.tunnel_id != 0u)
    {
        fib_tunnel_state_t *tun =
            fib_rib_tunnel_lookup(g_fib_work_local ? g_fib_work_local->rib : NULL, state->entry.tunnel_id);
        if (tun)
        {
            char relay[64] = "-";
            char labels[128] = "-";
            if (tun->entry.relay_addr.family == AF_INET || tun->entry.relay_addr.family == AF_INET6)
            {
                net_addr_to_str(&tun->entry.relay_addr, relay, sizeof(relay));
            }
            labels_to_str(tun->entry.label_count, tun->entry.labels, labels, sizeof(labels));
            g_string_append_printf(ctx->buf, "  Tunnel    : state=%s relay=%s oif=%u labels=[%s]\r\n",
                                   tun->entry.state ? "up" : "down", relay, tun->entry.out_ifindex, labels);
        }
        else
        {
            g_string_append(ctx->buf, "  Tunnel    : pending\r\n");
        }
    }
}

static void append_ilm_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    fib_ilm_state_t *state = (fib_ilm_state_t *)value;
    fib_show_ilm_ctx_t *ctx = (fib_show_ilm_ctx_t *)user_data;
    if (!state || !ctx || !ctx->buf || state->entry.vrf_id != ctx->vrf_id)
    {
        return;
    }
    if (ctx->has_filter && state->entry.in_label != ctx->filter_label)
    {
        return;
    }

    char relay[64] = "-";
    char labels[128] = "-";
    if (state->entry.relay_addr.family == AF_INET || state->entry.relay_addr.family == AF_INET6)
    {
        net_addr_to_str(&state->entry.relay_addr, relay, sizeof(relay));
    }
    labels_to_str(state->entry.label_count, state->entry.labels, labels, sizeof(labels));
    g_string_append_printf(ctx->buf, "%-8u %-8u %-10s %-8u %-14s %-10u %-20s %-14s %-9s %-7s\r\n", state->entry.vrf_id,
                           state->entry.in_label, action_name(state->entry.action), state->entry.nhlfe_id,
                           state->entry.state ? "up" : "down", state->entry.out_ifindex, relay, labels,
                           state->installed ? "yes" : "no", state->entry.state ? "yes" : "no");
    ctx->count++;
}

static void append_ilm_detail_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    fib_ilm_state_t *state = (fib_ilm_state_t *)value;
    fib_show_ilm_ctx_t *ctx = (fib_show_ilm_ctx_t *)user_data;
    if (!state || !ctx || !ctx->buf || state->entry.vrf_id != ctx->vrf_id)
    {
        return;
    }
    if (ctx->has_filter && state->entry.in_label != ctx->filter_label)
    {
        return;
    }

    char relay[64] = "-";
    char labels[128] = "-";
    if (state->entry.relay_addr.family == AF_INET || state->entry.relay_addr.family == AF_INET6)
    {
        net_addr_to_str(&state->entry.relay_addr, relay, sizeof(relay));
    }
    labels_to_str(state->entry.label_count, state->entry.labels, labels, sizeof(labels));

    ctx->count++;
    g_string_append_printf(ctx->buf,
                           "\r\nFIB MPLS ILM Detail: %u\r\n"
                           "  VRF       : %u\r\n"
                           "  Action    : %s(%u)\r\n"
                           "  State     : %s\r\n"
                           "  Installed : %s\r\n"
                           "  NHLFE     : %u\r\n"
                           "  OIF       : %u\r\n"
                           "  Relay     : %s\r\n"
                           "  Labels    : [%s]\r\n",
                           state->entry.in_label, state->entry.vrf_id, action_name(state->entry.action),
                           state->entry.action, state->entry.state ? "up" : "down", state->installed ? "yes" : "no",
                           state->entry.nhlfe_id, state->entry.out_ifindex, relay, labels);
}

static int handle_show_routes(dev_ipc_message_t *msg, uint16_t afi, const net_addr_t *filter_addr,
                              int64_t filter_prefix_len, const fib_show_vrf_filter_t *vrf_filter)
{
    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    fib_show_route_ctx_t ctx = {
        .buf = buf,
        .afi = afi,
        .vrf_id = vrf_filter ? vrf_filter->vrf_id : VRF_PUBLIC_VRF_ID,
        .vrf_name = vrf_filter ? vrf_filter->name : VRF_PUBLIC_VRF_NAME,
        .has_filter = filter_addr != NULL,
        .filter_prefix_len = filter_addr ? (uint8_t)filter_prefix_len : 0u,
        .count = 0,
    };
    if (filter_addr)
    {
        ctx.filter_addr = *filter_addr;
    }

    if (filter_addr)
    {
        fib_rib_foreach_route(g_fib_work_local ? g_fib_work_local->rib : NULL, append_route_detail_cb, &ctx);
    }
    else
    {
        g_string_append_printf(buf,
                               "\r\nFIB Routes (%s)\r\n"
                               "VRF: %s\r\n"
                               "%-7s %-26s %-20s %-10s %-8s %-8s %-8s %-9s %-7s\r\n"
                               "------- -------------------------- -------------------- ---------- "
                               "-------- -------- -------- --------- -------\r\n",
                               afi_name(afi), vrf_filter ? vrf_filter->name : VRF_PUBLIC_VRF_NAME, "AFI", "Prefix",
                               "Nexthop", "NH-Type", "OIF", "Metric", "Pref", "Installed", "SkipOS");

        fib_rib_foreach_route(g_fib_work_local ? g_fib_work_local->rib : NULL, append_route_cb, &ctx);
    }
    if (ctx.count == 0)
    {
        g_string_append(buf, "  (no routes)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u route(s)\r\n", ctx.count);
    return send_chunked(msg, buf);
}

static int handle_show_mpls(dev_ipc_message_t *msg, gboolean has_filter, uint32_t filter_label,
                            const fib_show_vrf_filter_t *vrf_filter)
{
    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    fib_show_ilm_ctx_t ctx = {
        .buf = buf,
        .vrf_id = vrf_filter ? vrf_filter->vrf_id : VRF_PUBLIC_VRF_ID,
        .has_filter = has_filter,
        .filter_label = filter_label,
        .count = 0,
    };

    if (has_filter)
    {
        fib_rib_foreach_ilm(g_fib_work_local ? g_fib_work_local->rib : NULL, append_ilm_detail_cb, &ctx);
    }
    else
    {
        g_string_append_printf(buf,
                               "\r\nFIB MPLS ILM (VRF: %s)\r\n"
                               "%-8s %-8s %-10s %-8s %-14s %-10s %-20s %-14s %-9s %-7s\r\n"
                               "-------- -------- ---------- -------- -------------- ---------- "
                               "-------------------- -------------- --------- -------\r\n",
                               vrf_filter ? vrf_filter->name : VRF_PUBLIC_VRF_NAME, "VRF", "Label", "Action", "NHLFE",
                               "State", "OIF", "Relay", "Labels", "Installed", "Active");

        fib_rib_foreach_ilm(g_fib_work_local ? g_fib_work_local->rib : NULL, append_ilm_cb, &ctx);
    }
    if (ctx.count == 0)
    {
        g_string_append(buf, "  (no MPLS ILM entries)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u ILM entr%s\r\n", ctx.count, ctx.count == 1 ? "y" : "ies");
    return send_chunked(msg, buf);
}

static int handle_show_os(dev_ipc_message_t *msg, uint16_t afi, const fib_show_vrf_filter_t *vrf_filter)
{
    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    sa_family_t family = AF_INET;
    if (afi == ROUTE_AFI_IPV6)
    {
        family = AF_INET6;
    }
    else if (afi == 0)
    {
        family = AF_MPLS;
    }
    uint32_t table_filter = vrf_filter ? vrf_filter->l3vrf_table_id : RT_TABLE_MAIN;
    gboolean include_local_table = family != AF_MPLS;
    uint32_t local_master_ifindex = 0u;
    if (vrf_filter && vrf_filter->vrf_id != VRF_PUBLIC_VRF_ID)
    {
        local_master_ifindex = if_nametoindex(vrf_filter->name);
        include_local_table = local_master_ifindex != 0u;
    }
    g_string_append_printf(buf, "\r\nOS FIB (VRF: %s table=%u%s)\r\n",
                           vrf_filter ? vrf_filter->name : VRF_PUBLIC_VRF_NAME, table_filter,
                           include_local_table ? "+local" : "");
    if (fib_os_show(buf, family, table_filter, TRUE, include_local_table, local_master_ifindex) != ERRCODE_SUCCESS)
    {
        g_string_free(buf, TRUE);
        send_resp(msg, "Error: 读取内核路由表失败\r\n");
        return ERRCODE_FAIL;
    }

    return send_chunked(msg, buf);
}

typedef struct fib_show_nexthop_ctx
{
    GString *buf;
    int has_afi;
    uint16_t afi;
    int has_vrf;
    uint32_t vrf_id;
    int has_nhid;
    uint32_t nhid;
    uint32_t count;
} fib_show_nexthop_ctx_t;

static void append_nexthop_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const fib_nexthop_state_t *state = (const fib_nexthop_state_t *)value;
    fib_show_nexthop_ctx_t *ctx = (fib_show_nexthop_ctx_t *)user_data;
    if (!state || !ctx)
    {
        return;
    }
    const fib_nexthop_entry_t *e = &state->entry;
    if (ctx->has_afi && e->afi != ctx->afi)
    {
        return;
    }
    if (ctx->has_vrf && e->vrf_id != ctx->vrf_id)
    {
        return;
    }
    if (ctx->has_nhid && e->nexthop_id != ctx->nhid)
    {
        return;
    }

    char gw[64] = "-";
    char oif[32] = "-";
    if (e->gateway_addr.family == AF_INET || e->gateway_addr.family == AF_INET6)
    {
        net_addr_to_str(&e->gateway_addr, gw, sizeof(gw));
    }
    oif_to_str(e->out_ifindex, oif, sizeof(oif));
    g_string_append_printf(ctx->buf, "%-10u %-6u %-5s %-10s %-6s %-20s %-6s\r\n", e->nexthop_id, e->vrf_id,
                           afi_name(e->afi), nh_type_name(e->nh_type), e->state ? "up" : "down", gw, oif);
    ctx->count++;
}

static int handle_show_fib_nexthop(dev_ipc_message_t *msg, int has_afi, uint16_t afi, int has_vrf, uint32_t vrf_id,
                                   int has_nhid, uint32_t nhid)
{
    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    fib_show_nexthop_ctx_t ctx = {
        .buf = buf,
        .has_afi = has_afi,
        .afi = afi,
        .has_vrf = has_vrf,
        .vrf_id = vrf_id,
        .has_nhid = has_nhid,
        .nhid = nhid,
        .count = 0,
    };
    g_string_append_printf(buf,
                           "\r\nFIB Nexthop Objects\r\n"
                           "%-10s %-6s %-5s %-10s %-6s %-20s %-6s\r\n"
                           "---------- ------ ----- ---------- ------ -------------------- ------\r\n",
                           "NH-ID", "VRF", "AFI", "NH-Type", "State", "Gateway", "OIF");
    fib_rib_foreach_nexthop(g_fib_work_local ? g_fib_work_local->rib : NULL, append_nexthop_cb, &ctx);
    if (ctx.count == 0)
    {
        g_string_append(buf, "  (no nexthop objects)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u nexthop(s)\r\n", ctx.count);
    return send_chunked(msg, buf);
}

static int handle_show_fib(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int show_ipv4 = 0;
    int show_ipv6 = 0;
    int show_mpls = 0;
    int show_os = 0;
    int show_nexthop = 0;
    gboolean has_nhid = FALSE;
    uint32_t filter_nhid = 0;
    net_addr_t filter_addr;
    gboolean has_filter_addr = FALSE;
    int64_t filter_prefix_len = -1;
    gboolean has_filter_prefix_len = FALSE;
    gboolean has_filter_label = FALSE;
    uint32_t filter_label = 0;
    char vrf_name[VRF_NAME_MAX_LEN] = {0};
    cli_tlv_entry_t entry;

    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry))
        {
            switch (entry.cfg_id)
            {
                case 1:
                    show_ipv4 = 1;
                    break;
                case 2:
                    show_ipv6 = 1;
                    break;
                case 3:
                    show_os = 1;
                    break;
                case 7:
                    show_mpls = 1;
                    break;
                case 8:
                    filter_label = (uint32_t)cli_tlv_entry_get_int(&entry);
                    has_filter_label = TRUE;
                    break;
                case 11:
                    show_nexthop = 1;
                    break;
                case 13:
                    filter_nhid = (uint32_t)cli_tlv_entry_get_int(&entry);
                    has_nhid = TRUE;
                    break;
                case 10:
                {
                    const char *text = cli_tlv_entry_get_text(&entry);
                    if (text)
                    {
                        g_strlcpy(vrf_name, text, sizeof(vrf_name));
                    }
                    break;
                }
                case 4:
                {
                    const char *text = cli_tlv_entry_get_text(&entry);
                    if (text && net_addr_from_str(text, &filter_addr) == 0)
                    {
                        has_filter_addr = TRUE;
                    }
                    break;
                }
                case 5:
                case 6:
                    filter_prefix_len = cli_tlv_entry_get_int(&entry);
                    has_filter_prefix_len = TRUE;
                    break;
                default:
                    break;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (show_nexthop)
    {
        /* show fib nexthop [ipv4|ipv6] [vrf X] [id N]：afi/vrf/id 不指定即全部 */
        int has_afi = (show_ipv4 || show_ipv6);
        uint16_t nh_afi = show_ipv6 ? ROUTE_AFI_IPV6 : ROUTE_AFI_IPV4;
        int has_vrf = (vrf_name[0] != '\0');
        uint32_t vrf_id = VRF_PUBLIC_VRF_ID;
        if (has_vrf)
        {
            fib_show_vrf_filter_t vf;
            if (fib_show_vrf_filter_resolve(vrf_name, &vf) != ERRCODE_SUCCESS)
            {
                char resp[160];
                snprintf(resp, sizeof(resp), "Error: VRF %s not found\r\n", vrf_name);
                send_resp(msg, resp);
                return ERRCODE_FAIL;
            }
            vrf_id = vf.vrf_id;
        }
        return handle_show_fib_nexthop(msg, has_afi, nh_afi, has_vrf, vrf_id, has_nhid, filter_nhid);
    }

    uint16_t afi = show_ipv6 ? ROUTE_AFI_IPV6 : ROUTE_AFI_IPV4;
    if (!show_ipv4 && !show_ipv6)
    {
        afi = ROUTE_AFI_IPV4;
    }

    fib_show_vrf_filter_t vrf_filter;
    if (fib_show_vrf_filter_resolve(vrf_name, &vrf_filter) != ERRCODE_SUCCESS)
    {
        char resp[160];
        snprintf(resp, sizeof(resp), "Error: VRF %s not found\r\n", vrf_name);
        send_resp(msg, resp);
        return ERRCODE_FAIL;
    }

    if (show_mpls)
    {
        if (has_filter_addr || has_filter_prefix_len)
        {
            send_resp(msg, "Error: MPLS FIB query does not support IP prefix filter\r\n");
            return ERRCODE_FAIL;
        }
        if (show_os && has_filter_label)
        {
            send_resp(msg, "Error: MPLS OS FIB query does not support label filter\r\n");
            return ERRCODE_FAIL;
        }
        return show_os ? handle_show_os(msg, 0, &vrf_filter)
                       : handle_show_mpls(msg, has_filter_label, filter_label, &vrf_filter);
    }

    if (has_filter_addr != has_filter_prefix_len)
    {
        send_resp(msg, "Error: prefix query must include prefix-length\r\n");
        return ERRCODE_FAIL;
    }
    if (show_os && has_filter_addr)
    {
        send_resp(msg, "Error: OS FIB query does not support prefix filter\r\n");
        return ERRCODE_FAIL;
    }
    if (has_filter_addr)
    {
        if (afi == ROUTE_AFI_IPV4 && (filter_addr.family != AF_INET || filter_prefix_len < 0 || filter_prefix_len > 32))
        {
            send_resp(msg, "Error: invalid IPv4 prefix/prefix-length\r\n");
            return ERRCODE_FAIL;
        }
        if (afi == ROUTE_AFI_IPV6 &&
            (filter_addr.family != AF_INET6 || filter_prefix_len < 0 || filter_prefix_len > 128))
        {
            send_resp(msg, "Error: invalid IPv6 prefix/prefix-length\r\n");
            return ERRCODE_FAIL;
        }
        (void)net_addr_prefix_normalize(&filter_addr, (uint8_t)filter_prefix_len);
    }

    return show_os
               ? handle_show_os(msg, afi, &vrf_filter)
               : handle_show_routes(msg, afi, has_filter_addr ? &filter_addr : NULL, filter_prefix_len, &vrf_filter);
}

int fib_show_dispatch(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        return cli_chunk_stream_continue(&g_fib_show_stream, fib_local_ipc_ctx(), DEV_MODULE_ID_FIB, msg);
    }

    if (!msg->payload)
    {
        return ERRCODE_FAIL;
    }

    fib_show_cleanup();

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("FIB: show payload parse failed");
        return ERRCODE_FAIL;
    }

    int result = ERRCODE_FAIL;
    switch (parser.group_id)
    {
        case FIB_CLI_GROUP_ID_SHOW:
            result = handle_show_fib(msg, &parser);
            break;
        default:
            LOG_WARN("FIB: unknown show group_id=%u", parser.group_id);
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}

void fib_show_cleanup(void)
{
    cli_chunk_stream_reset(&g_fib_show_stream);
}
