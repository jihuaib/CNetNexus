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
#include "bgp_session.h"
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

    /* 计算 Optional Parameters 长度：每个 AF 占 8 字节 */
    guint n_afs = af_peers ? g_list_length(af_peers) : 0;
    uint8_t opt_len = (uint8_t)(8 * n_afs);

    /* BGP OPEN: header(19) + version(1) + my-as(2) + hold-time(2) + bgp-id(4) + opt-len(1) + opt(n) */
    uint16_t total_len = (uint16_t)(29 + opt_len);
    uint8_t msg[29 + 8 * 16]; /* 最多支持 16 个 AF */

    memcpy(msg, BGP_MARKER, 16);

    uint16_t len_be = htons(total_len);
    memcpy(msg + 16, &len_be, 2);
    msg[18] = (uint8_t)BGP_MSG_OPEN;

    msg[19] = 4; /* BGP 版本 */

    uint16_t as_be = htons((uint16_t)local_as);
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

    /* 填充 MP 扩展能力 Optional Parameters */
    if (n_afs > 0)
    {
        uint8_t *opt_ptr = msg + 29;
        for (GList *l = af_peers; l != NULL; l = l->next)
        {
            bgp_peer_t *ap = (bgp_peer_t *)l->data;
            /* Optional Parameter: type=2(Capability), len=6 */
            opt_ptr[0] = 2;
            opt_ptr[1] = 6;
            /* Capability: code=1(MP Extensions), cap-len=4 */
            opt_ptr[2] = 1;
            opt_ptr[3] = 4;
            /* AFI(2B) + reserved(1B) + SAFI(1B)，从所属 instance 获取 afi/safi */
            uint16_t afi_be = htons((uint16_t)ap->inst->afi);
            memcpy(opt_ptr + 4, &afi_be, 2);
            opt_ptr[6] = 0;
            opt_ptr[7] = (uint8_t)ap->inst->safi;
            opt_ptr += 8;
        }
        LOG_INFO("BGP: 携带 %u 个 AF 能力参数发送 OPEN", n_afs);
    }

    ssize_t n = send(conn->fd, msg, total_len, MSG_NOSIGNAL);
    if (n != (ssize_t)total_len)
    {
        LOG_ERROR("BGP: 向 %s 发送 OPEN 失败", _ip);
        return -1;
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

    /* 最小 OPEN 报文体：version(1)+as(2)+hold(2)+bgp-id(4)+opt-len(1) = 10 */
    if (body_len < 10)
    {
        LOG_ERROR("BGP: peer %s OPEN 报文体过短 (%u)", _ip, body_len);
        return -1;
    }

    uint8_t version = body[0];
    if (version != 4)
    {
        LOG_WARN("BGP: peer %s OPEN version=%u 不支持", _ip, version);
        return -1;
    }

    uint16_t remote_as_be;
    memcpy(&remote_as_be, body + 1, 2);
    conn->session->remote_as = ntohs(remote_as_be);

    /* BGP Router ID 始终为 IPv4 格式（协议规定） */
    struct in_addr bgp_id;
    memcpy(&bgp_id, body + 5, 4);
    inet_ntop(AF_INET, &bgp_id, conn->session->remote_id, sizeof(conn->session->remote_id));

    LOG_INFO("BGP: 收到 %s 的 OPEN (AS=%u, ID=%s)", _ip, conn->session->remote_as, conn->session->remote_id);

    /* 解析 Optional Parameters，提取 MP 扩展能力 */
    uint8_t opt_len = body[9];
    uint16_t pos = 10;
    while (pos + 2 <= (uint16_t)(10 + opt_len) && pos + 2 <= body_len)
    {
        uint8_t param_type = body[pos];
        uint8_t param_len = body[pos + 1];
        pos += 2;

        if (param_type == 2) /* Capability */
        {
            uint8_t cap_end = (uint8_t)(pos + param_len);
            uint8_t cap_pos = (uint8_t)pos;
            while (cap_pos + 2 <= cap_end && cap_pos + 2 <= body_len)
            {
                uint8_t cap_code = body[cap_pos];
                uint8_t cap_len = body[cap_pos + 1];
                cap_pos += 2;

                if (cap_code == 1 && cap_len >= 4 && cap_pos + 4 <= body_len)
                {
                    /* Multiprotocol Extensions (RFC 4760) */
                    uint16_t afi_be;
                    memcpy(&afi_be, body + cap_pos, 2);
                    uint16_t afi = ntohs(afi_be);
                    uint8_t safi = body[cap_pos + 3];

                    char af_key[32];
                    snprintf(af_key, sizeof(af_key), "%u-%u", afi, safi);
                    conn->session->negotiated_afs = g_list_append(conn->session->negotiated_afs, g_strdup(af_key));
                    LOG_INFO("BGP: peer %s 携带 MP 能力: AFI=%u SAFI=%u", _ip, afi, safi);
                }
                cap_pos += cap_len;
            }
        }

        pos += param_len;
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
                    /* ESTABLISHED 状态下的周期 KEEPALIVE */
                    LOG_DEBUG("BGP: 收到 %s KEEPALIVE", _ip);
                }
                break;

            case BGP_MSG_UPDATE:
                LOG_INFO("BGP: 收到 %s UPDATE 报文（暂未处理）", _ip);
                break;

            case BGP_MSG_NOTIFICATION:
                LOG_WARN("BGP: 收到 %s NOTIFICATION 报文，关闭会话", _ip);
                return -1;

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
