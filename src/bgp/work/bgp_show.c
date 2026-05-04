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

#include "bgp_attr_intern.h"
#include "bgp_cli.h"
#include "bgp_conn.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_update_group.h"
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
    if (afi == BGP_AFI_IPV4 && safi == BGP_SAFI_QP)
    {
        return "ipv4-qp";
    }
    if (afi == BGP_AFI_IPV6 && safi == BGP_SAFI_QP)
    {
        return "ipv6-qp";
    }
    if (afi == BGP_AFI_IPV4 && safi == BGP_SAFI_LABELED)
    {
        return "ipv4-labeled";
    }
    if (afi == BGP_AFI_IPV6 && safi == BGP_SAFI_LABELED)
    {
        return "ipv6-labeled";
    }
    return "unknown";
}

static gboolean bgp_show_af_list_contains(const GArray *afs, guint32 af_key)
{
    if (!afs)
    {
        return FALSE;
    }

    for (guint i = 0; i < afs->len; i++)
    {
        if (g_array_index(afs, guint32, i) == af_key)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void bgp_show_append_af_list(GString *buf, const GArray *afs)
{
    if (!buf)
    {
        return;
    }

    if (!afs || afs->len == 0)
    {
        g_string_append(buf, "    none\r\n");
        return;
    }

    for (guint i = 0; i < afs->len; i++)
    {
        guint32 packed = g_array_index(afs, guint32, i);
        bgp_afi_t afi = (bgp_afi_t)(uint16_t)(packed >> 16);
        bgp_safi_t safi = (bgp_safi_t)(uint8_t)(packed & 0xFF);
        g_string_append_printf(buf, "    afi=%u safi=%u (%s)\r\n", (unsigned)afi, (unsigned)safi,
                               bgp_af_str(afi, safi));
    }
}

static void bgp_show_append_negotiated_af_list(GString *buf, const bgp_session_t *sess)
{
    gboolean any = FALSE;

    if (!buf)
    {
        return;
    }

    if (!sess || !sess->local_afs || !sess->remote_afs)
    {
        g_string_append(buf, "    none\r\n");
        return;
    }

    for (guint i = 0; i < sess->local_afs->len; i++)
    {
        guint32 packed = g_array_index(sess->local_afs, guint32, i);
        if (!bgp_show_af_list_contains(sess->remote_afs, packed))
        {
            continue;
        }
        bgp_afi_t afi = (bgp_afi_t)(uint16_t)(packed >> 16);
        bgp_safi_t safi = (bgp_safi_t)(uint8_t)(packed & 0xFF);
        g_string_append_printf(buf, "    afi=%u safi=%u (%s)\r\n", (unsigned)afi, (unsigned)safi,
                               bgp_af_str(afi, safi));
        any = TRUE;
    }

    if (!any)
    {
        g_string_append(buf, "    none\r\n");
    }
}

/** 返回 session 当前状态字符串 */
static const char *sess_state_str(const bgp_session_t *sess)
{
    if (!sess)
    {
        return "Idle";
    }
    switch (sess->fsm_state)
    {
        case BGP_FSM_STATE_IDLE:
            return "Idle";
        case BGP_FSM_STATE_CONNECT:
            return "Connect";
        case BGP_FSM_STATE_ACTIVE:
            return "Active";
        case BGP_FSM_STATE_OPEN_SENT:
            return "OpenSent";
        case BGP_FSM_STATE_OPEN_CONFIRM:
            return "OpenConfirm";
        case BGP_FSM_STATE_ESTABLISHED:
            return "Established";
        default:
            return "Unknown";
    }
}

/**
 * @brief 返回 AF 视角下 peer 状态字符串
 *
 * 直接读取 peer->state（在 catchup_session / reset_negotiated 中维护）：
 *   ESTABLISHED       → "Established"
 *   NOT_NEGOTIATED    → "NoNegotiated"（session ESTABLISHED 但对端未协商本 AF）
 *   IDLE（session 未 ESTABLISHED） → 回落到 session FSM 状态串
 */
static const char *peer_af_state_str(const bgp_peer_t *peer, const bgp_session_t *sess)
{
    if (peer)
    {
        if (peer->state == BGP_PEER_STATE_ESTABLISHED)
        {
            return "Established";
        }
        if (peer->state == BGP_PEER_STATE_NOT_NEGOTIATED)
        {
            return "NoNegotiated";
        }
    }
    return sess_state_str(sess);
}

/** 返回能力位对应的可读字符串 */
static const char *cap_yn(uint32_t caps, uint32_t bit)
{
    return BIT_TEST(caps, bit) ? "Yes" : "No";
}

static const char *bgp_sess_type_str(bgp_sess_type_t t)
{
    switch (t)
    {
        case BGP_SESS_TYPE_IBGP:
            return "iBGP";
        case BGP_SESS_TYPE_EBGP:
            return "eBGP";
        default:
            return "Unknown";
    }
}

static const char *bgp_nh_rule_str(bgp_nh_rule_t r)
{
    switch (r)
    {
        case BGP_NH_RULE_LOCAL:
            return "local";
        case BGP_NH_RULE_PASS:
            return "pass";
        case BGP_NH_RULE_CONFIG:
            return "config";
        default:
            return "unknown";
    }
}

static void bgp_router_id_to_str(uint32_t rid_host_order, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }

    if (rid_host_order == 0)
    {
        snprintf(buf, sz, "0.0.0.0");
        return;
    }

    struct in_addr tmp;
    tmp.s_addr = htonl(rid_host_order);
    inet_ntop(AF_INET, &tmp, buf, (socklen_t)sz);
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

/* 路由表固定列宽（Network 列按实际前缀长度动态扩展） */
#define BGP_RT_COL_NET_MIN 24
#define BGP_RT_MARKER_COL 3
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
    guint net_col_width;
    uint32_t listed_heads;
    uint32_t listed_routes;
} bgp_show_route_ctx_t;

typedef struct bgp_show_route_width_ctx
{
    guint net_col_width;
} bgp_show_route_width_ctx_t;

/**
 * @brief 将单条路径的各字段格式化到 lp/med/as_path 缓冲区
 */
static void bgp_route_fmt_fields(const bgp_route_node_t *route, char *lp, size_t lp_sz, char *med, size_t med_sz,
                                 char *as_path, size_t as_sz)
{
    if (BGP_ROUTE_ATTR(route)->has_local_pref)
    {
        snprintf(lp, lp_sz, "%u", BGP_ROUTE_ATTR(route)->local_pref);
    }
    else
    {
        snprintf(lp, lp_sz, "-");
    }
    if (BGP_ROUTE_ATTR(route)->has_med)
    {
        snprintf(med, med_sz, "%u", BGP_ROUTE_ATTR(route)->med);
    }
    else
    {
        snprintf(med, med_sz, "-");
    }
    if (BGP_ROUTE_ATTR(route)->as_path[0] != '\0')
    {
        snprintf(as_path, as_sz, "%.*s", (int)(as_sz - 1), BGP_ROUTE_ATTR(route)->as_path);
    }
    else
    {
        snprintf(as_path, as_sz, "-");
    }
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
        const char *label_name = bgp_route_label_name(route);
        if (label_name)
        {
            g_string_append_printf(buf, "    %-10s: %u\r\n", label_name, route->label);
        }
        g_string_append_printf(buf, "    Attr-ID  : %u (refcnt=%u)\r\n", route->attr ? route->attr->attr_id : 0,
                               route->attr ? route->attr->refcnt : 0);
        g_string_append_printf(buf, "    NextHop  : %s\r\n", nh);
        g_string_append_printf(buf, "    LocPref  : %s\r\n", lp);
        g_string_append_printf(buf, "    MED      : %s\r\n", med);
        g_string_append_printf(buf, "    Origin   : %s\r\n", bgp_origin_str(BGP_ROUTE_ATTR(route)->origin));
        g_string_append_printf(buf, "    Valid    : %s\r\n",
                               BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID) ? "Yes" : "No");
        g_string_append_printf(buf, "    IterState: %s\r\n", iter_state_str);
        g_string_append_printf(buf, "    Iter-NH  : %s\r\n", iter_nh);
        g_string_append_printf(buf, "    Out-If   : %s\r\n", out_if);
        g_string_append_printf(buf, "    Tunnel-ID: %u\r\n", route->tunnel_id);
        g_string_append_printf(buf, "    Flags    : 0x%08X (%s)\r\n", route->flags, flags_str);
        g_string_append_printf(buf, "    AS-Path  : %s\r\n", as_path);

        if (BGP_ROUTE_ATTR(route)->communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Community: %s\r\n", BGP_ROUTE_ATTR(route)->communities);
        }
        if (BGP_ROUTE_ATTR(route)->ext_communities[0] != '\0')
        {
            g_string_append_printf(buf, "    Ext-Comm : %s\r\n", BGP_ROUTE_ATTR(route)->ext_communities);
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
        g_string_append_printf(buf, "    Added    : %s\r\n", ts_added);
        g_string_append_printf(buf, "    Updated  : %s\r\n\r\n", ts_updated);
    }
}

static uint8_t bgp_show_qp_dqpn_bytes(uint32_t dqpn)
{
    if (dqpn <= 0xFFu)
    {
        return 1;
    }
    if (dqpn <= 0xFFFFu)
    {
        return 2;
    }
    return 3;
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
    nlri->qp.dqpn_len = bgp_show_qp_dqpn_bytes(nlri->qp.dqpn);
    nlri->qp.prefix.addr = addr;
    nlri->qp.prefix.prefix_len = (uint8_t)mask_ul;
    return TRUE;
}

/**
 * @brief 处理 show bgp route af ipv4-unicast|ipv6-unicast [<ip> <masklen>] / ipv4-qp|ipv6-qp [<qp-key>] 命令
 *
 * group_id=10, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=ip-address, 4=masklen, 7=qp-route-key
 * 不带查询参数时显示路由表；unicast 详情使用 ip/masklen，QP 详情使用 dqpn=<n>,ip=<pfx>/<mask>
 */
static int handle_bgp_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    gboolean has_af = FALSE;
    char ip_str[64] = {0};
    char qp_query[256] = {0};
    uint32_t masklen = 0;
    gboolean has_ip = FALSE;
    gboolean has_masklen = FALSE;
    gboolean has_qp_query = FALSE;

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
            case 9:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_LABELED;
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

    if ((ctx.safi != BGP_SAFI_QP && has_ip && has_masklen) || (ctx.safi == BGP_SAFI_QP && has_qp_query))
    {
        bgp_nlri_entry_t nlri;
        memset(&nlri, 0, sizeof(nlri));
        char nlri_str[BGP_NLRI_KEY_MAX];

        if (ctx.safi == BGP_SAFI_QP)
        {
            char err[160];
            if (!bgp_show_parse_qp_query(qp_query, ctx.afi, &nlri, err, sizeof(err)))
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

    /* 遍历所有 RD entry 的 RIB（VPN AF 下可能多张；非 VPN AF 仅公网一张） */
    {
        GHashTableIter rd_iter;
        gpointer rd_key, rd_val;
        g_hash_table_iter_init(&rd_iter, inst->rd_entries);
        while (g_hash_table_iter_next(&rd_iter, &rd_key, &rd_val))
        {
            (void)rd_key;
            bgp_rd_entry_t *e = (bgp_rd_entry_t *)rd_val;
            if (!e || !e->rib || !e->rib->head_tree)
            {
                continue;
            }
            g_tree_foreach(e->rib->head_tree, bgp_show_route_head_cb, &show_ctx);
        }
    }

    g_string_append_printf(resp_buf, "\r\nTotal: %u networks, %u paths\r\n\r\n", show_ctx.listed_heads,
                           show_ctx.listed_routes);

    return bgp_work_send_chunked_response(msg, resp_buf);
}

/**
 * @brief 处理 show bgp neighbor af ipv4-unicast|ipv6-unicast|ipv4-qp|ipv6-qp [<ip>] 命令
 *
 * group_id=9, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=ip-address, 4=ipv4-qp, 5=ipv6-qp
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
            case 4:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_QP;
                has_af = TRUE;
                break;
            case 5:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_QP;
                has_af = TRUE;
                break;
            case 6:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_LABELED;
                has_af = TRUE;
                break;
            case 7:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_LABELED;
                has_af = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_af)
    {
        bgp_show_send_cli_response(
            msg, "BGP Error: Missing address-family. Use 'af ipv4-unicast', 'af ipv6-unicast', 'af ipv4-qp', or 'af "
                 "ipv6-qp'.\r\n");
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
                const char *state = peer_af_state_str(peer, psess);

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
    g_string_append_printf(resp_buf, "  %-24s: %d\r\n", "Primary Connection FD",
                           (sess->pri_conn) ? sess->pri_conn->fd : -1);
    g_string_append_printf(resp_buf, "  %-24s: %d\r\n", "Secondary Connection FD",
                           (sess->sec_conn) ? sess->sec_conn->fd : -1);

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
    g_string_append_printf(
        resp_buf, "  %-16s  %-10s  %-10s  %-10s\r\n", "Extended-Nexthop", cap_yn(sess->flags, BGP_SESS_CAP_EXT_NEXTHOP),
        cap_yn(sess->remote_caps, BGP_SESS_CAP_EXT_NEXTHOP), cap_yn(sess->negotiated_caps, BGP_SESS_CAP_EXT_NEXTHOP));

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

    GString *local_af_list = g_string_new("");
    GString *remote_af_list = g_string_new("");
    GString *negotiated_af_list = g_string_new("");
    if (!local_af_list || !remote_af_list || !negotiated_af_list)
    {
        g_string_free(resp_buf, TRUE);
        if (local_af_list)
        {
            g_string_free(local_af_list, TRUE);
        }
        if (remote_af_list)
        {
            g_string_free(remote_af_list, TRUE);
        }
        if (negotiated_af_list)
        {
            g_string_free(negotiated_af_list, TRUE);
        }
        bgp_show_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_show_append_af_list(local_af_list, sess->local_afs);
    bgp_show_append_af_list(remote_af_list, sess->remote_afs);
    bgp_show_append_negotiated_af_list(negotiated_af_list, sess);

    g_string_append_printf(resp_buf, "\r\n  %-24s: \r\n%s", "Local Address Families", local_af_list->str);
    g_string_append_printf(resp_buf, "  %-24s: \r\n%s", "Remote Address Families", remote_af_list->str);
    g_string_append_printf(resp_buf, "  %-24s: \r\n%s", "Negotiated Address Families", negotiated_af_list->str);

    g_string_free(local_af_list, TRUE);
    g_string_free(remote_af_list, TRUE);
    g_string_free(negotiated_af_list, TRUE);

    g_string_append(resp_buf, "\r\n  Received Messages:\r\n");
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "OPEN", sess->rx_msg_stats.open);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "UPDATE", sess->rx_msg_stats.update);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "NOTIFICATION", sess->rx_msg_stats.notification);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "KEEPALIVE", sess->rx_msg_stats.keepalive);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "Unknown", sess->rx_msg_stats.unknown);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "Total", sess->rx_msg_stats.total);

    g_string_append(resp_buf, "\r\n  Sent Messages:\r\n");
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "OPEN", sess->tx_msg_stats.open);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "UPDATE", sess->tx_msg_stats.update);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "NOTIFICATION", sess->tx_msg_stats.notification);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "KEEPALIVE", sess->tx_msg_stats.keepalive);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "Total", sess->tx_msg_stats.total);

    g_string_append(resp_buf, "\r\n");

    return bgp_work_send_chunked_response(msg, resp_buf);
}

typedef struct bgp_show_ug_stats
{
    uint32_t subgroup_count;
    uint32_t peer_count;
    uint32_t aro_count;
    uint32_t pending_ann;
    uint32_t pending_wd;
    uint32_t total_ann;
    uint32_t total_wd;
} bgp_show_ug_stats_t;

static void bgp_show_ug_collect_stats(const bgp_update_group_t *ug, bgp_show_ug_stats_t *st)
{
    if (!st)
    {
        return;
    }
    memset(st, 0, sizeof(*st));
    if (!ug)
    {
        return;
    }

    for (const GList *sl = ug->subgroups; sl; sl = sl->next)
    {
        const bgp_nh_subgroup_t *sg = (const bgp_nh_subgroup_t *)sl->data;
        if (!sg)
        {
            continue;
        }

        st->subgroup_count++;
        st->peer_count += sg->peer_count;
        st->aro_count += bgp_adj_rib_out_count(sg->adj_rib_out);
        st->pending_ann += sg->announce_queue ? (uint32_t)g_queue_get_length(sg->announce_queue) : 0u;
        st->pending_wd += sg->withdraw_queue ? (uint32_t)g_queue_get_length(sg->withdraw_queue) : 0u;
        st->total_ann += sg->announce_count;
        st->total_wd += sg->withdraw_count;
    }
}

static void bgp_show_ug_append_neighbors(GString *buf, const bgp_nh_subgroup_t *sg)
{
    g_string_append_printf(buf, "      %-39s %-10s %-12s %s\r\n", "Neighbor", "Remote-AS", "State", "Router-ID");
    g_string_append_printf(buf, "      %-39s %-10s %-12s %s\r\n", "---------------------------------------",
                           "----------", "------------", "---------------");

    for (const GList *l = sg->peer_list; l; l = l->next)
    {
        const bgp_peer_t *peer = (const bgp_peer_t *)l->data;
        if (!peer || !peer->vrf)
        {
            continue;
        }
        const bgp_session_t *sess = bgp_vrf_find_session(peer->vrf, &peer->addr);
        if (!sess)
        {
            continue;
        }

        char nbr_ip[64];
        char rid[32];
        net_addr_to_str(&sess->neighbor_addr, nbr_ip, sizeof(nbr_ip));
        bgp_router_id_to_str(sess->remote_id, rid, sizeof(rid));

        g_string_append_printf(buf, "      %-39s %-10u %-12s %s\r\n", nbr_ip, sess->remote_as,
                               peer_af_state_str(peer, sess), rid);
    }
}

/** 把 negotiated_caps 位图转成可读字符串，如 "as4,rr,ext-nh" / "-" */
static void bgp_show_ug_caps_str(uint32_t caps, char *out, size_t out_size)
{
    if (out_size == 0)
    {
        return;
    }
    out[0] = '\0';
    const char *parts[3];
    int n = 0;
    if (caps & BGP_SESS_CAP_AS4)
    {
        parts[n++] = "as4";
    }
    if (caps & BGP_SESS_CAP_ROUTE_REFRESH)
    {
        parts[n++] = "rr";
    }
    if (caps & BGP_SESS_CAP_EXT_NEXTHOP)
    {
        parts[n++] = "ext-nh";
    }
    if (n == 0)
    {
        g_strlcpy(out, "-", out_size);
        return;
    }
    for (int i = 0; i < n; i++)
    {
        if (i > 0)
        {
            g_strlcat(out, ",", out_size);
        }
        g_strlcat(out, parts[i], out_size);
    }
}

static void bgp_show_ug_append_detail(GString *buf, const bgp_update_group_t *ug)
{
    bgp_show_ug_stats_t st;
    bgp_show_ug_collect_stats(ug, &st);

    char caps_str[48];
    bgp_show_ug_caps_str(ug->key.negotiated_caps, caps_str, sizeof(caps_str));

    g_string_append_printf(buf, "  Session-Type : %s\r\n", bgp_sess_type_str(ug->key.sess_type));
    g_string_append_printf(buf, "  Remote-AS    : %u\r\n", ug->key.remote_as);
    g_string_append_printf(buf, "  Negotiated   : 0x%08X (%s)\r\n", ug->key.negotiated_caps, caps_str);
    g_string_append_printf(buf, "  Policy-Hash  : 0x%08X\r\n", ug->key.policy_hash);
    g_string_append_printf(buf, "  Peer-Family  : %u\r\n", (unsigned)ug->key.peer_family);
    g_string_append_printf(buf, "  Subgroups    : %u\r\n", st.subgroup_count);
    g_string_append_printf(buf, "  Neighbors    : %u\r\n", st.peer_count);
    g_string_append_printf(buf, "  Adj-RIB-Out  : %u\r\n", st.aro_count);
    g_string_append_printf(buf, "  Pending      : announce=%u withdraw=%u\r\n", st.pending_ann, st.pending_wd);
    g_string_append_printf(buf, "  Counters     : announce=%u withdraw=%u\r\n", st.total_ann, st.total_wd);
    g_string_append(buf, "\r\n");

    uint32_t sg_index = 0;
    for (const GList *sl = ug->subgroups; sl; sl = sl->next)
    {
        const bgp_nh_subgroup_t *sg = (const bgp_nh_subgroup_t *)sl->data;
        if (!sg)
        {
            continue;
        }
        sg_index++;

        char local_addr[64];
        if (sg->key.effective_local_addr.family != 0)
        {
            net_addr_to_str(&sg->key.effective_local_addr, local_addr, sizeof(local_addr));
        }
        else
        {
            snprintf(local_addr, sizeof(local_addr), "-");
        }

        g_string_append_printf(buf, "  Subgroup #%u\r\n", sg_index);
        g_string_append_printf(buf, "    NH Rule       : %s\r\n", bgp_nh_rule_str(sg->key.rule));
        g_string_append_printf(buf, "    Local Address : %s\r\n", local_addr);
        g_string_append_printf(buf, "    Peers         : %u\r\n", sg->peer_count);
        g_string_append_printf(buf, "    Adj-RIB-Out   : %u\r\n", bgp_adj_rib_out_count(sg->adj_rib_out));
        g_string_append_printf(buf, "    Pending Queue : announce=%u withdraw=%u\r\n",
                               sg->announce_queue ? (uint32_t)g_queue_get_length(sg->announce_queue) : 0u,
                               sg->withdraw_queue ? (uint32_t)g_queue_get_length(sg->withdraw_queue) : 0u);
        g_string_append_printf(buf, "    Counters      : announce=%u withdraw=%u\r\n", sg->announce_count,
                               sg->withdraw_count);

        if (!sg->peer_list)
        {
            g_string_append(buf, "    Neighbors     : (none)\r\n\r\n");
            continue;
        }
        g_string_append(buf, "    Neighbors:\r\n");
        bgp_show_ug_append_neighbors(buf, sg);
        g_string_append(buf, "\r\n");
    }
}

/**
 * @brief 处理 show bgp update-group af ipv4-unicast|ipv6-unicast [<group-id>] 命令
 *
 * group_id=15, cfg_id: 1=ipv4-unicast, 2=ipv6-unicast, 3=group-id
 */
static int handle_bgp_show_update_group(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    gboolean has_af = FALSE;
    gboolean has_group_id = FALSE;
    uint32_t group_id = 0;

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
                group_id = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_group_id = TRUE;
                break;
            case 4:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_QP;
                has_af = TRUE;
                break;
            case 5:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_QP;
                has_af = TRUE;
                break;
            case 6:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_LABELED;
                has_af = TRUE;
                break;
            case 7:
                ctx.afi = BGP_AFI_IPV6;
                ctx.safi = BGP_SAFI_LABELED;
                has_af = TRUE;
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
    GString *buf = g_string_sized_new(1024);
    if (!buf)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    if (has_group_id)
    {
        g_string_append_printf(buf, "\r\nBGP Update-Group Detail (AF: %s, Group-ID: %u)\r\n",
                               bgp_af_str(ctx.afi, ctx.safi), group_id);
        g_string_append(buf, "============================================================\r\n");

        const bgp_update_group_t *found = NULL;
        if (inst)
        {
            for (const GList *ul = inst->update_groups; ul; ul = ul->next)
            {
                const bgp_update_group_t *ug = (const bgp_update_group_t *)ul->data;
                if (ug && ug->group_id == group_id)
                {
                    found = ug;
                    break;
                }
            }
        }

        if (!found)
        {
            g_string_append_printf(buf, "  Update-group %u not found.\r\n\r\n", group_id);
            return bgp_work_send_chunked_response(msg, buf);
        }

        bgp_show_ug_append_detail(buf, found);
        return bgp_work_send_chunked_response(msg, buf);
    }

    g_string_append_printf(buf, "\r\nBGP Update-Groups (AF: %s)\r\n", bgp_af_str(ctx.afi, ctx.safi));
    g_string_append(buf, "============================================================\r\n");

    if (!inst || !inst->update_groups)
    {
        g_string_append(buf, "  (no update-groups)\r\n\r\n");
        return bgp_work_send_chunked_response(msg, buf);
    }

    g_string_append_printf(buf, "  %-8s %-10s %-10s %-10s %-10s %-10s %-12s %s\r\n", "Group-ID", "SessType",
                           "Subgroups", "Neighbors", "AdjRibOut", "Pend-A", "Pend-W", "Policy-Hash");
    g_string_append_printf(buf, "  %-8s %-10s %-10s %-10s %-10s %-10s %-12s %s\r\n", "--------", "--------",
                           "----------", "----------", "----------", "----------", "------------", "-----------");

    uint32_t listed_groups = 0;
    uint32_t total_subgroups = 0;
    uint32_t total_neighbors = 0;
    uint32_t total_adj_rib_out = 0;

    for (const GList *ul = inst->update_groups; ul; ul = ul->next)
    {
        const bgp_update_group_t *ug = (const bgp_update_group_t *)ul->data;
        if (!ug)
        {
            continue;
        }
        bgp_show_ug_stats_t st;
        bgp_show_ug_collect_stats(ug, &st);

        g_string_append_printf(buf, "  %-8u %-10s %-10u %-10u %-10u %-10u %-12u 0x%08X\r\n", ug->group_id,
                               bgp_sess_type_str(ug->key.sess_type), st.subgroup_count, st.peer_count, st.aro_count,
                               st.pending_ann, st.pending_wd, ug->key.policy_hash);

        listed_groups++;
        total_subgroups += st.subgroup_count;
        total_neighbors += st.peer_count;
        total_adj_rib_out += st.aro_count;
    }

    g_string_append_printf(buf, "\r\nTotal: %u groups, %u subgroups, %u neighbors, %u adj-rib-out entries\r\n\r\n",
                           listed_groups, total_subgroups, total_neighbors, total_adj_rib_out);
    return bgp_work_send_chunked_response(msg, buf);
}

/** 把 source_flags 位图转成可读字符串，如 "loc-rib" / "rib-out" / "loc-rib,rib-out" / "-" */
static void bgp_show_attr_source_str(uint32_t flags, char *buf, size_t buf_size)
{
    if (buf_size == 0)
    {
        return;
    }
    buf[0] = '\0';
    const char *parts[2];
    int n = 0;
    if (flags & BGP_ATTR_SRC_LOC_RIB)
    {
        parts[n++] = "loc-rib";
    }
    if (flags & BGP_ATTR_SRC_RIB_OUT)
    {
        parts[n++] = "rib-out";
    }
    if (n == 0)
    {
        g_strlcpy(buf, "-", buf_size);
        return;
    }
    for (int i = 0; i < n; i++)
    {
        if (i > 0)
        {
            g_strlcat(buf, ",", buf_size);
        }
        g_strlcat(buf, parts[i], buf_size);
    }
}

/** intern 表遍历回调：把单条 attr 追加到摘要表格中 */
static void bgp_show_attr_summary_cb(const bgp_attr_ref_t *ref, gpointer user_data)
{
    GString *b = (GString *)user_data;
    if (!ref || !b)
    {
        return;
    }
    char src_str[32];
    bgp_show_attr_source_str(ref->source_flags, src_str, sizeof(src_str));
    g_string_append_printf(b, "  %-8u %-6u %-16s %s\r\n", ref->attr_id, ref->refcnt, src_str,
                           ref->attr.as_path[0] ? ref->attr.as_path : "-");
}

/**
 * @brief 格式化单条属性详情到 GString
 */
static void bgp_show_attr_detail(GString *buf, const bgp_attr_ref_t *ref)
{
    const bgp_attr_t *a = &ref->attr;
    char src_str[32];
    bgp_show_attr_source_str(ref->source_flags, src_str, sizeof(src_str));
    g_string_append_printf(buf, "  Attr-ID    : %u\r\n", ref->attr_id);
    g_string_append_printf(buf, "  Source     : %s\r\n", src_str);
    g_string_append_printf(buf, "  RefCount   : %u\r\n", ref->refcnt);
    g_string_append_printf(buf, "  Hash       : 0x%08X\r\n", ref->hash);
    g_string_append_printf(buf, "  Origin     : %s\r\n", bgp_origin_str(a->origin));
    g_string_append_printf(buf, "  AS-Path    : %s\r\n", a->as_path[0] ? a->as_path : "-");
    g_string_append_printf(buf, "  LocPref    : %s", a->has_local_pref ? "" : "-");
    if (a->has_local_pref)
    {
        g_string_append_printf(buf, "%u", a->local_pref);
    }
    g_string_append(buf, "\r\n");
    g_string_append_printf(buf, "  MED        : %s", a->has_med ? "" : "-");
    if (a->has_med)
    {
        g_string_append_printf(buf, "%u", a->med);
    }
    g_string_append(buf, "\r\n");
    g_string_append_printf(buf, "  AtomicAggr : %s\r\n", a->atomic_aggregate ? "Yes" : "No");
    if (a->aggregator[0] != '\0')
    {
        g_string_append_printf(buf, "  Aggregator : %s\r\n", a->aggregator);
    }
    if (a->communities[0] != '\0')
    {
        g_string_append_printf(buf, "  Community  : %s\r\n", a->communities);
    }
    if (a->ext_communities[0] != '\0')
    {
        g_string_append_printf(buf, "  Ext-Comm   : %s\r\n", a->ext_communities);
    }
    if (a->large_communities[0] != '\0')
    {
        g_string_append_printf(buf, "  Lrg-Comm   : %s\r\n", a->large_communities);
    }
    if (a->has_originator_id)
    {
        char oid[64];
        net_addr_to_str(&a->originator_id, oid, sizeof(oid));
        g_string_append_printf(buf, "  Originator : %s\r\n", oid);
    }
}

/**
 * @brief 处理 show bgp attr [<attr-id>] 命令
 *
 * group_id=20, cfg_id: 1=attr-id
 * 不带 attr-id 时显示 intern 表摘要，带时显示指定属性详情。
 */
static int handle_bgp_show_attr(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t attr_id = 0;
    gboolean has_id = FALSE;

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
            attr_id = (uint32_t)cli_tlv_entry_get_int(&entry);
            has_id = TRUE;
        }
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_sized_new(512);

    if (!has_id)
    {
        /* 摘要模式：显示 intern 表统计 + 逐条列表 */
        g_string_append_printf(buf, "\r\nBGP Attribute Intern Table\r\n");
        g_string_append_printf(buf, "  Unique attributes: %u\r\n\r\n", bgp_attr_intern_count());
        g_string_append(buf, "  Attr-ID  Refs   Source           AS-Path\r\n");
        g_string_append(buf, "  -------- ------ ---------------- ----------------\r\n");
        bgp_attr_intern_foreach(bgp_show_attr_summary_cb, buf);
        g_string_append(buf, "\r\n");
        return bgp_work_send_chunked_response(msg, buf);
    }

    /* 详情模式：按 ID 查找并输出 */
    const bgp_attr_ref_t *ref = bgp_attr_find_by_id(attr_id);
    if (!ref)
    {
        g_string_append_printf(buf, "\r\nBGP Error: Attribute ID %u not found.\r\n\r\n", attr_id);
        return bgp_work_send_chunked_response(msg, buf);
    }

    g_string_append_printf(buf, "\r\nBGP Attribute Detail\r\n");
    bgp_show_attr_detail(buf, ref);
    g_string_append(buf, "\r\n");
    return bgp_work_send_chunked_response(msg, buf);
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
        case BGP_CLI_GROUP_ID_SHOW_ATTR:
            result = handle_bgp_show_attr(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_SHOW_UG:
            result = handle_bgp_show_update_group(msg, &parser);
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
