/**
 * @file   bgp_pkt.c
 * @brief  BGP 报文组包与解析实现
 * @author jhb
 * @date   2026/03/07
 */
#include "bgp_pkt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp_instance.h"
#include "bgp_parse.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "log.h"

/** BGP 报文 Marker：16 字节全 0xFF */
static const uint8_t BGP_MARKER[16] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// ============================================================================
// 报文发送
// ============================================================================

int bgp_pkt_send_open(bgp_conn_t *conn, uint32_t local_as, const char *router_id, GList *af_peers)
{
    char _ip[64];
    net_addr_to_str(&conn->peer_addr, _ip, sizeof(_ip));

    /* 从 session->flags 读取本地配置的能力集 */
    uint32_t cap_flags = (conn->session) ? conn->session->flags : BGP_SESS_CAP_DEFAULT;
    gboolean send_rr = BIT_TEST(cap_flags, BGP_SESS_CAP_ROUTE_REFRESH);
    gboolean send_as4 = BIT_TEST(cap_flags, BGP_SESS_CAP_AS4);

    /* 计算 Optional Parameters 长度：
     *   每个 AF  = type(1)+len(1)+cap_code(1)+cap_len(1)+AFI(2)+rsv(1)+SAFI(1) = 8 B
     *   Route Refresh = type(1)+len(1)+cap_code(1)+cap_len(1)                   = 4 B
     *   AS4     = type(1)+len(1)+cap_code(1)+cap_len(1)+AS(4)                   = 8 B
     */
    guint n_afs = af_peers ? g_list_length(af_peers) : 0;
    uint8_t extra_len = (uint8_t)((send_rr ? 4U : 0U) + (send_as4 ? 8U : 0U));
    uint8_t opt_len = (uint8_t)(8U * n_afs + extra_len);

    /* BGP OPEN: header(19) + version(1) + my-as(2) + hold-time(2) + bgp-id(4) + opt-len(1) + opt(n) */
    uint16_t total_len = (uint16_t)(29 + opt_len);
    /* 最多 16 个 AF(128 B) + Route Refresh(4 B) + AS4(8 B) = 140 B extra */
    uint8_t msg[29 + 8 * 16 + 12];

    memcpy(msg, BGP_MARKER, 16);

    uint16_t len_be = htons(total_len);
    memcpy(msg + 16, &len_be, 2);
    msg[18] = (uint8_t)BGP_MSG_OPEN;

    msg[19] = 4; /* BGP 版本 */

    /* my-as：AS4 时 RFC 6793 要求填写 AS_TRANS(23456)；简化实现直接截断 */
    uint16_t as_field = send_as4 ? (local_as > 65535U ? 23456U : (uint16_t)local_as) : (uint16_t)local_as;
    uint16_t as_be = htons(as_field);
    memcpy(msg + 20, &as_be, 2);

    uint16_t hold_be = htons(BGP_HOLD_TIME);
    memcpy(msg + 22, &hold_be, 2);

    struct in_addr bgp_id;
    if (inet_pton(AF_INET, router_id ? router_id : "0.0.0.0", &bgp_id) <= 0)
    {
        bgp_id.s_addr = 0;
    }
    memcpy(msg + 24, &bgp_id, 4);

    msg[28] = opt_len;

    uint8_t *opt_ptr = msg + 29;

    /* 填充 MP 扩展能力（每个 AF 一个 Optional Parameter） */
    for (GList *l = af_peers; l != NULL; l = l->next)
    {
        bgp_peer_t *ap = (bgp_peer_t *)l->data;
        opt_ptr[0] = 2; /* type=Capability */
        opt_ptr[1] = 6; /* len=6 */
        opt_ptr[2] = 1; /* code=MP Extensions */
        opt_ptr[3] = 4; /* cap-len=4 */
        uint16_t afi_be = htons((uint16_t)ap->inst->afi);
        memcpy(opt_ptr + 4, &afi_be, 2);
        opt_ptr[6] = 0;
        opt_ptr[7] = (uint8_t)ap->inst->safi;
        opt_ptr += 8;
    }

    /* 填充 Route Refresh 能力（RFC 2918）*/
    if (send_rr)
    {
        opt_ptr[0] = 2;                     /* type=Capability */
        opt_ptr[1] = 2;                     /* len=2 */
        opt_ptr[2] = BGP_CAP_ROUTE_REFRESH; /* code=2 */
        opt_ptr[3] = 0;                     /* cap-len=0 */
        opt_ptr += 4;
    }

    /* 填充 4 字节 AS 能力（RFC 6793）*/
    if (send_as4)
    {
        opt_ptr[0] = 2;           /* type=Capability */
        opt_ptr[1] = 6;           /* len=6 */
        opt_ptr[2] = BGP_CAP_AS4; /* code=65 */
        opt_ptr[3] = 4;           /* cap-len=4 */
        uint32_t as4_be = htonl(local_as);
        memcpy(opt_ptr + 4, &as4_be, 4);
        opt_ptr += 8;
    }

    if (n_afs > 0 || send_rr || send_as4)
    {
        LOG_INFO("BGP: OPEN 能力集: AF=%u%s%s", n_afs, send_rr ? " RR" : "", send_as4 ? " AS4" : "");
    }

    ssize_t n = send(conn->fd, msg, total_len, MSG_NOSIGNAL);
    if (n != (ssize_t)total_len)
    {
        LOG_ERROR("BGP: 向 %s 发送 OPEN 失败", _ip);
        return -1;
    }

    /* 记录本次 OPEN 实际发出的能力集 */
    if (conn->session)
    {
        conn->session->local_caps = cap_flags;
    }

    LOG_INFO("BGP: 已向 %s 发送 OPEN (AS=%u, ID=%s)", _ip, local_as, router_id ? router_id : "0.0.0.0");
    return 0;
}

int bgp_pkt_send_keepalive(bgp_conn_t *conn)
{
    char _ip[64];
    net_addr_to_str(&conn->peer_addr, _ip, sizeof(_ip));

    /* KEEPALIVE 只有 19 字节头部 */
    uint8_t msg[BGP_MSG_HEADER_SIZE];
    memcpy(msg, BGP_MARKER, 16);

    uint16_t len = htons(BGP_MSG_HEADER_SIZE);
    memcpy(msg + 16, &len, 2);
    msg[18] = (uint8_t)BGP_MSG_KEEPALIVE;

    ssize_t n = send(conn->fd, msg, sizeof(msg), MSG_NOSIGNAL);
    if (n != (ssize_t)sizeof(msg))
    {
        LOG_ERROR("BGP: 向 %s 发送 KEEPALIVE 失败", _ip);
        return -1;
    }

    LOG_DEBUG("BGP: 已向 %s 发送 KEEPALIVE", _ip);
    return 0;
}

// ============================================================================
// 数据接收与状态机
// ============================================================================

/**
 * @brief 解析 BGP OPEN 报文体，填充 session 的 remote_as / remote_id / negotiated_afs
 * @param conn     连接处理器
 * @param body     报文体指针（header 之后）
 * @param body_len 报文体长度
 * @return 0 成功，-1 格式错误
 */
static int parse_bgp_open(bgp_conn_t *conn, const uint8_t *body, uint16_t body_len)
{
    char _ip[64];
    net_addr_to_str(&conn->peer_addr, _ip, sizeof(_ip));

    bgp_open_msg_t msg;
    if (bgp_open_parse(body, body_len, &msg) < 0)
    {
        LOG_ERROR("BGP: peer %s OPEN 解析失败", _ip);
        return -1;
    }

    /* 优先使用 4 字节 AS（RFC 6793 AS_TRANS 处理） */
    conn->session->remote_as = msg.cap_as4 ? msg.cap_as4 : msg.my_as;

    strncpy(conn->session->remote_id, msg.bgp_id, sizeof(conn->session->remote_id) - 1);
    conn->session->remote_id[sizeof(conn->session->remote_id) - 1] = '\0';

    /* 记录远端能力集 */
    uint32_t remote_caps = 0;
    if (msg.cap_route_refresh)
    {
        BIT_SET(remote_caps, BGP_SESS_CAP_ROUTE_REFRESH);
    }
    if (msg.cap_as4)
    {
        BIT_SET(remote_caps, BGP_SESS_CAP_AS4);
    }
    conn->session->remote_caps = remote_caps;
    conn->session->negotiated_caps = conn->session->local_caps & remote_caps;

    /* 记录并协商 Hold Time（取本地与远端的较小值，RFC 4271 §4.2） */
    conn->session->remote_hold = msg.hold_time;
    conn->session->negotiated_hold = (msg.hold_time < BGP_HOLD_TIME) ? msg.hold_time : BGP_HOLD_TIME;

    LOG_INFO("BGP: 收到 %s 的 OPEN (AS=%u, ID=%s, hold=%u, caps=0x%02X)", _ip, conn->session->remote_as,
             conn->session->remote_id, msg.hold_time, remote_caps);

    /* 将 MP 能力写入 negotiated_afs */
    if (conn->session->negotiated_afs)
    {
        g_list_free_full(conn->session->negotiated_afs, g_free);
        conn->session->negotiated_afs = NULL;
    }
    for (uint8_t i = 0; i < msg.mp_count; i++)
    {
        char af_key[32];
        snprintf(af_key, sizeof(af_key), "%u-%u", msg.mp_afs[i], msg.mp_safis[i]);
        conn->session->negotiated_afs = g_list_append(conn->session->negotiated_afs, g_strdup(af_key));
        LOG_INFO("BGP: peer %s MP 能力: AFI=%u SAFI=%u", _ip, msg.mp_afs[i], msg.mp_safis[i]);
    }

    return 0;
}

int bgp_pkt_on_data(bgp_conn_t *conn)
{
    char _ip[64];
    net_addr_to_str(&conn->peer_addr, _ip, sizeof(_ip));

    bgp_session_t *sess = conn->session;

    /* 将数据追加到接收缓冲区 */
    ssize_t n = recv(conn->fd, sess->recv_buf + sess->recv_len, BGP_RECV_BUF_SIZE - sess->recv_len, 0);

    if (n == 0)
    {
        LOG_INFO("BGP: peer %s 关闭了连接", _ip);
        return -1;
    }
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0; /* 暂无数据，继续等待 */
        }
        LOG_PERROR("BGP: recv from %s 失败", _ip);
        return -1;
    }

    sess->recv_len += (uint32_t)n;

    /* 逐帧解析 BGP 报文 */
    while (sess->recv_len >= BGP_MSG_HEADER_SIZE)
    {
        /* 校验 Marker */
        if (memcmp(sess->recv_buf, BGP_MARKER, 16) != 0)
        {
            LOG_ERROR("BGP: peer %s Marker 校验失败，关闭连接", _ip);
            return -1;
        }

        uint16_t msg_len_be;
        memcpy(&msg_len_be, sess->recv_buf + 16, 2);
        uint16_t msg_len = ntohs(msg_len_be);

        if (msg_len < BGP_MSG_HEADER_SIZE || msg_len > BGP_RECV_BUF_SIZE)
        {
            LOG_ERROR("BGP: peer %s 报文长度 %u 非法", _ip, msg_len);
            return -1;
        }

        if (sess->recv_len < msg_len)
        {
            /* 数据未到齐，等待下次读取 */
            break;
        }

        uint8_t msg_type = sess->recv_buf[18];
        const uint8_t *body = sess->recv_buf + BGP_MSG_HEADER_SIZE;
        uint16_t body_len = msg_len - BGP_MSG_HEADER_SIZE;

        switch (msg_type)
        {
            case BGP_MSG_OPEN:
                /* 收到对端 OPEN -> 解析 -> 回复 KEEPALIVE -> 进入 OPEN_CONFIRM */
                if (parse_bgp_open(conn, body, body_len) < 0)
                {
                    return -1;
                }
                if (bgp_pkt_send_keepalive(conn) < 0)
                {
                    return -1;
                }
                conn->session->state = BGP_CONN_STATE_OPEN_CONFIRM;
                break;

            case BGP_MSG_KEEPALIVE:
                if (conn->session->state == BGP_CONN_STATE_OPEN_CONFIRM)
                {
                    conn->session->state = BGP_CONN_STATE_ESTABLISHED;
                    LOG_INFO("BGP: 与 %s (AS%u) 会话已建立", _ip, sess->remote_as);
                }
                else
                {
                    /* ESTABLISHED 状态下的周期 KEEPALIVE：通知 bgp_main 重置 Hold 定时器 */
                    LOG_DEBUG("BGP: 收到 %s KEEPALIVE", _ip);
                    sess->hold_reset_pending = TRUE;
                }
                break;

            case BGP_MSG_UPDATE:
            {
                bgp_update_result_t *upd = NULL;
                uint32_t parse_flags = BGP_PARSE_FLAG_AS4;
                if (bgp_update_parse(body, body_len, parse_flags, &upd) == 0 && upd)
                {
                    bgp_rib_update_stats_t rib_stats = {0};
                    bgp_vrf_apply_update(sess->vrf, &sess->neighbor_addr, upd, &rib_stats);

                    LOG_INFO(
                        "BGP: %s UPDATE: afi=%u safi=%u +%u -%u | RIB new=%u upd=%u wd=%u miss=%u heads=%u routes=%u",
                        _ip, upd->afi, upd->safi, upd->reach_len, upd->unreach_len, rib_stats.reach_new,
                        rib_stats.reach_update, rib_stats.unreach_removed, rib_stats.unreach_miss,
                        bgp_vrf_rib_head_count(sess->vrf), bgp_vrf_rib_route_count(sess->vrf));
                    bgp_update_result_free(upd);
                }
                else
                {
                    LOG_WARN("BGP: %s UPDATE 解析失败", _ip);
                }
                /* UPDATE 也重置 Hold 定时器（RFC 4271 §8.2.2） */
                sess->hold_reset_pending = TRUE;
                break;
            }

            case BGP_MSG_NOTIFICATION:
            {
                bgp_notif_msg_t notif;
                if (bgp_notif_parse(body, body_len, &notif) == 0)
                {
                    LOG_WARN("BGP: 收到 %s NOTIFICATION: %s (code=%u sub=%u)", _ip, notif.error_str, notif.error_code,
                             notif.error_subcode);
                }
                else
                {
                    LOG_WARN("BGP: 收到 %s NOTIFICATION 报文，关闭会话", _ip);
                }
                return -1;
            }

            default:
                LOG_WARN("BGP: peer %s 未知报文类型 %u，关闭连接", _ip, msg_type);
                return -1;
        }

        /* 将已处理的报文从缓冲区移除 */
        uint32_t remaining = sess->recv_len - msg_len;
        if (remaining > 0)
        {
            memmove(sess->recv_buf, sess->recv_buf + msg_len, remaining);
        }
        sess->recv_len = remaining;
    }

    return 0;
}
