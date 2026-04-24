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
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp.h"
#include "bgp_bmp_thread.h"
#include "bgp_instance.h"
#include "bgp_pkt_build.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "log.h"

/** BGP 报文 Marker：16 字节全 0xFF */
static const uint8_t BGP_MARKER[16] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static gboolean bgp_open_af_requires_ext_nexthop(const bgp_peer_t *peer, const net_addr_t *remote_addr)
{
    if (!peer || !peer->inst || peer->inst->afi != BGP_AFI_IPV4)
    {
        return FALSE;
    }

    /* RFC 8950: only IPv4 NLRI with an IPv6 next hop needs Capability 5. */
    if (remote_addr && remote_addr->family == AF_INET6)
    {
        return TRUE;
    }

    /* Project extension: IPv4-QP uses an IPv6 BID as next hop. */
    return peer->inst->safi == BGP_SAFI_QP;
}

// ============================================================================
// 报文发送
// ============================================================================

int bgp_pkt_send_open(bgp_conn_t *conn, uint32_t local_as, uint32_t router_id, GList *af_peers)
{
    char _ip[64];
    net_addr_to_str(&conn->peer_addr, _ip, sizeof(_ip));

    /* 从 session->flags 读取本地配置的能力集 */
    uint32_t cap_flags = (conn->session) ? conn->session->flags : BGP_SESS_CAP_DEFAULT;
    gboolean send_rr = BIT_TEST(cap_flags, BGP_SESS_CAP_ROUTE_REFRESH);
    gboolean send_as4 = BIT_TEST(cap_flags, BGP_SESS_CAP_AS4);
    gboolean want_ext_nh = BIT_TEST(cap_flags, BGP_SESS_CAP_EXT_NEXTHOP);

    /* 统计本次 OPEN 中实际需要发送的 Extended Next Hop 三元组。 */
    guint ext_nh_count = 0;
    if (want_ext_nh && af_peers)
    {
        for (GList *l = af_peers; l != NULL; l = l->next)
        {
            bgp_peer_t *ap = (bgp_peer_t *)l->data;
            if (bgp_open_af_requires_ext_nexthop(ap, &conn->peer_addr))
            {
                ext_nh_count++;
            }
        }
    }
    gboolean send_ext_nh = (ext_nh_count > 0);
    uint32_t local_caps_sent = cap_flags;
    if (!send_ext_nh)
    {
        BIT_CLR(local_caps_sent, BGP_SESS_CAP_EXT_NEXTHOP);
    }

    /* 计算 Optional Parameters 长度：
     *   每个 AF      = type(1)+len(1)+cap_code(1)+cap_len(1)+AFI(2)+rsv(1)+SAFI(1)          = 8 B
     *   Route Refresh= type(1)+len(1)+cap_code(1)+cap_len(1)                                 = 4 B
     *   AS4          = type(1)+len(1)+cap_code(1)+cap_len(1)+AS(4)                            = 8 B
     *   Ext NH       = type(1)+len(1)+cap_code(1)+cap_len(1)+N*(NLRI_AFI(2)+SAFI(2)+NH_AFI(2))
     *                = 4 + N*6
     */
    guint n_afs = af_peers ? g_list_length(af_peers) : 0;
    uint8_t ext_nh_opt_len = (ext_nh_count > 0) ? (uint8_t)(4U + ext_nh_count * 6U) : 0;
    uint8_t extra_len = (uint8_t)((send_rr ? 4U : 0U) + (send_as4 ? 8U : 0U) + ext_nh_opt_len);
    uint8_t opt_len = (uint8_t)(8U * n_afs + extra_len);

    /* BGP OPEN: header(19) + version(1) + my-as(2) + hold-time(2) + bgp-id(4) + opt-len(1) + opt(n) */
    uint16_t total_len = (uint16_t)(29 + opt_len);
    /* 最多 16 个 AF(128 B) + Route Refresh(4 B) + AS4(8 B) + Ext NH(4+16*6=100 B) = 240 B extra */
    uint8_t msg[29 + 8 * 16 + 12 + 4 + 16 * 6];

    memcpy(msg, BGP_MARKER, 16);

    uint16_t len_be = htons(total_len);
    memcpy(msg + 16, &len_be, 2);
    msg[18] = (uint8_t)BGP_MSG_OPEN;

    msg[19] = 4; /* BGP 版本 */

    /* my-as：AS4 时 RFC 6793 要求填写 AS_TRANS(23456)；简化实现直接截断 */
    uint16_t as_field = send_as4 ? (local_as > 65535U ? 23456U : (uint16_t)local_as) : (uint16_t)local_as;
    uint16_t as_be = htons(as_field);
    memcpy(msg + 20, &as_be, 2);

    uint16_t local_hold = BGP_HOLD_TIME;
    if (conn->session && conn->session->vrf && conn->session->vrf->hold_time > 0)
    {
        local_hold = conn->session->vrf->hold_time;
    }
    uint16_t hold_be = htons(local_hold);
    memcpy(msg + 22, &hold_be, 2);

    /* BGP Identifier 直接由主机序 uint32_t 转为网络序写入报文 */
    uint32_t bgp_id_be = htonl(router_id);
    memcpy(msg + 24, &bgp_id_be, 4);

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

    /* 填充 Extended Next Hop 能力（RFC 8950）：仅对 IPv4 AF 声明可使用 IPv6 nexthop */
    if (send_ext_nh)
    {
        uint8_t nh_val_len = (uint8_t)(ext_nh_count * 6U);
        opt_ptr[0] = 2;                         /* type=Capability */
        opt_ptr[1] = (uint8_t)(2 + nh_val_len); /* len=code(1)+cap_len(1)+value */
        opt_ptr[2] = BGP_CAP_EXT_NEXTHOP;       /* code=5 */
        opt_ptr[3] = nh_val_len;                /* cap-len */
        opt_ptr += 4;
        for (GList *l = af_peers; l != NULL; l = l->next)
        {
            bgp_peer_t *ap = (bgp_peer_t *)l->data;
            if (!bgp_open_af_requires_ext_nexthop(ap, &conn->peer_addr))
            {
                continue;
            }
            uint16_t nlri_afi_be = htons((uint16_t)ap->inst->afi);
            uint16_t nlri_safi_be = htons((uint16_t)ap->inst->safi);
            uint16_t nh_afi_be = htons(BGP_AFI_IPV6);
            memcpy(opt_ptr, &nlri_afi_be, 2);
            memcpy(opt_ptr + 2, &nlri_safi_be, 2);
            memcpy(opt_ptr + 4, &nh_afi_be, 2);
            opt_ptr += 6;
        }
    }

    if (n_afs > 0 || send_rr || send_as4 || ext_nh_count > 0)
    {
        LOG_INFO("BGP: OPEN capabilities: AF=%u%s%s%s", n_afs, send_rr ? " RR" : "", send_as4 ? " AS4" : "",
                 ext_nh_count > 0 ? " EXT-NH" : "");
    }

    ssize_t n = send(conn->fd, msg, total_len, MSG_NOSIGNAL);
    if (n != (ssize_t)total_len)
    {
        LOG_ERROR("BGP: Failed to send OPEN to %s", _ip);
        return -1;
    }

    /* 记录本次 OPEN 实际发出的能力集和本地 BGP Identifier（用于 RFC §6.8 碰撞检测） */
    if (conn->session)
    {
        conn->session->local_caps = local_caps_sent;
        conn->session->local_router_id = router_id;
    }

    char _rid_str[16];
    struct in_addr _rid_addr;
    _rid_addr.s_addr = htonl(router_id);
    inet_ntop(AF_INET, &_rid_addr, _rid_str, sizeof(_rid_str));
    LOG_INFO("BGP: Sent OPEN to %s (AS=%u, ID=%s)", _ip, local_as, _rid_str);
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
        LOG_ERROR("BGP: Failed to send KEEPALIVE to %s", _ip);
        return -1;
    }

    LOG_DEBUG("BGP: Sent KEEPALIVE to %s", _ip);
    return 0;
}

int bgp_pkt_send_notification(bgp_conn_t *conn, uint8_t error_code, uint8_t error_subcode)
{
    if (!conn || conn->fd < 0)
    {
        return -1;
    }

    char _ip[64];
    net_addr_to_str(&conn->peer_addr, _ip, sizeof(_ip));

    /* NOTIFICATION：头部 19 B + error_code 1 B + error_subcode 1 B = 21 B */
    uint8_t msg[BGP_MSG_HEADER_SIZE + 2];
    memcpy(msg, BGP_MARKER, 16);

    uint16_t len_be = htons((uint16_t)(BGP_MSG_HEADER_SIZE + 2));
    memcpy(msg + 16, &len_be, 2);
    msg[18] = (uint8_t)BGP_MSG_NOTIFICATION;
    msg[19] = error_code;
    msg[20] = error_subcode;

    ssize_t n = send(conn->fd, msg, sizeof(msg), MSG_NOSIGNAL);
    if (n != (ssize_t)sizeof(msg))
    {
        LOG_ERROR("BGP: Failed to send NOTIFICATION to %s (code=%u sub=%u)", _ip, error_code, error_subcode);
        return -1;
    }

    LOG_INFO("BGP: Sent NOTIFICATION to %s: %s (code=%u sub=%u)", _ip, bgp_notif_error_str(error_code, error_subcode),
             error_code, error_subcode);
    return 0;
}

// ============================================================================
// UPDATE 报文发送
// ============================================================================

/** UPDATE 发送缓冲区最大长度 */
#define BGP_UPDATE_BUF_SIZE 4096

/**
 * @brief 编码 ORIGIN 路径属性（well-known mandatory，1 字节值）
 * @return 写入字节数，-1=空间不足
 */
static int encode_origin(uint8_t *buf, int buf_size, bgp_origin_t origin)
{
    if (buf_size < 4)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_TRANSITIVE;
    buf[1] = BGP_PA_TYPE_ORIGIN;
    buf[2] = 1;
    buf[3] = (uint8_t)origin;
    return 4;
}

/**
 * @brief 从 AS_PATH 字符串解析 AS_SEQUENCE 成员，跳过 AS_SET 花括号块
 * @return 解析到的 AS 数量
 */
static int parse_as_seq(const char *as_path_str, uint32_t *out, int max)
{
    int count = 0;
    const char *p = as_path_str;
    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        /* 跳过 AS_SET 花括号块，不将其纳入 AS_SEQUENCE 编码 */
        if (*p == '{')
        {
            while (*p != '\0' && *p != '}')
            {
                p++;
            }
            if (*p == '}')
            {
                p++;
            }
            continue;
        }
        char *end;
        unsigned long val = strtoul(p, &end, 10);
        if (end == p)
        {
            break;
        }
        if (count < max)
        {
            out[count] = (uint32_t)val;
        }
        count++;
        p = end;
    }
    return count;
}

/**
 * @brief 编码 AS_PATH 路径属性（单 AS_SEQUENCE 段，4 字节 AS 号）
 * @return 写入字节数，-1=空间不足
 */
static int encode_as_path(uint8_t *buf, int buf_size, const char *as_path_str)
{
    uint32_t as_array[256];
    int n = 0;
    if (as_path_str && as_path_str[0] != '\0')
    {
        n = parse_as_seq(as_path_str, as_array, 256);
        if (n < 0)
        {
            n = 0;
        }
    }
    /* 非空时编码一个 AS_SEQUENCE 段（类型 1B + 长度 1B + n*4B）；空则属性值长度为 0 */
    int seg_len = (n > 0) ? (2 + n * 4) : 0;
    int attr_total = 3 + seg_len;
    if (buf_size < attr_total)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_TRANSITIVE;
    buf[1] = BGP_PA_TYPE_AS_PATH;
    buf[2] = (uint8_t)seg_len;
    if (n > 0)
    {
        buf[3] = 2; /* AS_SEQUENCE */
        buf[4] = (uint8_t)n;
        for (int i = 0; i < n; i++)
        {
            uint32_t asn_be = htonl(as_array[i]);
            memcpy(buf + 5 + i * 4, &asn_be, 4);
        }
    }
    return attr_total;
}

/**
 * @brief 编码 LOCAL_PREF 路径属性（well-known discretionary，4 字节）
 * @return 写入字节数，-1=空间不足
 */
static int encode_local_pref(uint8_t *buf, int buf_size, uint32_t local_pref)
{
    if (buf_size < 7)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_TRANSITIVE;
    buf[1] = BGP_PA_TYPE_LOCAL_PREF;
    buf[2] = 4;
    uint32_t lp_be = htonl(local_pref);
    memcpy(buf + 3, &lp_be, 4);
    return 7;
}

/**
 * @brief 编码 MED 路径属性（optional non-transitive，4 字节）
 * @return 写入字节数，-1=空间不足
 */
static int encode_med(uint8_t *buf, int buf_size, uint32_t med)
{
    if (buf_size < 7)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MED;
    buf[2] = 4;
    uint32_t med_be = htonl(med);
    memcpy(buf + 3, &med_be, 4);
    return 7;
}

/**
 * @brief 编码 COMMUNITY 路径属性（格式 "ASN:VAL ..."，空字符串时返回 0 跳过）
 * @return 写入字节数，0=无 community，-1=空间不足
 */
static int encode_community(uint8_t *buf, int buf_size, const char *community_str)
{
    if (!community_str || community_str[0] == '\0')
    {
        return 0;
    }
    uint32_t comms[256];
    int count = 0;
    const char *p = community_str;
    while (*p != '\0' && count < 256)
    {
        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        char *end1, *end2;
        unsigned long asn = strtoul(p, &end1, 10);
        if (end1 == p || *end1 != ':')
        {
            break;
        }
        unsigned long val = strtoul(end1 + 1, &end2, 10);
        comms[count++] = (uint32_t)(((asn & 0xFFFFu) << 16) | (val & 0xFFFFu));
        p = end2;
    }
    if (count == 0)
    {
        return 0;
    }
    int attr_total = 3 + count * 4;
    if (buf_size < attr_total)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_OPTIONAL | BGP_PA_FLAG_TRANSITIVE;
    buf[1] = BGP_PA_TYPE_COMMUNITY;
    buf[2] = (uint8_t)(count * 4);
    for (int i = 0; i < count; i++)
    {
        uint32_t c_be = htonl(comms[i]);
        memcpy(buf + 3 + i * 4, &c_be, 4);
    }
    return attr_total;
}

// ============================================================================
// Packed UPDATE / WITHDRAW 构建（多 NLRI 共享属性）
// ============================================================================

int bgp_pkt_build_packed_update(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                const bgp_attr_t *attr, const bgp_nexthop_t *nexthop, uint16_t afi, uint8_t safi,
                                int *out_packed_count)
{
    if (out_packed_count)
    {
        *out_packed_count = 0;
    }
    if (!buf || buf_size < BGP_MSG_HEADER_SIZE || !nlri_list || nlri_count <= 0 || !attr || !nexthop)
    {
        return -1;
    }

    const bgp_pkt_af_enc_t *enc = bgp_pkt_af_enc_find(afi, safi);
    if (!enc || !enc->encode_reach_pa_packed || !enc->encode_reach_nlri_packed)
    {
        return -1;
    }

    int pos = BGP_MSG_HEADER_SIZE;

    /* Withdrawn Routes Length = 0 */
    if (pos + 2 > buf_size)
    {
        return -1;
    }
    buf[pos++] = 0;
    buf[pos++] = 0;

    /* Total Path Attribute Length（稍后回填） */
    if (pos + 2 > buf_size)
    {
        return -1;
    }
    int pa_len_pos = pos;
    pos += 2;
    int pa_body_start = pos;

    int n = encode_origin(buf + pos, buf_size - pos, attr->origin);
    if (n < 0)
    {
        return -1;
    }
    pos += n;

    n = encode_as_path(buf + pos, buf_size - pos, attr->as_path);
    if (n < 0)
    {
        return -1;
    }
    pos += n;

    /* AF 相关 PA（Packed） */
    int pa_packed = 0;
    n = enc->encode_reach_pa_packed(buf + pos, buf_size - pos, nlri_list, nlri_count, nexthop, &pa_packed);
    if (n < 0 || pa_packed <= 0)
    {
        return -1;
    }
    pos += n;

    if (attr->has_local_pref)
    {
        n = encode_local_pref(buf + pos, buf_size - pos, attr->local_pref);
        if (n < 0)
        {
            return -1;
        }
        pos += n;
    }

    if (attr->has_med)
    {
        n = encode_med(buf + pos, buf_size - pos, attr->med);
        if (n < 0)
        {
            return -1;
        }
        pos += n;
    }

    n = encode_community(buf + pos, buf_size - pos, attr->communities);
    if (n < 0)
    {
        return -1;
    }
    pos += n;

    uint16_t pa_total_be = htons((uint16_t)(pos - pa_body_start));
    memcpy(buf + pa_len_pos, &pa_total_be, 2);

    /* NLRI 段（IPv4-trad 时打包前缀；MP_REACH 时返回 0） */
    int nlri_packed = 0;
    n = enc->encode_reach_nlri_packed(buf + pos, buf_size - pos, nlri_list, pa_packed, nexthop, &nlri_packed);
    if (n < 0)
    {
        return -1;
    }
    pos += n;

    int packed = (pa_packed < nlri_packed) ? pa_packed : nlri_packed;
    if (packed <= 0)
    {
        return -1;
    }

    /* BGP header */
    uint16_t total_len = (uint16_t)pos;
    memcpy(buf, BGP_MARKER, 16);
    uint16_t len_be = htons(total_len);
    memcpy(buf + 16, &len_be, 2);
    buf[18] = (uint8_t)BGP_MSG_UPDATE;

    if (out_packed_count)
    {
        *out_packed_count = packed;
    }
    return (int)total_len;
}

int bgp_pkt_build_packed_withdraw(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                  const bgp_conn_t *conn, uint16_t afi, uint8_t safi, int *out_packed_count)
{
    if (out_packed_count)
    {
        *out_packed_count = 0;
    }
    if (!buf || buf_size < BGP_MSG_HEADER_SIZE || !nlri_list || nlri_count <= 0 || !conn)
    {
        return -1;
    }

    const bgp_pkt_af_enc_t *enc = bgp_pkt_af_enc_find(afi, safi);
    if (!enc || !enc->encode_unreach_wd_packed || !enc->encode_unreach_pa_packed)
    {
        return -1;
    }

    int pos = BGP_MSG_HEADER_SIZE;

    /* Withdrawn Routes 段 */
    if (pos + 2 > buf_size)
    {
        return -1;
    }
    int wd_len_pos = pos;
    pos += 2;
    int wd_body_start = pos;

    int wd_packed = 0;
    int n = enc->encode_unreach_wd_packed(buf + pos, buf_size - pos, nlri_list, nlri_count, conn, &wd_packed);
    if (n < 0)
    {
        return -1;
    }
    pos += n;

    uint16_t wd_len_be = htons((uint16_t)(pos - wd_body_start));
    memcpy(buf + wd_len_pos, &wd_len_be, 2);

    /* PA 段（MP_UNREACH） */
    if (pos + 2 > buf_size)
    {
        return -1;
    }
    int pa_len_pos = pos;
    pos += 2;
    int pa_body_start = pos;

    int pa_packed = 0;
    /* MP_UNREACH 场景只需打包 wd_packed 之内的条目（wd 段为空时打包完整列表） */
    int pa_input_count = (n == 0) ? nlri_count : wd_packed;
    n = enc->encode_unreach_pa_packed(buf + pos, buf_size - pos, nlri_list, pa_input_count, conn, &pa_packed);
    if (n < 0)
    {
        return -1;
    }
    pos += n;

    uint16_t pa_total_be = htons((uint16_t)(pos - pa_body_start));
    memcpy(buf + pa_len_pos, &pa_total_be, 2);

    /* wd 和 pa 段只有一个会实际写入前缀：取非零值作为 packed 数 */
    int packed = 0;
    if (wd_packed > 0 && pa_packed > 0)
    {
        packed = (wd_packed < pa_packed) ? wd_packed : pa_packed;
    }
    else if (wd_packed > 0)
    {
        packed = wd_packed;
    }
    else
    {
        packed = pa_packed;
    }
    if (packed <= 0)
    {
        return -1;
    }

    uint16_t total_len = (uint16_t)pos;
    memcpy(buf, BGP_MARKER, 16);
    uint16_t len_be = htons(total_len);
    memcpy(buf + 16, &len_be, 2);
    buf[18] = (uint8_t)BGP_MSG_UPDATE;

    if (out_packed_count)
    {
        *out_packed_count = packed;
    }
    return (int)total_len;
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
        LOG_ERROR("BGP: peer %s OPEN parse failed", _ip);
        return -1;
    }

    /* 优先使用 4 字节 AS（RFC 6793 AS_TRANS 处理） */
    uint32_t peer_claimed_as = msg.cap_as4 ? msg.cap_as4 : msg.my_as;

    /* RFC 4271 §6.2：对端 OPEN 携带的 AS 必须与本端配置的 remote-as 一致，
     * 否则发送 NOTIFICATION OPEN/BAD_PEER_AS 并断链；严禁覆盖 sess->remote_as。 */
    if (conn->session->remote_as != 0 && peer_claimed_as != conn->session->remote_as)
    {
        LOG_ERROR("BGP: peer %s OPEN AS mismatch: configured=%u received=%u, sending NOTIFICATION Bad Peer AS", _ip,
                  conn->session->remote_as, peer_claimed_as);
        bgp_pkt_send_notification(conn, BGP_ERR_OPEN, BGP_OPEN_ERR_BAD_PEER_AS);
        return -1;
    }

    /* 将点分十进制 BGP Identifier 转为主机序 uint32_t 存储 */
    struct in_addr _rid_tmp;
    if (inet_pton(AF_INET, msg.bgp_id, &_rid_tmp) == 1)
    {
        conn->session->remote_id = ntohl(_rid_tmp.s_addr);
    }
    else
    {
        conn->session->remote_id = 0;
    }

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
    if (msg.cap_ext_nexthop)
    {
        BIT_SET(remote_caps, BGP_SESS_CAP_EXT_NEXTHOP);
    }
    conn->session->remote_caps = remote_caps;
    conn->session->negotiated_caps = conn->session->local_caps & remote_caps;

    /* 记录并协商 Hold Time（取本地与远端的较小值，RFC 4271 §4.2） */
    conn->session->remote_hold = msg.hold_time;
    uint16_t local_hold = BGP_HOLD_TIME;
    if (conn->session->vrf && conn->session->vrf->hold_time > 0)
    {
        local_hold = conn->session->vrf->hold_time;
    }
    conn->session->negotiated_hold = (msg.hold_time < local_hold) ? msg.hold_time : local_hold;

    /* 将 remote_id 转回字符串仅用于日志 */
    char _remote_rid_str[16];
    struct in_addr _remote_rid_addr;
    _remote_rid_addr.s_addr = htonl(conn->session->remote_id);
    inet_ntop(AF_INET, &_remote_rid_addr, _remote_rid_str, sizeof(_remote_rid_str));
    LOG_INFO("BGP: Received OPEN from %s (AS=%u, ID=%s, hold=%u, caps=0x%02X)", _ip, conn->session->remote_as,
             _remote_rid_str, msg.hold_time, remote_caps);

    /* 将 MP 能力写入 negotiated_afs（以 afi<<16|safi 打包为 guint32，无堆分配） */
    if (conn->session->negotiated_afs)
    {
        g_array_free(conn->session->negotiated_afs, TRUE);
        conn->session->negotiated_afs = NULL;
    }
    conn->session->negotiated_afs = g_array_new(FALSE, FALSE, sizeof(guint32));
    for (uint8_t i = 0; i < msg.mp_count; i++)
    {
        guint32 packed = ((guint32)msg.mp_afs[i] << 16) | (guint32)msg.mp_safis[i];
        g_array_append_val(conn->session->negotiated_afs, packed);
        LOG_INFO("BGP: peer %s MP capability: AFI=%u SAFI=%u", _ip, msg.mp_afs[i], msg.mp_safis[i]);
    }

    return 0;
}

static ssize_t bgp_pkt_peek_bytes(int fd, uint8_t *buf, size_t len)
{
    for (;;)
    {
        ssize_t n = recv(fd, buf, len, MSG_PEEK);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        return n;
    }
}

static ssize_t bgp_pkt_read_exact(int fd, uint8_t *buf, size_t len)
{
    size_t offset = 0;
    while (offset < len)
    {
        ssize_t n = recv(fd, buf + offset, len - offset, 0);
        if (n == 0)
        {
            return 0;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        offset += (size_t)n;
    }
    return (ssize_t)offset;
}

static void bgp_pkt_set_primary_conn(bgp_session_t *sess, bgp_conn_t *winner)
{
    if (!sess || !winner || sess->pri_conn == winner)
    {
        return;
    }

    if (sess->sec_conn == winner)
    {
        bgp_conn_t *other = sess->pri_conn;
        sess->pri_conn = winner;
        sess->sec_conn = other;
    }
    else if (!sess->pri_conn)
    {
        sess->pri_conn = winner;
        if (sess->sec_conn == winner)
        {
            sess->sec_conn = NULL;
        }
    }

    sess->pri_last_socket_error = sess->pri_conn ? sess->pri_conn->last_socket_error : 0;
    sess->sec_last_socket_error = sess->sec_conn ? sess->sec_conn->last_socket_error : 0;

    if (sess->fsm_state == BGP_FSM_STATE_CONNECT && sess->pri_conn && !sess->pri_conn->is_connecting)
    {
        sess->fsm_state = BGP_FSM_STATE_OPEN_SENT;
    }
}

/**
 * @brief sec_conn 出错或收到非 OPEN 报文时，直接关闭 sec_conn（不走 FSM）
 */
static void pkt_close_sec(bgp_session_t *sess)
{
    bgp_conn_close(sess, &sess->sec_conn, g_bgp_work_local->epoll_fd);
}

/**
 * @brief 连接出错时的统一处理：sec_conn 直接关闭，pri_conn 走 FSM
 */
static void pkt_conn_error(bgp_session_t *sess, bgp_conn_t *conn, bgp_fsm_event_t evt)
{
    if (conn == sess->sec_conn)
    {
        pkt_close_sec(sess);
    }
    else
    {
        bgp_fsm_event(sess, evt);
    }
}

void bgp_pkt_on_data(bgp_conn_t *conn)
{
    uint8_t header[BGP_MSG_HEADER_SIZE];
    uint8_t frame[BGP_RECV_BUF_SIZE];

    if (!conn || !conn->session)
    {
        return;
    }

    char _ip[64];
    net_addr_to_str(&conn->peer_addr, _ip, sizeof(_ip));

    bgp_session_t *sess = conn->session;

    for (;;)
    {
        ssize_t n = bgp_pkt_peek_bytes(conn->fd, header, sizeof(header));
        if (n == 0)
        {
            LOG_INFO("BGP: peer %s closed connection", _ip);
            pkt_conn_error(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS);
            return;
        }
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            LOG_PERROR("BGP: recv from %s failed", _ip);
            pkt_conn_error(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS);
            return;
        }
        if ((size_t)n < sizeof(header))
        {
            return;
        }

        if (memcmp(header, BGP_MARKER, 16) != 0)
        {
            LOG_ERROR("BGP: peer %s Marker validation failed, closing connection", _ip);
            pkt_conn_error(sess, conn, BGP_EVT_BGP_HEADER_ERR);
            return;
        }

        uint16_t msg_len_be;
        memcpy(&msg_len_be, header + 16, 2);
        uint16_t msg_len = ntohs(msg_len_be);

        if (msg_len < BGP_MSG_HEADER_SIZE || msg_len > BGP_RECV_BUF_SIZE)
        {
            LOG_ERROR("BGP: peer %s message length %u invalid", _ip, msg_len);
            pkt_conn_error(sess, conn, BGP_EVT_BGP_HEADER_ERR);
            return;
        }

        n = bgp_pkt_peek_bytes(conn->fd, frame, msg_len);
        if (n == 0)
        {
            LOG_INFO("BGP: peer %s closed connection", _ip);
            pkt_conn_error(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS);
            return;
        }
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            LOG_PERROR("BGP: recv from %s failed", _ip);
            pkt_conn_error(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS);
            return;
        }
        if (n < msg_len)
        {
            return;
        }

        n = bgp_pkt_read_exact(conn->fd, frame, msg_len);
        if (n == 0)
        {
            LOG_INFO("BGP: peer %s closed connection", _ip);
            pkt_conn_error(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS);
            return;
        }
        if (n < 0)
        {
            LOG_PERROR("BGP: recv from %s failed", _ip);
            pkt_conn_error(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS);
            return;
        }
        if (n != msg_len)
        {
            LOG_ERROR("BGP: peer %s short read: expected %u got %zd", _ip, msg_len, n);
            pkt_conn_error(sess, conn, BGP_EVT_TCP_CONNECTION_FAILS);
            return;
        }

        uint8_t msg_type = frame[18];
        const uint8_t *body = frame + BGP_MSG_HEADER_SIZE;
        uint16_t body_len = msg_len - BGP_MSG_HEADER_SIZE;

        /* 按报文类型累计本 session 收包统计（含未知类型） */
        bgp_session_rx_msg_count(sess, msg_type);

        switch (msg_type)
        {
            case BGP_MSG_OPEN:
            {
                if (parse_bgp_open(conn, body, body_len) < 0)
                {
                    pkt_conn_error(sess, conn, BGP_EVT_BGP_OPEN_MSG_ERR);
                    return;
                }

                /* §6.8 碰撞检测：两条连接同时存在时比较 BGP Router-ID */
                if (sess->pri_conn && sess->sec_conn)
                {
                    uint32_t local_id_n = sess->local_router_id;
                    uint32_t remote_id_n = sess->remote_id;
                    char _col_local_str[16], _col_remote_str[16];
                    struct in_addr _col_tmp;

                    _col_tmp.s_addr = htonl(local_id_n);
                    inet_ntop(AF_INET, &_col_tmp, _col_local_str, sizeof(_col_local_str));
                    _col_tmp.s_addr = htonl(remote_id_n);
                    inet_ntop(AF_INET, &_col_tmp, _col_remote_str, sizeof(_col_remote_str));

                    LOG_INFO("BGP: %s §6.8 collision: local-id=%s(%u) remote-id=%s(%u), this conn is %s", _ip,
                             _col_local_str, local_id_n, _col_remote_str, remote_id_n,
                             conn->is_active ? "active" : "passive");

                    if (local_id_n == remote_id_n)
                    {
                        LOG_WARN("BGP: %s §6.8 collision: BGP ID equal, closing current connection", _ip);
                        if (conn == sess->sec_conn)
                        {
                            pkt_close_sec(sess);
                        }
                        else
                        {
                            bgp_fsm_event(sess, BGP_EVT_OPEN_COLLISION_DUMP);
                        }
                        return;
                    }

                    gboolean shall_close_active = (local_id_n < remote_id_n);
                    gboolean close_me =
                        (shall_close_active && conn->is_active) || (!shall_close_active && !conn->is_active);

                    if (close_me)
                    {
                        LOG_INFO("BGP: %s §6.8 collision: closing this %s connection", _ip,
                                 conn->is_active ? "active" : "passive");
                        if (conn == sess->sec_conn)
                        {
                            pkt_close_sec(sess);
                        }
                        else
                        {
                            bgp_fsm_event(sess, BGP_EVT_OPEN_COLLISION_DUMP);
                        }
                        return;
                    }

                    /* 当前连接胜出：提升为 pri_conn，关闭败方 */
                    LOG_INFO("BGP: %s §6.8 collision: keeping this %s connection, closing other", _ip,
                             conn->is_active ? "active" : "passive");
                    bgp_pkt_set_primary_conn(sess, conn);
                    bgp_conn_close(sess, &sess->sec_conn, g_bgp_work_local->epoll_fd);
                }
                else
                {
                    bgp_pkt_set_primary_conn(sess, conn);
                }

                /* conn 现在一定是 sess->pri_conn */
                if (bgp_pkt_send_keepalive(sess->pri_conn) < 0)
                {
                    bgp_fsm_event(sess, BGP_EVT_TCP_CONNECTION_FAILS);
                    return;
                }

                bgp_fsm_event(sess, BGP_EVT_BGP_OPEN);
                break;
            }

            case BGP_MSG_KEEPALIVE:
            {
                /* sec_conn 收到 KEEPALIVE：不应发生，直接关闭 */
                if (conn != sess->pri_conn)
                {
                    LOG_WARN("BGP: peer %s KEEPALIVE received on secondary connection, closing it", _ip);
                    pkt_close_sec(sess);
                    return;
                }

                if (sess->fsm_state == BGP_FSM_STATE_OPEN_CONFIRM)
                {
                    LOG_INFO("BGP: Session established with %s (AS%u)", _ip, sess->remote_as);
                    bgp_fsm_event(sess, BGP_EVT_KEEPALIVE_MSG);
                }
                else if (sess->fsm_state == BGP_FSM_STATE_ESTABLISHED)
                {
                    LOG_DEBUG("BGP: Received %s KEEPALIVE", _ip);
                    bgp_session_reset_hold(sess);
                }
                else
                {
                    LOG_WARN("BGP: peer %s unexpected KEEPALIVE while session in %s", _ip,
                             bgp_fsm_state_str(sess->fsm_state));
                    bgp_fsm_event(sess, BGP_EVT_KEEPALIVE_MSG);
                    return;
                }
                break;
            }

            case BGP_MSG_UPDATE:
            {
                /* sec_conn 或非 ESTABLISHED 状态收到 UPDATE：异常 */
                if (conn != sess->pri_conn || sess->fsm_state != BGP_FSM_STATE_ESTABLISHED)
                {
                    LOG_WARN("BGP: peer %s unexpected UPDATE while session in %s", _ip,
                             bgp_fsm_state_str(sess->fsm_state));
                    pkt_conn_error(sess, conn, BGP_EVT_UPDATE_MSG);
                    return;
                }

                bgp_update_result_t *upd = NULL;
                uint32_t parse_flags = BGP_PARSE_FLAG_AS4;
                if (BIT_TEST(sess->negotiated_caps, BGP_SESS_CAP_EXT_NEXTHOP))
                {
                    parse_flags |= BGP_PARSE_FLAG_EXT_NEXTHOP;
                }
                if (bgp_update_parse(body, body_len, parse_flags, &upd) == 0 && upd)
                {
                    bgp_peer_update_ingest_stats_t ingest_stats = {0};
                    bgp_worker_ingest_peer_update(sess, upd, &ingest_stats);

                    LOG_INFO("BGP: %s UPDATE: afi=%u safi=%u +%u -%u | route-iter reach(ok=%u fail=%u) "
                             "unreach(ok=%u fail=%u) heads=%u routes=%u",
                             _ip, upd->afi, upd->safi, upd->reach_len, upd->unreach_len, ingest_stats.reach_injected,
                             ingest_stats.reach_failed, ingest_stats.unreach_injected, ingest_stats.unreach_failed,
                             bgp_vrf_rib_head_count(sess->vrf), bgp_vrf_rib_route_count(sess->vrf));
                    bgp_update_result_free(upd);
                }
                else
                {
                    LOG_WARN("BGP: %s UPDATE parse failed", _ip);
                    bgp_fsm_event(sess, BGP_EVT_UPDATE_MSG_ERR);
                    return;
                }

                bgp_bmp_thread_notify_route_monitoring(sess, frame, msg_len);
                bgp_session_reset_hold(sess);
                break;
            }

            case BGP_MSG_NOTIFICATION:
            {
                bgp_notif_msg_t notif;
                bgp_fsm_event_t close_evt = BGP_EVT_NOTIF_MSG;
                if (bgp_notif_parse(body, body_len, &notif) == 0)
                {
                    LOG_WARN("BGP: Received %s NOTIFICATION: %s (code=%u sub=%u)", _ip, notif.error_str,
                             notif.error_code, notif.error_subcode);
                    if (notif.error_code == BGP_ERR_OPEN && notif.error_subcode == BGP_OPEN_ERR_UNSUPPORTED_VERSION)
                    {
                        close_evt = BGP_EVT_NOTIF_MSG_VER_ERR;
                    }
                }
                else
                {
                    LOG_WARN("BGP: Received %s NOTIFICATION message, closing session", _ip);
                }
                pkt_conn_error(sess, conn, close_evt);
                return;
            }

            default:
                LOG_WARN("BGP: peer %s unknown message type %u, closing connection", _ip, msg_type);
                pkt_conn_error(sess, conn, BGP_EVT_BGP_HEADER_ERR);
                return;
        }
    }
}
