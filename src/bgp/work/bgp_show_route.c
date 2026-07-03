/**
 * @file   bgp_show_route.c
 * @brief  BGP show route 命令处理（从 bgp_show.c 拆出，专用于路由显示）
 * @author jhb
 * @date   2026/06/01
 */
#include "bgp_show_route.h"

#include <arpa/inet.h>
#include <glib.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp.h"
#include "bgp_adj_rib_in.h"
#include "bgp_attr_intern.h"
#include "bgp_ext_community.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_nexthop.h"
#include "bgp_peer.h"
#include "bgp_pkt.h"
#include "bgp_rd.h"
#include "bgp_relay.h"
#include "bgp_rib.h"
#include "bgp_show.h"
#include "bgp_update_group.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "bit.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "net_addr.h"

/**
 * @brief 解析 RD 字符串（ASN:NN 或 IP:NN）到 8 字节，供 rd 过滤使用
 * @return 0 成功，-1 格式非法
 */
static int bgp_show_rd_from_str(const char *s, bgp_rd_t *out)
{
    if (!s || !out)
    {
        return -1;
    }
    const char *colon = strchr(s, ':');
    if (!colon || colon == s || colon[1] == '\0')
    {
        return -1;
    }
    char left[64];
    size_t left_len = (size_t)(colon - s);
    if (left_len >= sizeof(left))
    {
        return -1;
    }
    memcpy(left, s, left_len);
    left[left_len] = '\0';
    const char *right = colon + 1;
    memset(out->bytes, 0, sizeof(out->bytes));

    if (strchr(left, '.'))
    {
        struct in_addr ip;
        char *endp = NULL;
        unsigned long val = strtoul(right, &endp, 10);
        if (inet_pton(AF_INET, left, &ip) != 1 || !endp || *endp != '\0' || val > 0xFFFFu)
        {
            return -1;
        }
        out->bytes[0] = 0x00;
        out->bytes[1] = 0x01;
        memcpy(out->bytes + 2, &ip.s_addr, 4);
        out->bytes[6] = (uint8_t)(val >> 8);
        out->bytes[7] = (uint8_t)val;
        return 0;
    }

    char *endp = NULL;
    unsigned long asn = strtoul(left, &endp, 10);
    if (!endp || *endp != '\0' || asn > 0xFFFFu)
    {
        return -1;
    }
    endp = NULL;
    unsigned long val = strtoul(right, &endp, 10);
    if (!endp || *endp != '\0' || val > 0xFFFFFFFFul)
    {
        return -1;
    }
    out->bytes[0] = 0x00;
    out->bytes[1] = 0x00;
    out->bytes[2] = (uint8_t)(asn >> 8);
    out->bytes[3] = (uint8_t)asn;
    out->bytes[4] = (uint8_t)(val >> 24);
    out->bytes[5] = (uint8_t)(val >> 16);
    out->bytes[6] = (uint8_t)(val >> 8);
    out->bytes[7] = (uint8_t)val;
    return 0;
}

static void bgp_nexthop_to_str(const bgp_nexthop_t *nexthop, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }

    char global[64] = "-";
    if (nexthop && nexthop->global.family != 0)
    {
        net_addr_to_str(&nexthop->global, global, sizeof(global));
    }

    if (nexthop && nexthop->has_link_local && nexthop->link_local.family != 0)
    {
        char ll[64];
        net_addr_to_str(&nexthop->link_local, ll, sizeof(ll));
        g_strlcpy(buf, global, sz);
        g_strlcat(buf, " (ll:", sz);
        g_strlcat(buf, ll, sz);
        g_strlcat(buf, ")", sz);
        return;
    }

    snprintf(buf, sz, "%s", global);
}

/** 将 BGP route flags 格式化为可读字符串 */
static void bgp_route_flags_to_str(uint32_t flags, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }

    buf[0] = '\0';
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_BEST))
    {
        g_strlcat(buf, "BEST|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_IMPORT))
    {
        g_strlcat(buf, "IMPORT|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_LOCAL_CROSS))
    {
        g_strlcat(buf, "LOCAL_CROSS|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_REMOTE_CROSS))
    {
        g_strlcat(buf, "REMOTE_CROSS|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_VALID))
    {
        g_strlcat(buf, "VALID|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_FLUSHED))
    {
        g_strlcat(buf, "FLUSHED|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_STALE))
    {
        g_strlcat(buf, "STALE|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_NO_ADV))
    {
        g_strlcat(buf, "NO_ADV|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_IMPORT_RIB))
    {
        g_strlcat(buf, "IMPORT_RIB|", sz);
    }
    if (BIT_TEST(flags, BGP_ROUTE_FLAG_LOCAL_DELIVERY))
    {
        g_strlcat(buf, "LOCAL_DELIVERY|", sz);
    }

    size_t n = strlen(buf);
    if (n == 0)
    {
        g_strlcpy(buf, "NONE", sz);
        return;
    }
    if (buf[n - 1] == '|')
    {
        buf[n - 1] = '\0';
    }
}

/** 将出接口索引格式化为 ifname(ifindex) */
static void bgp_ifindex_to_str(uint32_t ifindex, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    if (ifindex == 0)
    {
        snprintf(buf, sz, "-");
        return;
    }

    char ifname[IF_NAMESIZE] = {0};
    if (if_indextoname(ifindex, ifname))
    {
        snprintf(buf, sz, "%s(%u)", ifname, ifindex);
        return;
    }

    snprintf(buf, sz, "if%u", ifindex);
}

/* 路由表固定列宽（Network 列按实际前缀长度动态扩展） */
#define BGP_RT_COL_NET_MIN 24
#define BGP_RT_MARKER_COL 3
#define BGP_RT_COL_NH 20
#define BGP_RT_COL_LP 8
#define BGP_RT_COL_MED 8
#define BGP_RT_COL_ORIG 12

typedef struct bgp_show_route_ctx
{
    GString *buf;
    guint net_col_width;
    uint32_t listed_heads;
    uint32_t listed_routes;
} bgp_show_route_ctx_t;

typedef struct bgp_show_route_width_ctx
{
    guint net_col_width;
} bgp_show_route_width_ctx_t;

typedef struct bgp_show_rib_in_ctx
{
    GString *buf;
    guint net_col_width;
    const bgp_nlri_entry_t *filter;
    gboolean filter_is_vpn_prefix;
    uint32_t listed_routes;
} bgp_show_rib_in_ctx_t;

typedef struct bgp_show_rib_out_ctx
{
    GString *buf;
    guint net_col_width;
    const bgp_nlri_entry_t *filter;
    gboolean filter_is_vpn_prefix;
    uint32_t listed_routes;
} bgp_show_rib_out_ctx_t;

/**
 * @brief 将单条路径的各字段格式化到 lp/med/as_path 缓冲区
 */
static void bgp_attr_fmt_fields(const bgp_attr_t *attr, char *lp, size_t lp_sz, char *med, size_t med_sz, char *as_path,
                                size_t as_sz)
{
    if (attr && attr->has_local_pref)
    {
        snprintf(lp, lp_sz, "%u", attr->local_pref);
    }
    else
    {
        snprintf(lp, lp_sz, "-");
    }
    if (attr && attr->has_med)
    {
        snprintf(med, med_sz, "%u", attr->med);
    }
    else
    {
        snprintf(med, med_sz, "-");
    }
    if (attr && attr->as_path[0] != '\0')
    {
        snprintf(as_path, as_sz, "%.*s", (int)(as_sz - 1), attr->as_path);
    }
    else
    {
        snprintf(as_path, as_sz, "-");
    }
}

static void bgp_route_fmt_fields(const bgp_route_node_t *route, char *lp, size_t lp_sz, char *med, size_t med_sz,
                                 char *as_path, size_t as_sz)
{
    bgp_attr_fmt_fields(route ? BGP_ROUTE_ATTR(route) : NULL, lp, lp_sz, med, med_sz, as_path, as_sz);
}

static const char *bgp_route_label_name(const bgp_route_node_t *route)
{
    if (!route || !route->has_label)
    {
        return NULL;
    }

    switch (route->label_source)
    {
        case BGP_ROUTE_LABEL_SOURCE_LOCAL:
            return "LocalLabel";
        case BGP_ROUTE_LABEL_SOURCE_RECEIVED:
            return "RecvLabel";
        default:
            return "Label";
    }
}

static void bgp_show_evpn_esi_to_str(const bgp_esi_t *esi, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    if (!esi)
    {
        snprintf(buf, sz, "-");
        return;
    }
    const uint8_t *b = esi->bytes;
    snprintf(buf, sz, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6],
             b[7], b[8], b[9]);
}

static gboolean bgp_show_route_width_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const bgp_rthead_t *head = (const bgp_rthead_t *)value;
    bgp_show_route_width_ctx_t *ctx = (bgp_show_route_width_ctx_t *)user_data;
    if (!head || !ctx)
    {
        return FALSE;
    }

    char prefix_str[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(&head->nlri, prefix_str, sizeof(prefix_str));

    guint required_width = (guint)strlen(prefix_str) + BGP_RT_MARKER_COL;
    if (required_width > ctx->net_col_width)
    {
        ctx->net_col_width = required_width;
    }
    return FALSE;
}

static void bgp_show_route_width_each_rib(bgp_instance_t *inst_unused, bgp_rd_entry_t *entry_unused, bgp_rib_t *rib,
                                          gpointer user_data)
{
    (void)inst_unused;
    (void)entry_unused;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    g_tree_foreach(rib->head_tree, bgp_show_route_width_cb, user_data);
}

static guint bgp_show_route_network_col_width(const bgp_instance_t *inst)
{
    bgp_show_route_width_ctx_t ctx = {.net_col_width = BGP_RT_COL_NET_MIN};

    if (!inst || !inst->rd_entries)
    {
        return ctx.net_col_width;
    }
    bgp_inst_foreach_rib((bgp_instance_t *)inst, bgp_show_route_width_each_rib, &ctx);
    return ctx.net_col_width;
}

static gboolean bgp_show_route_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    bgp_show_route_ctx_t *ctx = (bgp_show_route_ctx_t *)user_data;
    if (!head || !ctx)
    {
        return FALSE;
    }

    char prefix_str[BGP_NLRI_KEY_MAX];
    /* VPN AF：RD 已在段头显示，行内只打印纯前缀 ip/len；其它 AF 用通用格式化 */
    if (bgp_safi_is_vpn(head->nlri.safi) && head->nlri.type == BGP_NLRI_PREFIX)
    {
        char ipbuf[INET6_ADDRSTRLEN] = "?";
        inet_ntop(head->nlri.prefix.prefix.addr.family, &head->nlri.prefix.prefix.addr.u, ipbuf, sizeof(ipbuf));
        snprintf(prefix_str, sizeof(prefix_str), "%s/%u", ipbuf, head->nlri.prefix.prefix.prefix_len);
    }
    else
    {
        bgp_nlri_to_str(&head->nlri, prefix_str, sizeof(prefix_str));
    }
    gboolean first = TRUE;

    for (const GList *l = head->route_list; l; l = l->next)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)l->data;
        if (!route)
        {
            continue;
        }

        char nh[64], lp[16], med[16], as_path[64];
        bgp_nexthop_t bgp_nh;
        memset(&bgp_nh, 0, sizeof(bgp_nh));
        (void)bgp_nexthop_get_route_bgp(route, &bgp_nh);
        bgp_nexthop_to_str(&bgp_nh, nh, sizeof(nh));
        bgp_route_fmt_fields(route, lp, sizeof(lp), med, sizeof(med), as_path, sizeof(as_path));

        /* 路由标记：'>'=BEST，'v'=VALID */
        int net_field_width =
            (int)((ctx->net_col_width > BGP_RT_MARKER_COL) ? (ctx->net_col_width - BGP_RT_MARKER_COL) : 1);
        g_string_append_printf(ctx->buf, "%c%c %-*s %-*s %-*s %-*s %-*s %s\r\n",
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_BEST) ? '>' : ' ',
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID) ? 'v' : ' ', net_field_width,
                               first ? prefix_str : "", BGP_RT_COL_NH, nh, BGP_RT_COL_LP, lp, BGP_RT_COL_MED, med,
                               BGP_RT_COL_ORIG, bgp_origin_str(BGP_ROUTE_ATTR(route)->origin), as_path);

        if (first)
        {
            ctx->listed_heads++;
            first = FALSE;
        }
        ctx->listed_routes++;
    }

    return FALSE;
}

/**
 * @brief 显示单条前缀的所有路径详情（供 show bgp route af ... <ip> <masklen> 使用）
 */
static void bgp_show_route_append_nlri_detail(GString *buf, const bgp_nlri_entry_t *nlri)
{
    if (!buf || !nlri || nlri->type != BGP_NLRI_EVPN || nlri->evpn.route_type != 5)
    {
        return;
    }

    char rd_str[48];
    char esi_str[32];
    char prefix_str[80];
    char gw_str[64];
    bgp_rd_to_str(&nlri->evpn.rd, rd_str, sizeof(rd_str));
    bgp_show_evpn_esi_to_str(&nlri->evpn.esi, esi_str, sizeof(esi_str));
    net_prefix_to_str(&nlri->evpn.ip_prefix, prefix_str, sizeof(prefix_str));
    net_addr_to_str(&nlri->evpn.gw_ip, gw_str, sizeof(gw_str));

    g_string_append_printf(buf, "  EVPN Type     : 5 (IP Prefix)\r\n");
    g_string_append_printf(buf, "  EVPN RD       : %s\r\n", rd_str);
    g_string_append_printf(buf, "  EVPN ESI      : %s\r\n", esi_str);
    g_string_append_printf(buf, "  EVPN EthTag   : %u\r\n", nlri->evpn.eth_tag);
    g_string_append_printf(buf, "  EVPN Prefix   : %s\r\n", prefix_str);
    g_string_append_printf(buf, "  EVPN Gateway  : %s\r\n", gw_str);
    g_string_append_printf(buf, "  EVPN Label    : %u\r\n", nlri->evpn.label1);
}

static void bgp_show_route_detail(GString *buf, const bgp_rthead_t *head)
{
    uint32_t path_count = (uint32_t)g_list_length(head->route_list);
    g_string_append_printf(buf, "  Head QueueRefCnt: %u\r\n", head->queue_refcnt);
    bgp_show_route_append_nlri_detail(buf, &head->nlri);
    g_string_append_printf(buf, "  Paths          : %u\r\n\r\n", path_count);

    for (const GList *l = head->route_list; l; l = l->next)
    {
        const bgp_route_node_t *route = (const bgp_route_node_t *)l->data;
        if (!route)
        {
            continue;
        }

        char nh[64], lp[16], med[16], as_path[256], ts_added[32], ts_updated[32];
        char iter_nh[64], out_if[64], flags_str[128];
        bgp_nexthop_t bgp_nh;
        memset(&bgp_nh, 0, sizeof(bgp_nh));
        (void)bgp_nexthop_get_route_bgp(route, &bgp_nh);
        bgp_nexthop_to_str(&bgp_nh, nh, sizeof(nh));
        bgp_route_fmt_fields(route, lp, sizeof(lp), med, sizeof(med), as_path, sizeof(as_path));
        bgp_fmt_time_usec(route->added_at_usec, ts_added, sizeof(ts_added));
        bgp_fmt_time_usec(route->updated_at_usec, ts_updated, sizeof(ts_updated));
        bgp_route_flags_to_str(route->flags, flags_str, sizeof(flags_str));

        bgp_nexthop_value_t nh_value;
        memset(&nh_value, 0, sizeof(nh_value));
        (void)bgp_relay_get_route_iter_value(route, &nh_value);

        if (nh_value.iter_watched && nh_value.iter_relay_addr.family != 0)
        {
            net_addr_to_str(&nh_value.iter_relay_addr, iter_nh, sizeof(iter_nh));
        }
        else
        {
            snprintf(iter_nh, sizeof(iter_nh), "-");
        }
        bgp_ifindex_to_str((nh_value.iter_watched && nh_value.iter_resolved) ? nh_value.iter_out_ifindex : 0u, out_if,
                           sizeof(out_if));
        const char *iter_state_str =
            nh_value.iter_watched ? (nh_value.iter_resolved ? "Resolved" : "Unresolved") : "Unwatched";

        /* 路由标记：'>'=BEST，'v'=VALID */
        g_string_append_printf(buf, "%c%c ", BIT_TEST(route->flags, BGP_ROUTE_FLAG_BEST) ? '>' : ' ',
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID) ? 'v' : ' ');
        if (!bgp_route_is_synthetic(route))
        {
            char peer_str[64];
            net_addr_to_str(&route->source, peer_str, sizeof(peer_str));
            g_string_append_printf(buf, "From Peer  : %s\r\n", peer_str);
        }
        else
        {
            g_string_append_printf(buf, "Imported\r\n");
        }
        const char *label_name = bgp_route_label_name(route);
        if (label_name)
        {
            g_string_append_printf(buf, "    %-10s: %u\r\n", label_name, route->label);
        }
        g_string_append_printf(buf, "    Attr-ID  : %u (refcnt=%u)\r\n", route->attr ? route->attr->attr_id : 0,
                               route->attr ? route->attr->refcnt : 0);
        g_string_append_printf(buf, "    NextHop  : %s\r\n", nh);
        g_string_append_printf(buf, "    NH-ID    : %u\r\n", route->nexthop_id);
        g_string_append_printf(buf, "    LocPref  : %s\r\n", lp);
        g_string_append_printf(buf, "    MED      : %s\r\n", med);
        g_string_append_printf(buf, "    Origin   : %s\r\n", bgp_origin_str(BGP_ROUTE_ATTR(route)->origin));
        g_string_append_printf(buf, "    Valid    : %s\r\n",
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID) ? "Yes" : "No");
        g_string_append_printf(buf, "    IterState: %s\r\n", iter_state_str);
        g_string_append_printf(buf, "    Iter-NH  : %s\r\n", iter_nh);
        g_string_append_printf(buf, "    Out-If   : %s\r\n", out_if);
        g_string_append_printf(buf, "    Tunnel-ID: %u\r\n", nh_value.tunnel_id);
        g_string_append_printf(buf, "    Flags    : 0x%08X (%s)\r\n", route->flags, flags_str);
        /* 借用引用计数：> 0 表示有 import_rib mirror / bgp_relay watch 等模块持有借用指针 */
        g_string_append_printf(buf, "    BorrowRef: %u\r\n", route->borrow_refcnt);
        /* mirror 指向的源节点（仅 IMPORT_RIB 路径有效） */
        if (route->src_route)
        {
            g_string_append_printf(buf, "    SrcRoute : %p\r\n", (void *)route->src_route);
        }
        g_string_append_printf(buf, "    AS-Path  : %s\r\n", as_path);

        if (BGP_ROUTE_ATTR(route)->communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Community: %s\r\n", BGP_ROUTE_ATTR(route)->communities);
        }
        if (BGP_ROUTE_ATTR(route)->ext_communities_len > 0)
        {
            char ext_comm[BGP_ATTR_COMMUNITY_MAX];
            bgp_ext_community_format(BGP_ROUTE_ATTR(route)->ext_communities, BGP_ROUTE_ATTR(route)->ext_communities_len,
                                     ext_comm, sizeof(ext_comm));
            g_string_append_printf(buf, "    Ext-Comm : %s\r\n", ext_comm);
        }
        if (BGP_ROUTE_ATTR(route)->large_communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Lrg-Comm : %s\r\n", BGP_ROUTE_ATTR(route)->large_communities);
        }
        if (BGP_ROUTE_ATTR(route)->aggregator[0] != '\0')
        {
            g_string_append_printf(buf, "    Aggregator: %s\r\n", BGP_ROUTE_ATTR(route)->aggregator);
        }
        if (BGP_ROUTE_ATTR(route)->has_originator_id)
        {
            char oid[64];
            net_addr_to_str(&BGP_ROUTE_ATTR(route)->originator_id, oid, sizeof(oid));
            g_string_append_printf(buf, "    Originator: %s\r\n", oid);
        }
        if (BGP_ROUTE_ATTR(route)->cluster_list_len > 0)
        {
            g_string_append(buf, "    Cluster-List:");
            for (uint8_t i = 0; i < BGP_ROUTE_ATTR(route)->cluster_list_len; i++)
            {
                char cid[64];
                net_addr_to_str(&BGP_ROUTE_ATTR(route)->cluster_list[i], cid, sizeof(cid));
                g_string_append_printf(buf, " %s", cid);
            }
            g_string_append(buf, "\r\n");
        }
        g_string_append_printf(buf, "    Added    : %s\r\n", ts_added);
        g_string_append_printf(buf, "    Updated  : %s\r\n\r\n", ts_updated);
    }
}

static uint8_t bgp_show_qp_dqpn_wire_bits(uint32_t dqpn)
{
    if (dqpn <= 0xFFu)
    {
        return 8;
    }
    if (dqpn <= 0xFFFFu)
    {
        return 16;
    }
    return 24;
}

static gboolean bgp_show_parse_qp_query(const char *query, bgp_afi_t afi, bgp_nlri_entry_t *nlri, char *err,
                                        size_t err_sz)
{
    if (err && err_sz > 0)
    {
        err[0] = '\0';
    }
    if (!query || !nlri)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query format.\r\n");
        }
        return FALSE;
    }

    const char *expected_prefix = (afi == BGP_AFI_IPV6) ? "ipv6=" : "ip=";
    const char *comma = strchr(query, ',');
    if (!comma || comma == query || comma[1] == '\0' || strchr(comma + 1, ','))
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query format. Use dqpn=<n>,%s<prefix>/<mask>.\r\n",
                     expected_prefix);
        }
        return FALSE;
    }

    if (strncmp(query, "dqpn=", 5) != 0)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query format. Use dqpn=<n>,%s<prefix>/<mask>.\r\n",
                     expected_prefix);
        }
        return FALSE;
    }

    char dqpn_buf[32];
    size_t dqpn_len = (size_t)(comma - (query + 5));
    if (dqpn_len == 0 || dqpn_len >= sizeof(dqpn_buf))
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query DQPN.\r\n");
        }
        return FALSE;
    }
    memcpy(dqpn_buf, query + 5, dqpn_len);
    dqpn_buf[dqpn_len] = '\0';

    char *endp = NULL;
    unsigned long dqpn_ul = strtoul(dqpn_buf, &endp, 10);
    if (!endp || *endp != '\0' || dqpn_ul == 0ul || dqpn_ul > 0xFFFFFFul)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query DQPN.\r\n");
        }
        return FALSE;
    }

    const char *prefix_part = comma + 1;
    if (strncmp(prefix_part, expected_prefix, strlen(expected_prefix)) != 0)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query format. Use dqpn=<n>,%s<prefix>/<mask>.\r\n",
                     expected_prefix);
        }
        return FALSE;
    }

    const char *prefix_value = prefix_part + strlen(expected_prefix);
    const char *slash = strrchr(prefix_value, '/');
    if (!slash || slash == prefix_value || slash[1] == '\0')
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query format. Use dqpn=<n>,%s<prefix>/<mask>.\r\n",
                     expected_prefix);
        }
        return FALSE;
    }

    char addr_buf[INET6_ADDRSTRLEN];
    size_t addr_len = (size_t)(slash - prefix_value);
    if (addr_len == 0 || addr_len >= sizeof(addr_buf))
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query prefix.\r\n");
        }
        return FALSE;
    }
    memcpy(addr_buf, prefix_value, addr_len);
    addr_buf[addr_len] = '\0';

    net_addr_t addr = {0};
    if (net_addr_from_str(addr_buf, &addr) != 0)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query prefix.\r\n");
        }
        return FALSE;
    }
    if ((afi == BGP_AFI_IPV4 && addr.family != AF_INET) || (afi == BGP_AFI_IPV6 && addr.family != AF_INET6))
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: QP route query prefix AF mismatch.\r\n");
        }
        return FALSE;
    }

    endp = NULL;
    unsigned long mask_ul = strtoul(slash + 1, &endp, 10);
    unsigned long max_mask = (afi == BGP_AFI_IPV6) ? 128ul : 32ul;
    if (!endp || *endp != '\0' || mask_ul > max_mask)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid QP route query prefix length.\r\n");
        }
        return FALSE;
    }

    memset(nlri, 0, sizeof(*nlri));
    nlri->afi = (uint16_t)afi;
    nlri->safi = BGP_SAFI_QP;
    nlri->type = BGP_NLRI_QP;
    nlri->qp.dqpn = (uint32_t)dqpn_ul;
    nlri->qp.dqpn_len = bgp_show_qp_dqpn_wire_bits(nlri->qp.dqpn);
    nlri->qp.prefix.addr = addr;
    nlri->qp.prefix.prefix_len = (uint8_t)mask_ul;
    return TRUE;
}

static int bgp_show_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

static gboolean bgp_show_parse_evpn_esi(const char *s, bgp_esi_t *esi)
{
    if (!s || !esi)
    {
        return FALSE;
    }

    const char *p = s;
    for (uint8_t i = 0; i < 10; i++)
    {
        int hi = bgp_show_hex_nibble(p[0]);
        int lo = bgp_show_hex_nibble(p[1]);
        if (hi < 0 || lo < 0)
        {
            return FALSE;
        }
        esi->bytes[i] = (uint8_t)((hi << 4) | lo);
        p += 2;
        if (i < 9)
        {
            if (*p != ':')
            {
                return FALSE;
            }
            p++;
        }
    }
    return *p == '\0';
}

static uint16_t bgp_show_evpn_type5_build_raw(const bgp_nlri_evpn_t *e, uint8_t raw[512])
{
    if (!e || !raw || (e->ip_prefix.addr.family != AF_INET && e->ip_prefix.addr.family != AF_INET6))
    {
        return 0;
    }

    uint8_t addr_bytes = (e->ip_prefix.addr.family == AF_INET6) ? 16u : 4u;
    uint8_t pfx_bytes = (uint8_t)((e->ip_prefix.prefix_len + 7u) / 8u);
    if (pfx_bytes > addr_bytes)
    {
        return 0;
    }

    uint8_t vlen = (uint8_t)(8u + 10u + 4u + 1u + pfx_bytes + addr_bytes + 3u);
    uint16_t pos = 0;
    raw[pos++] = 5u;
    raw[pos++] = vlen;
    memcpy(raw + pos, e->rd.bytes, 8);
    pos += 8;
    memcpy(raw + pos, e->esi.bytes, 10);
    pos += 10;
    raw[pos++] = (uint8_t)(e->eth_tag >> 24);
    raw[pos++] = (uint8_t)(e->eth_tag >> 16);
    raw[pos++] = (uint8_t)(e->eth_tag >> 8);
    raw[pos++] = (uint8_t)e->eth_tag;
    raw[pos++] = (uint8_t)(pfx_bytes * 8u);
    if (pfx_bytes > 0)
    {
        if (e->ip_prefix.addr.family == AF_INET)
        {
            memcpy(raw + pos, &e->ip_prefix.addr.u.v4, pfx_bytes);
        }
        else
        {
            memcpy(raw + pos, e->ip_prefix.addr.u.v6.s6_addr, pfx_bytes);
        }
        pos += pfx_bytes;
    }
    if (e->gw_ip.family == AF_INET)
    {
        memcpy(raw + pos, &e->gw_ip.u.v4, 4);
    }
    else if (e->gw_ip.family == AF_INET6)
    {
        memcpy(raw + pos, e->gw_ip.u.v6.s6_addr, 16);
    }
    else
    {
        memset(raw + pos, 0, addr_bytes);
    }
    pos += addr_bytes;
    raw[pos++] = (uint8_t)(e->label1 >> 12);
    raw[pos++] = (uint8_t)(e->label1 >> 4);
    raw[pos++] = (uint8_t)(((e->label1 & 0xFu) << 4) | 0x01u);
    return pos;
}

static gboolean bgp_show_parse_evpn_prefix_value(const char *value, net_prefix_t *prefix)
{
    if (!value || !prefix)
    {
        return FALSE;
    }

    const char *slash = strrchr(value, '/');
    if (!slash || slash == value || slash[1] == '\0')
    {
        return FALSE;
    }

    char addr_buf[INET6_ADDRSTRLEN];
    size_t addr_len = (size_t)(slash - value);
    if (addr_len == 0 || addr_len >= sizeof(addr_buf))
    {
        return FALSE;
    }
    memcpy(addr_buf, value, addr_len);
    addr_buf[addr_len] = '\0';

    net_addr_t addr = {0};
    if (net_addr_from_str(addr_buf, &addr) != 0 || (addr.family != AF_INET && addr.family != AF_INET6))
    {
        return FALSE;
    }

    char *endp = NULL;
    unsigned long mask_ul = strtoul(slash + 1, &endp, 10);
    unsigned long max_mask = (addr.family == AF_INET6) ? 128ul : 32ul;
    if (!endp || *endp != '\0' || mask_ul > max_mask)
    {
        return FALSE;
    }

    uint8_t wire_mask = (uint8_t)(((mask_ul + 7ul) / 8ul) * 8ul);
    if (net_addr_prefix_normalize(&addr, wire_mask) != 0)
    {
        return FALSE;
    }

    memset(prefix, 0, sizeof(*prefix));
    prefix->addr = addr;
    prefix->prefix_len = wire_mask;
    return TRUE;
}

static gboolean bgp_show_parse_evpn_query(const char *query, bgp_nlri_entry_t *nlri, char *err, size_t err_sz)
{
    if (err && err_sz > 0)
    {
        err[0] = '\0';
    }
    if (!query || !nlri)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid EVPN route query format.\r\n");
        }
        return FALSE;
    }

    char work[BGP_NLRI_KEY_MAX];
    if (strlen(query) >= sizeof(work))
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: EVPN route query is too long.\r\n");
        }
        return FALSE;
    }
    snprintf(work, sizeof(work), "%s", query);

    bgp_nlri_entry_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.afi = BGP_AFI_L2VPN;
    tmp.safi = BGP_SAFI_EVPN;
    tmp.type = BGP_NLRI_EVPN;
    tmp.evpn.route_type = 5;

    gboolean has_type = FALSE;
    gboolean has_rd = FALSE;
    gboolean has_ethag = FALSE;
    gboolean has_prefix = FALSE;

    char *saveptr = NULL;
    for (char *tok = strtok_r(work, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr))
    {
        if (g_str_has_prefix(tok, "evpn:"))
        {
            tok += 5;
        }
        char *eq = strchr(tok, '=');
        if (!eq || eq == tok || eq[1] == '\0')
        {
            if (err && err_sz > 0)
            {
                snprintf(err, err_sz,
                         "BGP Error: Invalid EVPN route query format. Use "
                         "evpn:type=5,rd=<rd>,ethag=<n>,prefix=<prefix>/<mask>.\r\n");
            }
            return FALSE;
        }
        *eq = '\0';
        const char *key = tok;
        const char *value = eq + 1;

        if (strcmp(key, "type") == 0)
        {
            char *endp = NULL;
            unsigned long type_ul = strtoul(value, &endp, 10);
            if (!endp || *endp != '\0' || type_ul != 5ul)
            {
                if (err && err_sz > 0)
                {
                    snprintf(err, err_sz, "BGP Error: Only EVPN type=5 route query is supported.\r\n");
                }
                return FALSE;
            }
            has_type = TRUE;
        }
        else if (strcmp(key, "rd") == 0)
        {
            if (bgp_show_rd_from_str(value, &tmp.evpn.rd) != 0)
            {
                if (err && err_sz > 0)
                {
                    snprintf(err, err_sz, "BGP Error: Invalid EVPN route query RD.\r\n");
                }
                return FALSE;
            }
            has_rd = TRUE;
        }
        else if (strcmp(key, "esi") == 0)
        {
            if (!bgp_show_parse_evpn_esi(value, &tmp.evpn.esi))
            {
                if (err && err_sz > 0)
                {
                    snprintf(err, err_sz, "BGP Error: Invalid EVPN route query ESI.\r\n");
                }
                return FALSE;
            }
        }
        else if (strcmp(key, "ethag") == 0)
        {
            char *endp = NULL;
            unsigned long ethag_ul = strtoul(value, &endp, 10);
            if (!endp || *endp != '\0' || ethag_ul > 0xFFFFFFFFul)
            {
                if (err && err_sz > 0)
                {
                    snprintf(err, err_sz, "BGP Error: Invalid EVPN route query Ethernet Tag.\r\n");
                }
                return FALSE;
            }
            tmp.evpn.eth_tag = (uint32_t)ethag_ul;
            has_ethag = TRUE;
        }
        else if (strcmp(key, "prefix") == 0)
        {
            if (!bgp_show_parse_evpn_prefix_value(value, &tmp.evpn.ip_prefix))
            {
                if (err && err_sz > 0)
                {
                    snprintf(err, err_sz, "BGP Error: Invalid EVPN route query prefix.\r\n");
                }
                return FALSE;
            }
            has_prefix = TRUE;
        }
        else if (strcmp(key, "gw") == 0)
        {
            if (net_addr_from_str(value, &tmp.evpn.gw_ip) != 0 ||
                (tmp.evpn.gw_ip.family != AF_INET && tmp.evpn.gw_ip.family != AF_INET6))
            {
                if (err && err_sz > 0)
                {
                    snprintf(err, err_sz, "BGP Error: Invalid EVPN route query gateway.\r\n");
                }
                return FALSE;
            }
        }
        else if (strcmp(key, "label") == 0)
        {
            char *endp = NULL;
            unsigned long label_ul = strtoul(value, &endp, 10);
            if (!endp || *endp != '\0' || label_ul > 0xFFFFFul)
            {
                if (err && err_sz > 0)
                {
                    snprintf(err, err_sz, "BGP Error: Invalid EVPN route query label.\r\n");
                }
                return FALSE;
            }
            tmp.evpn.label1 = (uint32_t)label_ul;
        }
    }

    if (!has_type || !has_rd || !has_ethag || !has_prefix)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz,
                     "BGP Error: Invalid EVPN route query format. Use "
                     "evpn:type=5,rd=<rd>,ethag=<n>,prefix=<prefix>/<mask>.\r\n");
        }
        return FALSE;
    }

    if (tmp.evpn.gw_ip.family != 0 && tmp.evpn.gw_ip.family != tmp.evpn.ip_prefix.addr.family)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: EVPN route query gateway AF mismatch.\r\n");
        }
        return FALSE;
    }
    if (tmp.evpn.gw_ip.family == 0)
    {
        tmp.evpn.gw_ip.family = tmp.evpn.ip_prefix.addr.family;
    }

    tmp.evpn.raw_len = bgp_show_evpn_type5_build_raw(&tmp.evpn, tmp.evpn.raw);
    if (tmp.evpn.raw_len == 0)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid EVPN route query key.\r\n");
        }
        return FALSE;
    }

    *nlri = tmp;
    return TRUE;
}

static gboolean bgp_show_parse_prefix_query(const char *query, bgp_afi_t afi, bgp_safi_t safi, bgp_nlri_entry_t *nlri,
                                            char *err, size_t err_sz)
{
    if (err && err_sz > 0)
    {
        err[0] = '\0';
    }
    if (!query || !nlri)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid prefix query.\r\n");
        }
        return FALSE;
    }
    if (safi == BGP_SAFI_QP)
    {
        return bgp_show_parse_qp_query(query, afi, nlri, err, err_sz);
    }
    if (afi == BGP_AFI_L2VPN && safi == BGP_SAFI_EVPN)
    {
        return bgp_show_parse_evpn_query(query, nlri, err, err_sz);
    }

    const char *slash = strrchr(query, '/');
    if (!slash || slash == query || slash[1] == '\0')
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Prefix must use CIDR format, e.g. 10.0.0.0/24.\r\n");
        }
        return FALSE;
    }

    char addr_buf[INET6_ADDRSTRLEN];
    size_t addr_len = (size_t)(slash - query);
    if (addr_len == 0 || addr_len >= sizeof(addr_buf))
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid prefix address.\r\n");
        }
        return FALSE;
    }
    memcpy(addr_buf, query, addr_len);
    addr_buf[addr_len] = '\0';

    net_addr_t addr = {0};
    if (net_addr_from_str(addr_buf, &addr) != 0)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid prefix address.\r\n");
        }
        return FALSE;
    }
    if ((afi == BGP_AFI_IPV4 && addr.family != AF_INET) || (afi == BGP_AFI_IPV6 && addr.family != AF_INET6))
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Prefix AF mismatch.\r\n");
        }
        return FALSE;
    }

    char *endp = NULL;
    unsigned long mask_ul = strtoul(slash + 1, &endp, 10);
    unsigned long max_mask = (afi == BGP_AFI_IPV6) ? 128ul : 32ul;
    if (!endp || *endp != '\0' || mask_ul > max_mask)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid prefix length.\r\n");
        }
        return FALSE;
    }
    if (net_addr_prefix_normalize(&addr, (uint8_t)mask_ul) != 0)
    {
        if (err && err_sz > 0)
        {
            snprintf(err, err_sz, "BGP Error: Invalid prefix.\r\n");
        }
        return FALSE;
    }

    memset(nlri, 0, sizeof(*nlri));
    nlri->afi = (uint16_t)afi;
    nlri->safi = (uint8_t)safi;
    nlri->type = BGP_NLRI_PREFIX;
    nlri->prefix.prefix.addr = addr;
    nlri->prefix.prefix.prefix_len = (uint8_t)mask_ul;
    return TRUE;
}

static gboolean bgp_show_evpn_type5_matches(const bgp_nlri_entry_t *nlri, const bgp_nlri_entry_t *filter)
{
    if (!nlri || !filter)
    {
        return FALSE;
    }
    if (nlri->afi != BGP_AFI_L2VPN || nlri->safi != BGP_SAFI_EVPN || nlri->type != BGP_NLRI_EVPN ||
        filter->afi != BGP_AFI_L2VPN || filter->safi != BGP_SAFI_EVPN || filter->type != BGP_NLRI_EVPN)
    {
        return FALSE;
    }
    if (nlri->evpn.route_type != 5 || filter->evpn.route_type != 5)
    {
        return FALSE;
    }
    return memcmp(nlri->evpn.rd.bytes, filter->evpn.rd.bytes, sizeof(nlri->evpn.rd.bytes)) == 0 &&
           nlri->evpn.eth_tag == filter->evpn.eth_tag &&
           nlri->evpn.ip_prefix.prefix_len == filter->evpn.ip_prefix.prefix_len &&
           net_addr_equal(&nlri->evpn.ip_prefix.addr, &filter->evpn.ip_prefix.addr);
}

static gboolean bgp_show_rib_in_nlri_matches(const bgp_nlri_entry_t *nlri, const bgp_nlri_entry_t *filter,
                                             gboolean filter_is_vpn_prefix)
{
    if (!filter)
    {
        return TRUE;
    }
    if (!nlri)
    {
        return FALSE;
    }
    if (filter_is_vpn_prefix)
    {
        return nlri->type == BGP_NLRI_PREFIX && nlri->afi == filter->afi && nlri->safi == filter->safi &&
               nlri->prefix.prefix.prefix_len == filter->prefix.prefix.prefix_len &&
               net_addr_equal(&nlri->prefix.prefix.addr, &filter->prefix.prefix.addr);
    }
    if (filter->afi == BGP_AFI_L2VPN && filter->safi == BGP_SAFI_EVPN && filter->type == BGP_NLRI_EVPN)
    {
        return bgp_show_evpn_type5_matches(nlri, filter);
    }
    return bgp_nlri_equal(nlri, filter);
}

static void bgp_show_rib_in_width_foreach(gpointer key, gpointer value, gpointer user_data)
{
    (void)value;
    const bgp_nlri_entry_t *nlri = (const bgp_nlri_entry_t *)key;
    bgp_show_rib_in_ctx_t *ctx = (bgp_show_rib_in_ctx_t *)user_data;
    if (!nlri || !ctx || !bgp_show_rib_in_nlri_matches(nlri, ctx->filter, ctx->filter_is_vpn_prefix))
    {
        return;
    }

    char nlri_str[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
    guint required_width = (guint)strlen(nlri_str);
    if (required_width > ctx->net_col_width)
    {
        ctx->net_col_width = required_width;
    }
}

static void bgp_show_rib_in_route_foreach(gpointer key, gpointer value, gpointer user_data)
{
    const bgp_nlri_entry_t *nlri = (const bgp_nlri_entry_t *)key;
    const bgp_adj_rib_in_entry_t *entry = (const bgp_adj_rib_in_entry_t *)value;
    bgp_show_rib_in_ctx_t *ctx = (bgp_show_rib_in_ctx_t *)user_data;
    if (!nlri || !entry || !ctx || !bgp_show_rib_in_nlri_matches(nlri, ctx->filter, ctx->filter_is_vpn_prefix))
    {
        return;
    }

    char nlri_str[BGP_NLRI_KEY_MAX];
    char nh[64], lp[16], med[16], as_path[64];
    bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
    bgp_nexthop_to_str(&entry->nexthop, nh, sizeof(nh));
    bgp_attr_fmt_fields(entry->attr_ref ? &entry->attr_ref->attr : NULL, lp, sizeof(lp), med, sizeof(med), as_path,
                        sizeof(as_path));

    const bgp_attr_t *attr = entry->attr_ref ? &entry->attr_ref->attr : NULL;
    g_string_append_printf(ctx->buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", (int)ctx->net_col_width, nlri_str,
                           BGP_RT_COL_NH, nh, BGP_RT_COL_LP, lp, BGP_RT_COL_MED, med, BGP_RT_COL_ORIG,
                           bgp_origin_str(attr ? attr->origin : BGP_ORIGIN_INCOMPLETE), as_path);
    ctx->listed_routes++;
}

static const char *bgp_show_nh_rule_str(bgp_nh_rule_t rule)
{
    switch (rule)
    {
        case BGP_NH_RULE_LOCAL:
            return "LOCAL";
        case BGP_NH_RULE_PASS:
            return "PASS";
        case BGP_NH_RULE_CONFIG:
            return "CONFIG";
        default:
            return "UNKNOWN";
    }
}

static void bgp_show_rib_out_width_cb(const bgp_nlri_entry_t *nlri, const bgp_adj_rib_out_entry_t *entry,
                                      gpointer user_data)
{
    (void)entry;
    bgp_show_rib_out_ctx_t *ctx = (bgp_show_rib_out_ctx_t *)user_data;
    if (!nlri || !ctx || !bgp_show_rib_in_nlri_matches(nlri, ctx->filter, ctx->filter_is_vpn_prefix))
    {
        return;
    }

    char nlri_str[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
    guint required_width = (guint)strlen(nlri_str);
    if (required_width > ctx->net_col_width)
    {
        ctx->net_col_width = required_width;
    }
}

static void bgp_show_rib_out_route_cb(const bgp_nlri_entry_t *nlri, const bgp_adj_rib_out_entry_t *entry,
                                      gpointer user_data)
{
    bgp_show_rib_out_ctx_t *ctx = (bgp_show_rib_out_ctx_t *)user_data;
    if (!nlri || !entry || !ctx || !bgp_show_rib_in_nlri_matches(nlri, ctx->filter, ctx->filter_is_vpn_prefix))
    {
        return;
    }

    char nlri_str[BGP_NLRI_KEY_MAX];
    char nh[64], lp[16], med[16], as_path[64];
    bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
    bgp_nexthop_to_str(&entry->nexthop, nh, sizeof(nh));
    bgp_attr_fmt_fields(entry->attr_ref ? &entry->attr_ref->attr : NULL, lp, sizeof(lp), med, sizeof(med), as_path,
                        sizeof(as_path));

    const bgp_attr_t *attr = entry->attr_ref ? &entry->attr_ref->attr : NULL;
    g_string_append_printf(ctx->buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", (int)ctx->net_col_width, nlri_str,
                           BGP_RT_COL_NH, nh, BGP_RT_COL_LP, lp, BGP_RT_COL_MED, med, BGP_RT_COL_ORIG,
                           bgp_origin_str(attr ? attr->origin : BGP_ORIGIN_INCOMPLETE), as_path);
    ctx->listed_routes++;
}

static int bgp_show_peer_rib_in(dev_ipc_message_t *msg, const bgp_cli_ctx_t *ctx, bgp_instance_t *inst,
                                const net_addr_t *peer_addr, const char *prefix_filter)
{
    if (!inst || !inst->peer_hash)
    {
        bgp_show_send_cli_response(msg, "BGP Error: AF instance not found.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_peer_t *peer = (bgp_peer_t *)g_hash_table_lookup(inst->peer_hash, peer_addr);
    if (!peer)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Peer not found in this AF/VRF.\r\n");
        return ERRCODE_FAIL;
    }
    if (!peer->rib_in || !peer->rib_in->table)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Peer Adj-RIB-In not available.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_nlri_entry_t filter_nlri;
    bgp_nlri_entry_t *filter = NULL;
    gboolean filter_is_vpn_prefix = FALSE;
    char filter_str[BGP_NLRI_KEY_MAX] = "-";
    if (prefix_filter && prefix_filter[0] != '\0')
    {
        char err[160];
        if (!bgp_show_parse_prefix_query(prefix_filter, ctx->afi, ctx->safi, &filter_nlri, err, sizeof(err)))
        {
            bgp_show_send_cli_response(msg, err);
            return ERRCODE_FAIL;
        }
        filter = &filter_nlri;
        filter_is_vpn_prefix = bgp_safi_is_vpn(ctx->safi) && filter_nlri.type == BGP_NLRI_PREFIX;
        bgp_nlri_to_str(filter, filter_str, sizeof(filter_str));
    }

    GString *resp_buf = g_string_new("");
    if (!resp_buf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    char peer_str[64];
    net_addr_to_str(peer_addr, peer_str, sizeof(peer_str));
    g_string_append_printf(resp_buf, "\r\nBGP Peer Adj-RIB-In (AF: %s, VRF: %s)\r\n", bgp_af_str(ctx->afi, ctx->safi),
                           ctx->vrf_name);
    g_string_append(resp_buf, "============================================================\r\n");
    g_string_append_printf(resp_buf, "  Peer   : %s\r\n", peer_str);
    g_string_append_printf(resp_buf, "  Prefix : %s\r\n", filter ? filter_str : "all");
    g_string_append_printf(resp_buf, "  Routes : %u\r\n\r\n", bgp_adj_rib_in_count(peer->rib_in));

    bgp_show_rib_in_ctx_t show_ctx;
    memset(&show_ctx, 0, sizeof(show_ctx));
    show_ctx.buf = resp_buf;
    show_ctx.net_col_width = BGP_RT_COL_NET_MIN;
    show_ctx.filter = filter;
    show_ctx.filter_is_vpn_prefix = filter_is_vpn_prefix;

    g_hash_table_foreach(peer->rib_in->table, bgp_show_rib_in_width_foreach, &show_ctx);

    char *net_rule = g_strnfill(show_ctx.net_col_width, '-');
    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", (int)show_ctx.net_col_width, "Network",
                           BGP_RT_COL_NH, "NextHop", BGP_RT_COL_LP, "LocPref", BGP_RT_COL_MED, "MED", BGP_RT_COL_ORIG,
                           "Origin", "AS-Path");
    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", (int)show_ctx.net_col_width, net_rule,
                           BGP_RT_COL_NH, "--------------------", BGP_RT_COL_LP, "--------", BGP_RT_COL_MED, "--------",
                           BGP_RT_COL_ORIG, "------------", "--------");
    g_free(net_rule);

    g_hash_table_foreach(peer->rib_in->table, bgp_show_rib_in_route_foreach, &show_ctx);

    if (show_ctx.listed_routes == 0)
    {
        g_string_append(resp_buf, "  (no received routes)\r\n");
    }
    g_string_append_printf(resp_buf, "\r\nTotal: %u received routes\r\n\r\n", show_ctx.listed_routes);
    return bgp_work_send_chunked_response(msg, resp_buf);
}

static int bgp_show_peer_rib_out(dev_ipc_message_t *msg, const bgp_cli_ctx_t *ctx, bgp_instance_t *inst,
                                 const net_addr_t *peer_addr, const char *prefix_filter)
{
    if (!inst || !inst->peer_hash)
    {
        bgp_show_send_cli_response(msg, "BGP Error: AF instance not found.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_peer_t *peer = (bgp_peer_t *)g_hash_table_lookup(inst->peer_hash, peer_addr);
    if (!peer)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Peer not found in this AF/VRF.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_nlri_entry_t filter_nlri;
    bgp_nlri_entry_t *filter = NULL;
    gboolean filter_is_vpn_prefix = FALSE;
    char filter_str[BGP_NLRI_KEY_MAX] = "-";
    if (prefix_filter && prefix_filter[0] != '\0')
    {
        char err[160];
        if (!bgp_show_parse_prefix_query(prefix_filter, ctx->afi, ctx->safi, &filter_nlri, err, sizeof(err)))
        {
            bgp_show_send_cli_response(msg, err);
            return ERRCODE_FAIL;
        }
        filter = &filter_nlri;
        filter_is_vpn_prefix = bgp_safi_is_vpn(ctx->safi) && filter_nlri.type == BGP_NLRI_PREFIX;
        bgp_nlri_to_str(filter, filter_str, sizeof(filter_str));
    }

    GString *resp_buf = g_string_new("");
    if (!resp_buf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    char peer_str[64];
    net_addr_to_str(peer_addr, peer_str, sizeof(peer_str));
    g_string_append_printf(resp_buf, "\r\nBGP Peer Adj-RIB-Out (AF: %s, VRF: %s)\r\n", bgp_af_str(ctx->afi, ctx->safi),
                           ctx->vrf_name);
    g_string_append(resp_buf, "============================================================\r\n");
    g_string_append_printf(resp_buf, "  Peer   : %s\r\n", peer_str);
    g_string_append_printf(resp_buf, "  Prefix : %s\r\n", filter ? filter_str : "all");

    uint32_t total_routes = 0;
    for (const GList *sl = peer->subgroups; sl; sl = sl->next)
    {
        const bgp_nh_subgroup_t *sg = (const bgp_nh_subgroup_t *)sl->data;
        total_routes += bgp_adj_rib_out_count(sg ? sg->adj_rib_out : NULL);
    }
    g_string_append_printf(resp_buf, "  Routes : %u\r\n\r\n", total_routes);

    bgp_show_rib_out_ctx_t show_ctx;
    memset(&show_ctx, 0, sizeof(show_ctx));
    show_ctx.buf = resp_buf;
    show_ctx.net_col_width = BGP_RT_COL_NET_MIN;
    show_ctx.filter = filter;
    show_ctx.filter_is_vpn_prefix = filter_is_vpn_prefix;

    for (const GList *sl = peer->subgroups; sl; sl = sl->next)
    {
        const bgp_nh_subgroup_t *sg = (const bgp_nh_subgroup_t *)sl->data;
        bgp_adj_rib_out_foreach(sg ? sg->adj_rib_out : NULL, bgp_show_rib_out_width_cb, &show_ctx);
    }

    char *net_rule = g_strnfill(show_ctx.net_col_width, '-');
    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", (int)show_ctx.net_col_width, "Network",
                           BGP_RT_COL_NH, "NextHop", BGP_RT_COL_LP, "LocPref", BGP_RT_COL_MED, "MED", BGP_RT_COL_ORIG,
                           "Origin", "AS-Path");
    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", (int)show_ctx.net_col_width, net_rule,
                           BGP_RT_COL_NH, "--------------------", BGP_RT_COL_LP, "--------", BGP_RT_COL_MED, "--------",
                           BGP_RT_COL_ORIG, "------------", "--------");
    g_free(net_rule);

    uint32_t sg_index = 0;
    for (const GList *sl = peer->subgroups; sl; sl = sl->next)
    {
        const bgp_nh_subgroup_t *sg = (const bgp_nh_subgroup_t *)sl->data;
        if (!sg || !sg->adj_rib_out || bgp_adj_rib_out_count(sg->adj_rib_out) == 0)
        {
            continue;
        }
        sg_index++;
        gsize header_pos = resp_buf->len;
        uint32_t before = show_ctx.listed_routes;
        if (sg->parent)
        {
            g_string_append_printf(resp_buf, "\r\n  Update-Group %u / Subgroup %u (%s)\r\n", sg->parent->group_id,
                                   sg_index, bgp_show_nh_rule_str(sg->key.rule));
        }
        bgp_adj_rib_out_foreach(sg->adj_rib_out, bgp_show_rib_out_route_cb, &show_ctx);
        if (show_ctx.listed_routes == before && filter)
        {
            g_string_truncate(resp_buf, header_pos);
        }
    }

    if (show_ctx.listed_routes == 0)
    {
        g_string_append(resp_buf, "  (no advertised routes)\r\n");
    }
    g_string_append_printf(resp_buf, "\r\nTotal: %u advertised routes\r\n\r\n", show_ctx.listed_routes);
    return bgp_work_send_chunked_response(msg, resp_buf);
}

typedef struct bgp_show_evpn_detail_ctx
{
    GString *buf;
    const bgp_nlri_entry_t *filter;
    const char *rd_str;
    uint32_t found;
} bgp_show_evpn_detail_ctx_t;

static gboolean bgp_show_evpn_detail_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const bgp_rthead_t *head = (const bgp_rthead_t *)value;
    bgp_show_evpn_detail_ctx_t *ctx = (bgp_show_evpn_detail_ctx_t *)user_data;
    if (!head || !ctx || !bgp_show_evpn_type5_matches(&head->nlri, ctx->filter))
    {
        return FALSE;
    }

    g_string_append_printf(ctx->buf, "\r\n RD: %s\r\n", ctx->rd_str ? ctx->rd_str : "-");
    bgp_show_route_detail(ctx->buf, head);
    ctx->found++;
    return FALSE;
}

/**
 * @brief 处理 show bgp route af ipv4-unicast|ipv6-unicast [<ip> <masklen>] / ipv4-qp|ipv6-qp [<qp-key>] 命令
 *
 * group_id=10, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=ip-address, 4=masklen, 7=qp-route-key
 * 不带查询参数时显示路由表；unicast 详情使用 ip/masklen，QP 详情使用 dqpn=<n>,ip=<pfx>/<mask>
 */
int handle_bgp_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    gboolean has_af = FALSE;
    char ip_str[64] = {0};
    char qp_query[256] = {0};
    uint32_t masklen = 0;
    gboolean has_ip = FALSE;
    gboolean has_masklen = FALSE;
    gboolean has_qp_query = FALSE;
    bgp_rd_t rd_filter;
    gboolean has_rd_filter = FALSE;
    net_addr_t peer_addr;
    gboolean has_peer = FALSE;
    gboolean show_peer_rib_in = FALSE;
    gboolean show_peer_rib_out = FALSE;
    memset(&peer_addr, 0, sizeof(peer_addr));
    memset(&rd_filter, 0, sizeof(rd_filter));

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            bgp_cli_ctx_parse(&ctx, &entry);
            cli_tlv_entry_free(&entry);
            continue;
        }
        switch (entry.cfg_id)
        {
            case 1:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_UNICAST;
                has_af = TRUE;
                break;
            case 2:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_UNICAST;
                has_af = TRUE;
                break;
            case 3:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ip_str, sizeof(ip_str), "%s", s);
                    has_ip = TRUE;
                }
                break;
            }
            case 4:
                masklen = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_masklen = TRUE;
                break;
            case 5:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_QP;
                has_af = TRUE;
                break;
            case 6:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_QP;
                has_af = TRUE;
                break;
            case 8:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_LABELED;
                has_af = TRUE;
                break;
            case 10:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_VPN_UNICAST;
                has_af = TRUE;
                break;
            case 16:
                ctx.afi = BGP_AFI_L2VPN;
                ctx.safi = BGP_SAFI_EVPN;
                has_af = TRUE;
                break;
            case 7:
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(qp_query, sizeof(qp_query), "%s", s);
                    has_qp_query = TRUE;
                }
                break;
            }
            case 9:
            {
                /* vrf <vrf-name> (仅 ipv4/ipv6 unicast 支持) */
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ctx.vrf_name, sizeof(ctx.vrf_name), "%s", s);
                }
                break;
            }
            case 11:
            {
                /* rd <rd> 过滤（仅 VPN AF 有意义） */
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s && bgp_show_rd_from_str(s, &rd_filter) == 0)
                {
                    has_rd_filter = TRUE;
                }
                break;
            }
            case 12:
            {
                /* peer <peer-ipv4-address> */
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s && net_addr_from_str(s, &peer_addr) == 0)
                {
                    has_peer = TRUE;
                }
                break;
            }
            case 13:
                /* recieve-routes / receive-routes */
                show_peer_rib_in = TRUE;
                break;
            case 14:
                /* advertise-routes */
                show_peer_rib_out = TRUE;
                break;
            case 15:
            {
                /* peer <peer-ipv6-address> */
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s && net_addr_from_str(s, &peer_addr) == 0)
                {
                    has_peer = TRUE;
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_af)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Missing address-family. Use 'af ipv4-unicast', 'af ipv6-unicast', "
                                        "'af ipv4-qp', or 'af ipv6-qp'.\r\n");
        return ERRCODE_FAIL;
    }

    if (!g_bgp_work_local->protocol)
    {
        bgp_show_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_show_lookup_vrf(&ctx);
    if (!vrf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));

    if (show_peer_rib_in || show_peer_rib_out)
    {
        if (!has_peer)
        {
            bgp_show_send_cli_response(msg, "BGP Error: Missing or invalid peer address.\r\n");
            return ERRCODE_FAIL;
        }
        char rib_in_prefix[128] = {0};
        const char *prefix_filter = NULL;
        if (ctx.safi == BGP_SAFI_QP || ctx.safi == BGP_SAFI_EVPN)
        {
            prefix_filter = has_qp_query ? qp_query : NULL;
        }
        else if (has_ip || has_masklen)
        {
            if (!has_ip || !has_masklen)
            {
                bgp_show_send_cli_response(msg,
                                           "BGP Error: Prefix filter requires both IP address and mask length.\r\n");
                return ERRCODE_FAIL;
            }
            snprintf(rib_in_prefix, sizeof(rib_in_prefix), "%s/%u", ip_str, masklen);
            prefix_filter = rib_in_prefix;
        }
        if (show_peer_rib_out)
        {
            return bgp_show_peer_rib_out(msg, &ctx, inst, &peer_addr, prefix_filter);
        }
        return bgp_show_peer_rib_in(msg, &ctx, inst, &peer_addr, prefix_filter);
    }

    GString *resp_buf = g_string_new("");
    if (!resp_buf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    if ((ctx.safi != BGP_SAFI_QP && ctx.safi != BGP_SAFI_EVPN && has_ip && has_masklen) ||
        ((ctx.safi == BGP_SAFI_QP || ctx.safi == BGP_SAFI_EVPN) && has_qp_query))
    {
        bgp_nlri_entry_t nlri;
        memset(&nlri, 0, sizeof(nlri));
        char nlri_str[BGP_NLRI_KEY_MAX];

        if (ctx.safi == BGP_SAFI_QP || ctx.safi == BGP_SAFI_EVPN)
        {
            char err[160];
            if (!bgp_show_parse_prefix_query(qp_query, ctx.afi, ctx.safi, &nlri, err, sizeof(err)))
            {
                g_string_free(resp_buf, TRUE);
                bgp_show_send_cli_response(msg, err);
                return ERRCODE_FAIL;
            }
            bgp_nlri_to_str(&nlri, nlri_str, sizeof(nlri_str));
        }
        else
        {
            nlri.afi = ctx.afi;
            nlri.safi = ctx.safi;
            nlri.type = BGP_NLRI_PREFIX;
            nlri.prefix.prefix.prefix_len = (uint8_t)masklen;
            nlri.prefix.prefix.addr.family = (ctx.afi == BGP_AFI_IPV6) ? AF_INET6 : AF_INET;
            if (inet_pton(nlri.prefix.prefix.addr.family, ip_str, &nlri.prefix.prefix.addr.u) != 1)
            {
                g_string_free(resp_buf, TRUE);
                bgp_show_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
                return ERRCODE_FAIL;
            }
            /* VPN AF 前缀查询不强制带 RD：带 rd 只查该 RD，不带则跨所有 RD 按段列出。 */
            snprintf(nlri_str, sizeof(nlri_str), "%s/%u", ip_str, masklen);
        }
        g_string_append_printf(resp_buf, "\r\nBGP Route Detail: %s (AF: %s)\r\n", nlri_str,
                               bgp_af_str(ctx.afi, ctx.safi));
        g_string_append(resp_buf, "============================================================\r\n");

        if (!inst || !inst->rd_entries)
        {
            g_string_append(resp_buf, "  (no RIB)\r\n");
            return bgp_work_send_chunked_response(msg, resp_buf);
        }

        if (ctx.safi == BGP_SAFI_EVPN && nlri.type == BGP_NLRI_EVPN)
        {
            uint32_t found = 0;
            GHashTableIter rd_iter;
            gpointer rd_key = NULL;
            gpointer rd_val = NULL;
            g_hash_table_iter_init(&rd_iter, inst->rd_entries);
            while (g_hash_table_iter_next(&rd_iter, &rd_key, &rd_val))
            {
                (void)rd_key;
                bgp_rd_entry_t *e = (bgp_rd_entry_t *)rd_val;
                if (!e || !e->rib)
                {
                    continue;
                }
                if (has_rd_filter && memcmp(e->key.rd.bytes, rd_filter.bytes, sizeof(rd_filter.bytes)) != 0)
                {
                    continue;
                }
                char rd_str[48];
                bgp_rd_to_str(&e->key.rd, rd_str, sizeof(rd_str));
                bgp_show_evpn_detail_ctx_t detail_ctx = {
                    .buf = resp_buf,
                    .filter = &nlri,
                    .rd_str = rd_str,
                    .found = 0,
                };
                g_tree_foreach(e->rib->head_tree, bgp_show_evpn_detail_cb, &detail_ctx);
                found += detail_ctx.found;
            }
            if (found == 0)
            {
                g_string_append_printf(resp_buf, "  Route %s not found.\r\n", nlri_str);
            }
            return bgp_work_send_chunked_response(msg, resp_buf);
        }

        /* VPN AF：跨 RD 查同一前缀，按 RD 分段显示（带 rd 过滤时只看匹配的 RD）。 */
        if (bgp_safi_is_vpn(ctx.safi))
        {
            uint32_t found = 0;
            GHashTableIter rd_iter;
            gpointer rd_key = NULL;
            gpointer rd_val = NULL;
            g_hash_table_iter_init(&rd_iter, inst->rd_entries);
            while (g_hash_table_iter_next(&rd_iter, &rd_key, &rd_val))
            {
                (void)rd_key;
                bgp_rd_entry_t *e = (bgp_rd_entry_t *)rd_val;
                if (!e || !e->rib)
                {
                    continue;
                }
                if (has_rd_filter && memcmp(e->key.rd.bytes, rd_filter.bytes, sizeof(rd_filter.bytes)) != 0)
                {
                    continue;
                }
                bgp_nlri_entry_t q = nlri;
                memcpy(q.prefix.rd.bytes, e->key.rd.bytes, sizeof(q.prefix.rd.bytes));
                q.prefix.has_rd = true;
                const bgp_rthead_t *h = bgp_rib_lookup_head(e->rib, &q);
                if (!h)
                {
                    continue;
                }
                char rd_str[48];
                bgp_rd_to_str(&e->key.rd, rd_str, sizeof(rd_str));
                g_string_append_printf(resp_buf, "\r\n RD: %s\r\n", rd_str);
                bgp_show_route_detail(resp_buf, h);
                found++;
            }
            if (found == 0)
            {
                g_string_append_printf(resp_buf, "  Route %s not found.\r\n", nlri_str);
            }
            return bgp_work_send_chunked_response(msg, resp_buf);
        }

        bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, &nlri);
        const bgp_rthead_t *head = rib ? bgp_rib_lookup_head(rib, &nlri) : NULL;
        if (!head)
        {
            g_string_append_printf(resp_buf, "  Route %s not found.\r\n", nlri_str);
            return bgp_work_send_chunked_response(msg, resp_buf);
        }

        bgp_show_route_detail(resp_buf, head);
        return bgp_work_send_chunked_response(msg, resp_buf);
    }

    g_string_append_printf(resp_buf, "\r\nBGP Routes (AF: %s)\r\n", bgp_af_str(ctx.afi, ctx.safi));
    g_string_append(resp_buf, "============================================================\r\n");

    /* 汇总该 AF 下所有 RD 的 RIB 计数 */
    uint32_t total_heads = 0;
    uint32_t total_routes = 0;
    if (inst && inst->rd_entries)
    {
        GHashTableIter rd_iter;
        gpointer rd_key, rd_val;
        g_hash_table_iter_init(&rd_iter, inst->rd_entries);
        while (g_hash_table_iter_next(&rd_iter, &rd_key, &rd_val))
        {
            (void)rd_key;
            const bgp_rd_entry_t *e = (const bgp_rd_entry_t *)rd_val;
            if (!e)
            {
                continue;
            }
            if (has_rd_filter && memcmp(e->key.rd.bytes, rd_filter.bytes, sizeof(rd_filter.bytes)) != 0)
            {
                continue;
            }
            total_heads += bgp_rib_head_count(e->rib);
            total_routes += bgp_rib_route_count(e->rib);
        }
    }
    if (!inst || !inst->rd_entries || total_routes == 0)
    {
        g_string_append(resp_buf, "  (no routes)\r\n\r\n");
        return bgp_work_send_chunked_response(msg, resp_buf);
    }

    g_string_append_printf(resp_buf, "  Networks: %-6u  Paths: %u\r\n\r\n", total_heads, total_routes);
    g_string_append(resp_buf, "  Markers : '>'=BEST, 'v'=VALID\r\n\r\n");

    guint net_col_width = bgp_show_route_network_col_width(inst);
    char *net_rule = g_strnfill(net_col_width, '-');

    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", (int)net_col_width, "Network", BGP_RT_COL_NH,
                           "NextHop", BGP_RT_COL_LP, "LocPref", BGP_RT_COL_MED, "MED", BGP_RT_COL_ORIG, "Origin",
                           "AS-Path");
    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", (int)net_col_width, net_rule, BGP_RT_COL_NH,
                           "--------------------", BGP_RT_COL_LP, "--------", BGP_RT_COL_MED, "--------",
                           BGP_RT_COL_ORIG, "------------", "--------");
    g_free(net_rule);

    bgp_show_route_ctx_t show_ctx;
    show_ctx.buf = resp_buf;
    show_ctx.net_col_width = net_col_width;
    show_ctx.listed_heads = 0;
    show_ctx.listed_routes = 0;

    /* 遍历所有 RD entry 的 RIB（VPN AF 下可能多张并按 RD 分段显示；非 VPN AF 仅公网一张） */
    gboolean is_vpn = bgp_safi_is_vpn(ctx.safi);
    {
        GHashTableIter rd_iter;
        gpointer rd_key, rd_val;
        g_hash_table_iter_init(&rd_iter, inst->rd_entries);
        while (g_hash_table_iter_next(&rd_iter, &rd_key, &rd_val))
        {
            (void)rd_key;
            bgp_rd_entry_t *e = (bgp_rd_entry_t *)rd_val;
            if (!e || !e->rib || !e->rib->head_tree || bgp_rib_head_count(e->rib) == 0)
            {
                continue;
            }
            /* rd 过滤（仅匹配的 RD 显示） */
            if (has_rd_filter && memcmp(e->key.rd.bytes, rd_filter.bytes, sizeof(rd_filter.bytes)) != 0)
            {
                continue;
            }
            /* VPN AF：每个 RD 打印段头，同前缀在不同 RD 下据此区分 */
            if (is_vpn)
            {
                char rd_str[48];
                bgp_rd_to_str(&e->key.rd, rd_str, sizeof(rd_str));
                g_string_append_printf(resp_buf, "\r\n RD: %s\r\n", rd_str);
            }
            g_tree_foreach(e->rib->head_tree, bgp_show_route_head_cb, &show_ctx);
        }
    }

    g_string_append_printf(resp_buf, "\r\nTotal: %u networks, %u paths\r\n\r\n", show_ctx.listed_heads,
                           show_ctx.listed_routes);

    return bgp_work_send_chunked_response(msg, resp_buf);
}
