/**
 * @file   bgp_show.c
 * @brief  BGP show 命令处理（在 BGP worker 线程执行）
 */
#include <arpa/inet.h>
#include <glib.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bgp_bmp.h"
#include "bgp_bmp_cli.h"
#include "bgp_cli.h"
#include "bgp_conn.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

/* show 路径专属分片流状态，仅在 BGP worker 线程访问 */
static cli_chunk_stream_t g_bgp_show_stream;

static void bgp_show_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_bgp_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

static int bgp_work_send_chunked_response(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_bgp_show_stream, bgp_local_ipc_ctx(), DEV_MODULE_ID_BGP, msg, full_text);
}

int bgp_work_handle_continue_msg(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_bgp_show_stream, bgp_local_ipc_ctx(), DEV_MODULE_ID_BGP, msg);
}

void bgp_work_show_cleanup(void)
{
    cli_chunk_stream_reset(&g_bgp_show_stream);
}

typedef struct bgp_cli_ctx
{
    uint32_t vrf_id;
    bgp_afi_t afi;
    bgp_safi_t safi;
} bgp_cli_ctx_t;

static bgp_cli_ctx_t bgp_cli_ctx_default(void)
{
    bgp_cli_ctx_t c = {BGP_VRF_PUBLIC_ID, BGP_AFI_IPV4, BGP_SAFI_UNICAST};
    return c;
}

static void bgp_cli_ctx_parse(bgp_cli_ctx_t *ctx, cli_tlv_entry_t *entry)
{
    switch (entry->cfg_id)
    {
        case CLI_CTX_ID_BGP_VRF:
            ctx->vrf_id = cli_tlv_entry_get_ctx_uint32(entry);
            break;
        case CLI_CTX_ID_BGP_AFI:
            ctx->afi = (bgp_afi_t)cli_tlv_entry_get_ctx_uint32(entry);
            break;
        case CLI_CTX_ID_BGP_SAFI:
            ctx->safi = (bgp_safi_t)cli_tlv_entry_get_ctx_uint32(entry);
            break;
        default:
            break;
    }
}

static const char *bgp_af_str(bgp_afi_t afi, bgp_safi_t safi)
{
    if (afi == BGP_AFI_IPV4 && safi == BGP_SAFI_UNICAST)
    {
        return "ipv4-unicast";
    }
    if (afi == BGP_AFI_IPV6 && safi == BGP_SAFI_UNICAST)
    {
        return "ipv6-unicast";
    }
    return "unknown";
}

/** 返回 session 当前状态字符串 */
static const char *sess_state_str(const bgp_session_t *sess)
{
    const bgp_conn_t *conn = sess->pri_conn ? sess->pri_conn : sess->sec_conn;
    if (!conn || conn->fd == -1)
    {
        return "Idle";
    }
    if (conn->is_connecting)
    {
        return "Connect";
    }
    switch (conn->state)
    {
        case BGP_CONN_STATE_OPEN_SENT:
            return "OpenSent";
        case BGP_CONN_STATE_OPEN_CONFIRM:
            return "OpenConfirm";
        case BGP_CONN_STATE_ESTABLISHED:
            return "Established";
        default:
            return "Unknown";
    }
}

/** 返回能力位对应的可读字符串 */
static const char *cap_yn(uint32_t caps, uint32_t bit)
{
    return BIT_TEST(caps, bit) ? "Yes" : "No";
}

static void bgp_conn_last_error_to_str(const bgp_conn_t *conn, int fallback_error, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }

    int err = fallback_error;
    if (conn && conn->last_socket_error != 0)
    {
        err = conn->last_socket_error;
    }

    if (err == 0)
    {
        snprintf(buf, sz, "0 (none)");
        return;
    }
    snprintf(buf, sz, "%d (%s)", err, strerror(err));
}

/** ORIGIN 可读字符串 */
static const char *bgp_origin_str(bgp_origin_t origin)
{
    switch (origin)
    {
        case BGP_ORIGIN_IGP:
            return "IGP";
        case BGP_ORIGIN_EGP:
            return "EGP";
        case BGP_ORIGIN_INCOMPLETE:
            return "INCOMPLETE";
        default:
            return "UNKNOWN";
    }
}

/** 将 nexthop 结构格式化为单行文本 */
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

/* 路由表固定列宽 */
#define BGP_RT_COL_NET 24
#define BGP_RT_COL_NH 20
#define BGP_RT_COL_LP 8
#define BGP_RT_COL_MED 8
#define BGP_RT_COL_ORIG 12

/** 将 usec 时间戳格式化为本地时间字符串 */
static void bgp_fmt_time_usec(gint64 usec, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    if (usec <= 0)
    {
        snprintf(buf, sz, "-");
        return;
    }
    time_t sec = (time_t)(usec / 1000000);
    struct tm tmv;
    if (!localtime_r(&sec, &tmv))
    {
        snprintf(buf, sz, "-");
        return;
    }
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tmv);
}

typedef struct bgp_show_route_ctx
{
    GString *buf;
    uint32_t listed_heads;
    uint32_t listed_routes;
} bgp_show_route_ctx_t;

/**
 * @brief 将单条路径的各字段格式化到 lp/med/as_path 缓冲区
 */
static void bgp_route_fmt_fields(const bgp_route_node_t *route, char *lp, size_t lp_sz, char *med, size_t med_sz,
                                 char *as_path, size_t as_sz)
{
    if (route->attr.has_local_pref)
    {
        snprintf(lp, lp_sz, "%u", route->attr.local_pref);
    }
    else
    {
        snprintf(lp, lp_sz, "-");
    }
    if (route->attr.has_med)
    {
        snprintf(med, med_sz, "%u", route->attr.med);
    }
    else
    {
        snprintf(med, med_sz, "-");
    }
    if (route->attr.as_path[0] != '\0')
    {
        snprintf(as_path, as_sz, "%.*s", (int)(as_sz - 1), route->attr.as_path);
    }
    else
    {
        snprintf(as_path, as_sz, "-");
    }
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
    bgp_nlri_to_str(&head->nlri, prefix_str, sizeof(prefix_str));
    gboolean first = TRUE;

    for (const GList *l = head->route_list; l; l = l->next)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)l->data;
        if (!route)
        {
            continue;
        }

        char nh[64], lp[16], med[16], as_path[64];
        bgp_nexthop_to_str(&route->nexthop, nh, sizeof(nh));
        bgp_route_fmt_fields(route, lp, sizeof(lp), med, sizeof(med), as_path, sizeof(as_path));

        /* 路由标记：'>'=BEST，'v'=VALID */
        g_string_append_printf(ctx->buf, "%c%c%-*s %-*s %-*s %-*s %-*s %s\r\n",
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_BEST) ? '>' : ' ',
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID) ? 'v' : ' ', BGP_RT_COL_NET - 2,
                               first ? prefix_str : "", BGP_RT_COL_NH, nh, BGP_RT_COL_LP, lp, BGP_RT_COL_MED, med,
                               BGP_RT_COL_ORIG, bgp_origin_str(route->attr.origin), as_path);

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
static void bgp_show_route_detail(GString *buf, const bgp_rthead_t *head)
{
    uint32_t path_count = (uint32_t)g_list_length(head->route_list);
    g_string_append_printf(buf, "  Head QueueRefCnt: %u\r\n", head->queue_refcnt);
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
        bgp_nexthop_to_str(&route->nexthop, nh, sizeof(nh));
        bgp_route_fmt_fields(route, lp, sizeof(lp), med, sizeof(med), as_path, sizeof(as_path));
        bgp_fmt_time_usec(route->added_at_usec, ts_added, sizeof(ts_added));
        bgp_fmt_time_usec(route->updated_at_usec, ts_updated, sizeof(ts_updated));
        bgp_route_flags_to_str(route->flags, flags_str, sizeof(flags_str));

        if (route->iter_watched && route->iter_relay_addr.family != 0)
        {
            net_addr_to_str(&route->iter_relay_addr, iter_nh, sizeof(iter_nh));
        }
        else
        {
            snprintf(iter_nh, sizeof(iter_nh), "-");
        }
        bgp_ifindex_to_str((route->iter_watched && route->iter_resolved) ? route->iter_out_ifindex : 0u, out_if,
                           sizeof(out_if));
        const char *iter_state_str =
            route->iter_watched ? (route->iter_resolved ? "Resolved" : "Unresolved") : "Unwatched";

        /* 路由标记：'>'=BEST，'v'=VALID */
        g_string_append_printf(buf, "%c%c ", BIT_TEST(route->flags, BGP_ROUTE_FLAG_BEST) ? '>' : ' ',
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID) ? 'v' : ' ');
        if (!BIT_TEST(route->flags, BGP_ROUTE_FLAG_IMPORT))
        {
            char peer_str[64];
            net_addr_to_str(&route->source, peer_str, sizeof(peer_str));
            g_string_append_printf(buf, "From Peer  : %s\r\n", peer_str);
        }
        else
        {
            g_string_append_printf(buf, "Imported\r\n");
        }
        g_string_append_printf(buf, "    NextHop  : %s\r\n", nh);
        g_string_append_printf(buf, "    LocPref  : %s\r\n", lp);
        g_string_append_printf(buf, "    MED      : %s\r\n", med);
        g_string_append_printf(buf, "    Origin   : %s\r\n", bgp_origin_str(route->attr.origin));
        g_string_append_printf(buf, "    Valid    : %s\r\n",
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID) ? "Yes" : "No");
        g_string_append_printf(buf, "    IterState: %s\r\n", iter_state_str);
        g_string_append_printf(buf, "    Iter-NH  : %s\r\n", iter_nh);
        g_string_append_printf(buf, "    Out-If   : %s\r\n", out_if);
        g_string_append_printf(buf, "    Flags    : 0x%08X (%s)\r\n", route->flags, flags_str);
        g_string_append_printf(buf, "    AS-Path  : %s\r\n", as_path);

        if (route->attr.communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Community: %s\r\n", route->attr.communities);
        }
        if (route->attr.ext_communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Ext-Comm : %s\r\n", route->attr.ext_communities);
        }
        if (route->attr.large_communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Lrg-Comm : %s\r\n", route->attr.large_communities);
        }
        if (route->attr.aggregator[0] != '\0')
        {
            g_string_append_printf(buf, "    Aggregator: %s\r\n", route->attr.aggregator);
        }
        if (route->attr.has_originator_id)
        {
            char oid[64];
            net_addr_to_str(&route->attr.originator_id, oid, sizeof(oid));
            g_string_append_printf(buf, "    Originator: %s\r\n", oid);
        }
        g_string_append_printf(buf, "    Added    : %s\r\n", ts_added);
        g_string_append_printf(buf, "    Updated  : %s\r\n\r\n", ts_updated);
    }
}

/**
 * @brief 处理 show bgp route af ipv4-unicast|ipv6-unicast [<ip> <masklen>] 命令
 *
 * group_id=10, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=ip-address, 4=masklen
 * 不带 ip/masklen 时显示路由表（table），带时显示单前缀详情
 */
static int handle_bgp_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    gboolean has_af = FALSE;
    char ip_str[64] = {0};
    uint32_t masklen = 0;
    gboolean has_ip = FALSE;
    gboolean has_masklen = FALSE;

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
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_af)
    {
        bgp_show_send_cli_response(
            msg, "BGP Error: Missing address-family. Use 'af ipv4-unicast' or 'af ipv6-unicast'.\r\n");
        return ERRCODE_FAIL;
    }

    if (!g_bgp_work_local->protocol)
    {
        bgp_show_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, ctx.vrf_id);
    if (!vrf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));

    GString *resp_buf = g_string_new("");
    if (!resp_buf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    if (has_ip && has_masklen)
    {
        bgp_nlri_entry_t nlri;
        memset(&nlri, 0, sizeof(nlri));
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
        g_string_append_printf(resp_buf, "\r\nBGP Route Detail: %s/%u (AF: %s)\r\n", ip_str, masklen,
                               bgp_af_str(ctx.afi, ctx.safi));
        g_string_append(resp_buf, "============================================================\r\n");

        if (!inst || !inst->rib)
        {
            g_string_append(resp_buf, "  (no RIB)\r\n");
            return bgp_work_send_chunked_response(msg, resp_buf);
        }

        const bgp_rthead_t *head = bgp_rib_lookup_head(inst->rib, &nlri);
        if (!head)
        {
            g_string_append_printf(resp_buf, "  Route %s/%u not found.\r\n", ip_str, masklen);
            return bgp_work_send_chunked_response(msg, resp_buf);
        }

        bgp_show_route_detail(resp_buf, head);
        return bgp_work_send_chunked_response(msg, resp_buf);
    }

    g_string_append_printf(resp_buf, "\r\nBGP Routes (AF: %s)\r\n", bgp_af_str(ctx.afi, ctx.safi));
    g_string_append(resp_buf, "============================================================\r\n");

    if (!inst || !inst->rib || bgp_rib_route_count(inst->rib) == 0)
    {
        g_string_append(resp_buf, "  (no routes)\r\n\r\n");
        return bgp_work_send_chunked_response(msg, resp_buf);
    }

    g_string_append_printf(resp_buf, "  Networks: %-6u  Paths: %u\r\n\r\n", bgp_rib_head_count(inst->rib),
                           bgp_rib_route_count(inst->rib));
    g_string_append(resp_buf, "  Markers : '>'=BEST, 'v'=VALID\r\n\r\n");

    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", BGP_RT_COL_NET, "Network", BGP_RT_COL_NH,
                           "NextHop", BGP_RT_COL_LP, "LocPref", BGP_RT_COL_MED, "MED", BGP_RT_COL_ORIG, "Origin",
                           "AS-Path");
    g_string_append_printf(resp_buf, "%-*s %-*s %-*s %-*s %-*s %s\r\n", BGP_RT_COL_NET, "------------------------",
                           BGP_RT_COL_NH, "--------------------", BGP_RT_COL_LP, "--------", BGP_RT_COL_MED, "--------",
                           BGP_RT_COL_ORIG, "------------", "--------");

    bgp_show_route_ctx_t show_ctx;
    show_ctx.buf = resp_buf;
    show_ctx.listed_heads = 0;
    show_ctx.listed_routes = 0;

    g_tree_foreach(inst->rib->head_tree, bgp_show_route_head_cb, &show_ctx);

    g_string_append_printf(resp_buf, "\r\nTotal: %u networks, %u paths\r\n\r\n", show_ctx.listed_heads,
                           show_ctx.listed_routes);

    return bgp_work_send_chunked_response(msg, resp_buf);
}

/**
 * @brief 处理 show bgp neighbor af ipv4-unicast|ipv6-unicast [<ip>] 命令
 *
 * group_id=9, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=ip-address
 */
static int handle_bgp_show_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char ip_buf[64] = {0};
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    gboolean has_af = FALSE;

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
                    snprintf(ip_buf, sizeof(ip_buf), "%s", s);
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
        bgp_show_send_cli_response(
            msg, "BGP Error: Missing address-family. Use 'af ipv4-unicast' or 'af ipv6-unicast'.\r\n");
        return ERRCODE_FAIL;
    }

    if (!g_bgp_work_local->protocol)
    {
        bgp_show_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, ctx.vrf_id);
    if (!vrf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: VRF not found.\r\n");
        return ERRCODE_FAIL;
    }

    if (ip_buf[0] == '\0')
    {
        bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));

        GString *resp_buf = g_string_new("");
        if (!resp_buf)
        {
            bgp_show_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
            return ERRCODE_FAIL;
        }

        g_string_append_printf(resp_buf, "\r\nBGP Neighbors (AF: %s)\r\n", bgp_af_str(ctx.afi, ctx.safi));
        g_string_append(resp_buf, "============================================================\r\n");

        if (!inst || g_hash_table_size(inst->peer_hash) == 0)
        {
            g_string_append(resp_buf, "  (no neighbors configured)\r\n");
        }
        else
        {
            g_string_append_printf(resp_buf, "  %-17s%-11s%-17s%s\r\n", "Neighbor", "Remote-AS", "Router-ID", "State");
            g_string_append_printf(resp_buf, "  %-17s%-11s%-17s%s\r\n", "---------------", "---------",
                                   "---------------", "-----------");

            GHashTableIter iter;
            gpointer key, val;
            g_hash_table_iter_init(&iter, inst->peer_hash);
            while (g_hash_table_iter_next(&iter, &key, &val))
            {
                bgp_peer_t *peer = (bgp_peer_t *)val;
                bgp_session_t *psess = bgp_vrf_find_session(vrf, &peer->addr);

                char nbr_ip[64];
                net_addr_to_str(&peer->addr, nbr_ip, sizeof(nbr_ip));

                char _psess_rid_str[16];
                if (psess && psess->remote_id)
                {
                    struct in_addr _tmp;
                    _tmp.s_addr = htonl(psess->remote_id);
                    inet_ntop(AF_INET, &_tmp, _psess_rid_str, sizeof(_psess_rid_str));
                }
                else
                {
                    snprintf(_psess_rid_str, sizeof(_psess_rid_str), "0.0.0.0");
                }
                const char *rid = _psess_rid_str;
                uint32_t ras = psess ? psess->remote_as : 0;
                const char *state = psess ? sess_state_str(psess) : "Idle";

                g_string_append_printf(resp_buf, "  %-17s%-11u%-17s%s\r\n", nbr_ip, ras, rid, state);
            }
        }

        g_string_append(resp_buf, "\r\n");
        return bgp_work_send_chunked_response(msg, resp_buf);
    }

    net_addr_t ip_addr;
    if (net_addr_from_str(ip_buf, &ip_addr) != 0)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, &ip_addr);
    if (!sess)
    {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "BGP Error: Neighbor %s not found.\r\n", ip_buf);
        bgp_show_send_cli_response(msg, tmp);
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));
    gboolean af_enabled = (inst && g_hash_table_lookup(inst->peer_hash, &ip_addr));

    GString *resp_buf = g_string_new("");
    if (!resp_buf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    g_string_append_printf(resp_buf, "\r\nBGP Neighbor: %s\r\n", ip_buf);
    g_string_append(resp_buf, "==========================================\r\n");
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "Remote AS", sess->remote_as);
    char _sess_rid_str[32];
    if (sess->remote_id)
    {
        struct in_addr _tmp;
        _tmp.s_addr = htonl(sess->remote_id);
        inet_ntop(AF_INET, &_tmp, _sess_rid_str, sizeof(_sess_rid_str));
    }
    else
    {
        snprintf(_sess_rid_str, sizeof(_sess_rid_str), "(not established)");
    }
    g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Remote Router-ID", _sess_rid_str);
    g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Session State", sess_state_str(sess));
    char pri_last_err[128];
    char sec_last_err[128];
    bgp_conn_last_error_to_str(sess->pri_conn, sess->pri_last_socket_error, pri_last_err, sizeof(pri_last_err));
    bgp_conn_last_error_to_str(sess->sec_conn, sess->sec_last_socket_error, sec_last_err, sizeof(sec_last_err));
    g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Primary Last Error", pri_last_err);
    g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Secondary Last Error", sec_last_err);

    char _est_ts[32];
    bgp_fmt_time_usec(sess->established_at_usec, _est_ts, sizeof(_est_ts));
    g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Established At", _est_ts);

    g_string_append(resp_buf, "\r\n  Capabilities:\r\n");
    g_string_append_printf(resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "Feature", "Local", "Remote", "Negotiated");
    g_string_append_printf(resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "---------------", "---------", "---------",
                           "---------");
    g_string_append_printf(resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "AS4", cap_yn(sess->flags, BGP_SESS_CAP_AS4),
                           cap_yn(sess->remote_caps, BGP_SESS_CAP_AS4),
                           cap_yn(sess->negotiated_caps, BGP_SESS_CAP_AS4));
    g_string_append_printf(resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "Route-Refresh",
                           cap_yn(sess->flags, BGP_SESS_CAP_ROUTE_REFRESH),
                           cap_yn(sess->remote_caps, BGP_SESS_CAP_ROUTE_REFRESH),
                           cap_yn(sess->negotiated_caps, BGP_SESS_CAP_ROUTE_REFRESH));

    g_string_append(resp_buf, "\r\n  Hold Time:\r\n");
    uint16_t local_hold = (sess->vrf && sess->vrf->hold_time > 0) ? sess->vrf->hold_time : BGP_HOLD_TIME;
    g_string_append_printf(resp_buf, "  %-24s: %u s\r\n", "Local (sent)", local_hold);
    if (sess->remote_hold)
    {
        g_string_append_printf(resp_buf, "  %-24s: %u s\r\n", "Remote (received)", sess->remote_hold);
    }
    else
    {
        g_string_append_printf(resp_buf, "  %-24s: %s\r\n", "Remote (received)", "(not established)");
    }
    g_string_append_printf(resp_buf, "  %-24s: %u s\r\n", "Negotiated", sess->negotiated_hold);

    g_string_append(resp_buf, "\r\n  Negotiated Address Families:\r\n");
    if (sess->negotiated_afs && sess->negotiated_afs->len > 0)
    {
        for (guint _af_i = 0; _af_i < sess->negotiated_afs->len; _af_i++)
        {
            guint32 packed = g_array_index(sess->negotiated_afs, guint32, _af_i);
            uint16_t _afi = (uint16_t)(packed >> 16);
            uint8_t _safi = (uint8_t)(packed & 0xFF);
            g_string_append_printf(resp_buf, "    afi=%u safi=%u\r\n", _afi, _safi);
        }
    }
    else
    {
        g_string_append(resp_buf, "    (none)\r\n");
    }

    char af_label[64];
    snprintf(af_label, sizeof(af_label), "AF %s", bgp_af_str(ctx.afi, ctx.safi));
    g_string_append_printf(resp_buf, "\r\n  %-24s: %s\r\n", af_label, af_enabled ? "Enabled" : "Disabled");
    g_string_append(resp_buf, "\r\n");

    return bgp_work_send_chunked_response(msg, resp_buf);
}

// ============================================================================
// show bgp bmp
// ============================================================================

/** BMP 连接状态可读字符串 */
static const char *bmp_conn_state_str(bgp_bmp_conn_state_t st)
{
    switch (st)
    {
        case BGP_BMP_CONN_IDLE:
            return "Idle";
        case BGP_BMP_CONN_CONNECTING:
            return "Connecting";
        case BGP_BMP_CONN_UP:
            return "Up";
        case BGP_BMP_CONN_WAIT:
            return "Wait-Retry";
        default:
            return "Unknown";
    }
}

/** 单个 BMP 实例的详细信息（show bgp bmp instance <name>） */
static void bmp_show_instance_detail(GString *buf, const bgp_bmp_instance_t *inst)
{
    char collector_str[64] = "-";
    if (inst->collector_port > 0)
    {
        net_addr_to_str(&inst->collector_addr, collector_str, sizeof(collector_str));
    }

    g_string_append_printf(buf, "\r\nBMP Instance: %s\r\n", inst->name);
    g_string_append(buf, "------------------------------------------------------------\r\n");

    /* 配置 */
    g_string_append(buf, "  Configuration:\r\n");
    if (inst->collector_port > 0)
    {
        g_string_append_printf(buf, "    Collector:          %s port %u\r\n", collector_str, inst->collector_port);
    }
    else
    {
        g_string_append(buf, "    Collector:          (not configured)\r\n");
    }
    g_string_append_printf(buf, "    Stats interval:     %u sec%s\r\n", inst->stats_interval,
                           inst->stats_interval == 0 ? " (disabled)" : "");
    g_string_append_printf(buf, "    Reconnect interval: %u sec\r\n", inst->reconnect_interval);
    if (inst->monitor_all)
    {
        g_string_append(buf, "    Monitor:            all neighbors\r\n");
    }
    else if (inst->monitor_peers && g_hash_table_size(inst->monitor_peers) > 0)
    {
        g_string_append(buf, "    Monitor:            specific neighbors\r\n");
        GHashTableIter piter;
        gpointer pkey, pval;
        g_hash_table_iter_init(&piter, inst->monitor_peers);
        while (g_hash_table_iter_next(&piter, &pkey, &pval))
        {
            g_string_append_printf(buf, "      - %s\r\n", (const char *)pkey);
        }
    }
    else
    {
        g_string_append(buf, "    Monitor:            none\r\n");
    }

    /* 连接状态 */
    g_string_append(buf, "  Connection:\r\n");
    g_string_append_printf(buf, "    State:              %s\r\n", bmp_conn_state_str(inst->conn_state));

    if (inst->connected_at_usec > 0)
    {
        char time_buf[32];
        bgp_fmt_time_usec(inst->connected_at_usec, time_buf, sizeof(time_buf));
        g_string_append_printf(buf, "    Connected since:    %s\r\n", time_buf);
    }

    /* 统计 */
    g_string_append(buf, "  Statistics:\r\n");
    g_string_append_printf(buf, "    Initiation sent:    %u\r\n", inst->initiation_sent);
    g_string_append_printf(buf, "    Peer Up sent:       %u\r\n", inst->peer_up_sent);
    g_string_append_printf(buf, "    Peer Down sent:     %u\r\n", inst->peer_down_sent);
    g_string_append_printf(buf, "    Route Monitor sent: %u\r\n", inst->route_monitor_sent);
    g_string_append_printf(buf, "    Stats Report sent:  %u\r\n", inst->stats_report_sent);
}

static int handle_bgp_show_bmp(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char inst_name[BGP_BMP_INST_NAME_MAX] = {0};

    /* 解析可选的 instance <name> 参数 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(inst_name, sizeof(inst_name), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    GHashTable *bmp_ht = g_bgp_work_local->bmp_instances;

    /* 指定实例名 → 显示单实例详情 */
    if (inst_name[0] != '\0')
    {
        if (!bmp_ht)
        {
            bgp_show_send_cli_response(msg, "BMP Error: No BMP instances configured.\r\n");
            return ERRCODE_FAIL;
        }
        bgp_bmp_instance_t *inst = g_hash_table_lookup(bmp_ht, inst_name);
        if (!inst)
        {
            bgp_show_send_cli_response(msg, "BMP Error: Instance not found.\r\n");
            return ERRCODE_FAIL;
        }

        GString *resp_buf = g_string_new("");
        bmp_show_instance_detail(resp_buf, inst);
        return bgp_work_send_chunked_response(msg, resp_buf);
    }

    /* 无实例名 → 显示所有实例摘要 */
    GString *resp_buf = g_string_new("");
    g_string_append(resp_buf, "\r\nBMP Instances\r\n");
    g_string_append(resp_buf, "============================================================\r\n");

    if (!bmp_ht || g_hash_table_size(bmp_ht) == 0)
    {
        g_string_append(resp_buf, "  (no BMP instances configured)\r\n");
        return bgp_work_send_chunked_response(msg, resp_buf);
    }

    g_string_append_printf(resp_buf, "  %-20s%-18s%-8s%-14s%s\r\n", "Instance", "Collector", "Port", "State",
                           "PeerUp/Down");
    g_string_append_printf(resp_buf, "  %-20s%-18s%-8s%-14s%s\r\n", "-------------------", "-----------------",
                           "------", "------------", "-----------");

    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, bmp_ht);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        bgp_bmp_instance_t *inst = (bgp_bmp_instance_t *)val;

        char collector_str[64] = "-";
        char port_str[8] = "-";
        if (inst->collector_port > 0)
        {
            net_addr_to_str(&inst->collector_addr, collector_str, sizeof(collector_str));
            snprintf(port_str, sizeof(port_str), "%u", inst->collector_port);
        }

        char updown[32];
        snprintf(updown, sizeof(updown), "%u/%u", inst->peer_up_sent, inst->peer_down_sent);

        g_string_append_printf(resp_buf, "  %-20s%-18s%-8s%-14s%s\r\n", inst->name, collector_str, port_str,
                               bmp_conn_state_str(inst->conn_state), updown);
    }

    return bgp_work_send_chunked_response(msg, resp_buf);
}

int bgp_work_handle_show_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    /* 新 show 命令到来时清理上次可能残留的分片状态 */
    cli_chunk_stream_reset(&g_bgp_show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("BGP: show 命令 payload 解析失败");
        bgp_show_send_cli_response(msg, "BGP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("BGP: show 命令 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case BGP_CLI_GROUP_ID_SHOW_NEIGHBOR:
            result = handle_bgp_show_neighbor(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_SHOW_ROUTE:
            result = handle_bgp_show_route(msg, &parser);
            break;
        default:
            LOG_WARN("BGP: 未知 show 命令 group_id=%u", parser.group_id);
            bgp_show_send_cli_response(msg, "BGP Error: Unknown show command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
