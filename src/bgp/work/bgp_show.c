/**
 * @file   bgp_show.c
 * @brief  BGP show 命令处理（在 BGP worker 线程执行）
 */
#include "bgp_show.h"

#include <arpa/inet.h>
#include <glib.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bgp_attr_intern.h"
#include "bgp_cli.h"
#include "bgp_conn.h"
#include "bgp_ext_community.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_show_route.h"
#include "bgp_update_group.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "vrf.h"

/* show 路径专属分片流状态，仅在 BGP worker 线程访问 */
static cli_chunk_stream_t g_bgp_show_stream;

void bgp_show_send_cli_response(dev_ipc_message_t *msg, const char *text)
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

int bgp_work_send_chunked_response(dev_ipc_message_t *msg, GString *full_text)
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

bgp_cli_ctx_t bgp_cli_ctx_default(void)
{
    bgp_cli_ctx_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.vrf_name, sizeof(c.vrf_name), "%s", VRF_PUBLIC_VRF_NAME);
    c.afi = BGP_AFI_IPV4;
    c.safi = BGP_SAFI_UNICAST;
    return c;
}

static int bgp_cli_ctx_str_copy(const cli_tlv_entry_t *entry, char *out, size_t out_cap)
{
    if (!entry || entry->type != CLI_TLV_TYPE_CTX_STR || !entry->value || out_cap == 0)
    {
        return 0;
    }
    uint16_t copy_len = entry->length;
    if (copy_len >= out_cap)
    {
        copy_len = (uint16_t)(out_cap - 1);
    }
    memcpy(out, entry->value, copy_len);
    out[copy_len] = '\0';
    return 1;
}

void bgp_cli_ctx_parse(bgp_cli_ctx_t *ctx, cli_tlv_entry_t *entry)
{
    switch (entry->cfg_id)
    {
        case CLI_CTX_ID_BGP_AFI:
            ctx->afi = (bgp_afi_t)cli_tlv_entry_get_ctx_uint32(entry);
            break;
        case CLI_CTX_ID_BGP_SAFI:
            ctx->safi = (bgp_safi_t)cli_tlv_entry_get_ctx_uint32(entry);
            break;
        case CLI_CTX_ID_VRF_NAME:
            (void)bgp_cli_ctx_str_copy(entry, ctx->vrf_name, sizeof(ctx->vrf_name));
            break;
        default:
            break;
    }
}

bgp_vrf_t *bgp_show_lookup_vrf(const bgp_cli_ctx_t *ctx)
{
    if (!ctx || !g_bgp_work_local->protocol)
    {
        return NULL;
    }
    if (strcmp(ctx->vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        return bgp_protocol_get_vrf(g_bgp_work_local->protocol, BGP_VRF_PUBLIC_ID);
    }
    const vrf_api_cache_entry_t *entry = vrf_api_cache_lookup_by_name(ctx->vrf_name);
    return entry ? bgp_protocol_get_vrf(g_bgp_work_local->protocol, entry->vrf_id) : NULL;
}

const char *bgp_af_str(bgp_afi_t afi, bgp_safi_t safi)
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
    if (afi == BGP_AFI_IPV4 && safi == BGP_SAFI_VPN_UNICAST)
    {
        return "vpnv4";
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
const char *bgp_origin_str(bgp_origin_t origin)
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

/** 将 usec 时间戳格式化为本地时间字符串 */
void bgp_fmt_time_usec(gint64 usec, char *buf, size_t sz)
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
            case 8:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_VPN_UNICAST;
                has_af = TRUE;
                break;
            case 7:
            {
                /* vrf <vrf-name> (仅 ipv4/ipv6 unicast 支持) */
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ctx.vrf_name, sizeof(ctx.vrf_name), "%s", s);
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
            msg, "BGP Error: Missing address-family. Use 'af ipv4-unicast', 'af ipv6-unicast', 'af ipv4-qp', or 'af "
                 "ipv6-qp'.\r\n");
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
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "ROUTE-REFRESH", sess->rx_msg_stats.route_refresh);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "Unknown", sess->rx_msg_stats.unknown);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "Total", sess->rx_msg_stats.total);

    g_string_append(resp_buf, "\r\n  Sent Messages:\r\n");
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "OPEN", sess->tx_msg_stats.open);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "UPDATE", sess->tx_msg_stats.update);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "NOTIFICATION", sess->tx_msg_stats.notification);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "KEEPALIVE", sess->tx_msg_stats.keepalive);
    g_string_append_printf(resp_buf, "  %-24s: %u\r\n", "ROUTE-REFRESH", sess->tx_msg_stats.route_refresh);
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
            case 8:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_VPN_UNICAST;
                has_af = TRUE;
                break;
            case 7:
            {
                /* vrf <vrf-name> (仅 ipv4/ipv6 unicast 支持) */
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ctx.vrf_name, sizeof(ctx.vrf_name), "%s", s);
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

    bgp_vrf_t *vrf = bgp_show_lookup_vrf(&ctx);
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

/** intern 表遍历回调：把单条 attr 追加到摘要表格中 */
static void bgp_show_attr_summary_cb(const bgp_attr_ref_t *ref, gpointer user_data)
{
    GString *b = (GString *)user_data;
    if (!ref || !b)
    {
        return;
    }
    g_string_append_printf(b, "  %-8u %-6u %s\r\n", ref->attr_id, ref->refcnt,
                           ref->attr.as_path[0] ? ref->attr.as_path : "-");
}

/**
 * @brief 格式化单条属性详情到 GString
 */
static void bgp_show_attr_detail(GString *buf, const bgp_attr_ref_t *ref)
{
    const bgp_attr_t *a = &ref->attr;
    g_string_append_printf(buf, "  Attr-ID    : %u\r\n", ref->attr_id);
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
    if (a->ext_communities_len > 0)
    {
        char ext_comm[BGP_ATTR_COMMUNITY_MAX];
        bgp_ext_community_format(a->ext_communities, a->ext_communities_len, ext_comm, sizeof(ext_comm));
        g_string_append_printf(buf, "  Ext-Comm   : %s\r\n", ext_comm);
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
 * @brief 处理 show bgp attr af <afi-safi> [vrf <vrf-name>] [<attr-id>] 命令
 *
 * group_id=14, cfg_id: 1/2/4/5/6=AF, 7=vrf-name, 8=attr-id
 */
static int handle_bgp_show_attr(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    bgp_cli_ctx_t ctx = bgp_cli_ctx_default();
    gboolean has_af = FALSE;
    uint32_t attr_id = 0;
    gboolean has_id = FALSE;

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
            {
                const char *s = cli_tlv_entry_get_text(&entry);
                if (s)
                {
                    snprintf(ctx.vrf_name, sizeof(ctx.vrf_name), "%s", s);
                }
                break;
            }
            case 9:
                ctx.afi = BGP_AFI_IPV4;
                ctx.safi = BGP_SAFI_VPN_UNICAST;
                has_af = TRUE;
                break;
            case 8:
                attr_id = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_id = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_af)
    {
        bgp_show_send_cli_response(msg, "BGP Error: Missing address-family. Use 'show bgp attr af <afi-safi>'.\r\n");
        return ERRCODE_FAIL;
    }
    if (!g_bgp_work_local->protocol)
    {
        bgp_show_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    GString *buf = g_string_sized_new(512);
    bgp_vrf_t *vrf = bgp_show_lookup_vrf(&ctx);
    if (!vrf)
    {
        g_string_append_printf(buf, "\r\nBGP Error: VRF not found.\r\n\r\n");
        return bgp_work_send_chunked_response(msg, buf);
    }
    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(ctx.afi, ctx.safi));
    if (!inst)
    {
        g_string_append_printf(buf, "\r\nBGP Error: AF instance not found (%s).\r\n\r\n",
                               bgp_af_str(ctx.afi, ctx.safi));
        return bgp_work_send_chunked_response(msg, buf);
    }

    if (!has_id)
    {
        /* 摘要模式：显示 intern 表统计 + 逐条列表 */
        g_string_append_printf(buf, "\r\nBGP Attribute Intern Table (AF: %s, VRF: %s)\r\n",
                               bgp_af_str(ctx.afi, ctx.safi), ctx.vrf_name);
        g_string_append_printf(buf, "  Unique attributes: %u\r\n\r\n", bgp_attr_intern_count(inst));
        g_string_append(buf, "  Attr-ID  Refs   AS-Path\r\n");
        g_string_append(buf, "  -------- ------ ----------------\r\n");
        bgp_attr_intern_foreach(inst, bgp_show_attr_summary_cb, buf);
        g_string_append(buf, "\r\n");
        return bgp_work_send_chunked_response(msg, buf);
    }

    /* 详情模式：按 ID 查找并输出 */
    const bgp_attr_ref_t *ref = bgp_attr_find_by_id(inst, attr_id);
    if (!ref)
    {
        g_string_append_printf(buf, "\r\nBGP Error: Attribute ID %u not found in AF %s VRF %s.\r\n\r\n", attr_id,
                               bgp_af_str(ctx.afi, ctx.safi), ctx.vrf_name);
        return bgp_work_send_chunked_response(msg, buf);
    }

    g_string_append_printf(buf, "\r\nBGP Attribute Detail (AF: %s, VRF: %s)\r\n", bgp_af_str(ctx.afi, ctx.safi),
                           ctx.vrf_name);
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
