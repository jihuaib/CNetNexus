/**
 * @file   bgp_pkt_build_ipv4uc.c
 * @brief  BGP UPDATE 报文 IPv4 单播（AFI=1, SAFI=1）AF 编码器
 *         支持传统 NEXT_HOP + NLRI（IPv4 nexthop）和
 *         MP_REACH_NLRI / MP_UNREACH_NLRI（IPv6 nexthop, RFC 8950）
 * @author jhb
 * @date   2026/03/15
 */
#include <arpa/inet.h>
#include <string.h>

#include "bgp_pkt_build.h"
#include "bgp_session.h"

// ============================================================================
// 辅助判断
// ============================================================================

/** 判断是否为双栈场景：IPv4 前缀但 nexthop 为 IPv6（RFC 8950） */
static gboolean is_ipv6_nexthop(const bgp_nexthop_t *nexthop)
{
    return nexthop && nexthop->global.family == AF_INET6;
}

/** 判断是否需要使用 MP_UNREACH（IPv6 peer 且已协商 Extended Next Hop） */
static gboolean need_mp_unreach(const bgp_conn_t *conn)
{
    return conn && conn->peer_addr.family == AF_INET6 && conn->session &&
           BIT_TEST(conn->session->flags, BGP_SESS_CAP_EXT_NEXTHOP);
}

// ============================================================================
// IPv4 单播编码器实现
// ============================================================================

/**
 * @brief 编码传统 NEXT_HOP 路径属性（IPv4，4 字节）
 */
static int encode_nexthop_attr(uint8_t *buf, int buf_size, const net_addr_t *nh)
{
    if (nh->family != AF_INET)
    {
        return 0;
    }
    if (buf_size < 7)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_TRANSITIVE;
    buf[1] = BGP_PA_TYPE_NEXT_HOP;
    buf[2] = 4;
    memcpy(buf + 3, &nh->u.v4, 4);
    return 7;
}

/**
 * @brief 编码 MP_REACH_NLRI 路径属性（IPv4 前缀 + IPv6 nexthop, RFC 8950）
 *
 * 格式：AFI(2) + SAFI(1) + NHLen(1) + NH(16 or 32) + SNPA(1) + 前缀(1+n)
 */
static int encode_mp_reach_ipv4_v6nh(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                     const bgp_nexthop_t *nexthop)
{
    const net_prefix_t *pfx = &nlri->prefix.prefix;
    uint8_t plen = pfx->prefix_len;
    int pfx_bytes = 1 + (plen + 7) / 8;
    uint8_t nh_len = nexthop->has_link_local ? 32 : 16;
    /* 属性值长度：AFI(2)+SAFI(1)+NHLen(1)+NH(nh_len)+SNPA(1)+NLRI(pfx_bytes) */
    int value_len = 2 + 1 + 1 + (int)nh_len + 1 + pfx_bytes;
    int attr_total = 3 + value_len;
    if (buf_size < attr_total)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_REACH;
    buf[2] = (uint8_t)value_len;
    int pos = 3;
    uint16_t afi_be = htons(nlri->afi);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = nlri->safi;
    buf[pos++] = nh_len;
    memcpy(buf + pos, &nexthop->global.u.v6, 16);
    pos += 16;
    if (nexthop->has_link_local)
    {
        memcpy(buf + pos, &nexthop->link_local.u.v6, 16);
        pos += 16;
    }
    buf[pos++] = 0; /* SNPA count = 0 */
    /* IPv4 前缀 */
    buf[pos++] = plen;
    int nbytes = (plen + 7) / 8;
    memcpy(buf + pos, &pfx->addr.u.v4, nbytes);
    pos += nbytes;
    return pos;
}

/**
 * @brief 编码 MP_UNREACH_NLRI 路径属性（IPv4 前缀, RFC 8950）
 *
 * 格式：AFI(2) + SAFI(1) + 前缀(1+n)
 */
static int encode_mp_unreach_ipv4(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    const net_prefix_t *pfx = &nlri->prefix.prefix;
    uint8_t plen = pfx->prefix_len;
    int pfx_bytes = 1 + (plen + 7) / 8;
    /* 属性值长度：AFI(2)+SAFI(1)+NLRI(pfx_bytes) */
    int value_len = 2 + 1 + pfx_bytes;
    int attr_total = 3 + value_len;
    if (buf_size < attr_total)
    {
        return -1;
    }
    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_UNREACH;
    buf[2] = (uint8_t)value_len;
    int pos = 3;
    uint16_t afi_be = htons(nlri->afi);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = nlri->safi;
    buf[pos++] = plen;
    int nbytes = (plen + 7) / 8;
    memcpy(buf + pos, &pfx->addr.u.v4, nbytes);
    pos += nbytes;
    return pos;
}

/**
 * @brief 宣告 PA：IPv4 nexthop 用传统 NEXT_HOP，IPv6 nexthop 用 MP_REACH_NLRI
 */
static int ipv4uc_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                  const bgp_nexthop_t *nexthop)
{
    if (is_ipv6_nexthop(nexthop))
    {
        return encode_mp_reach_ipv4_v6nh(buf, buf_size, nlri, nexthop);
    }
    return encode_nexthop_attr(buf, buf_size, &nexthop->global);
}

/**
 * @brief 宣告 NLRI：传统方式写入 NLRI 字段；双栈（IPv6 nexthop）时返回 0（已在 MP_REACH 中携带）
 */
static int ipv4uc_encode_reach_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                    const bgp_nexthop_t *nexthop)
{
    if (is_ipv6_nexthop(nexthop))
    {
        return 0;
    }
    return bgp_pkt_encode_prefix(buf, buf_size, &nlri->prefix.prefix);
}

/**
 * @brief 撤销 Withdrawn Routes：传统方式写入前缀；IPv6 peer 时返回 0（通过 MP_UNREACH 携带）
 */
static int ipv4uc_encode_unreach_wd(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    if (need_mp_unreach(conn))
    {
        return 0;
    }
    return bgp_pkt_encode_prefix(buf, buf_size, &nlri->prefix.prefix);
}

/**
 * @brief 撤销 PA：IPv6 peer（已协商 EXT_NH）使用 MP_UNREACH_NLRI（RFC 8950）；否则返回 0
 */
static int ipv4uc_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    if (need_mp_unreach(conn))
    {
        return encode_mp_unreach_ipv4(buf, buf_size, nlri);
    }
    return 0;
}

/** IPv4 单播编码器描述符 */
static const bgp_pkt_af_enc_t g_ipv4uc_enc = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_UNICAST,
    .encode_reach_pa = ipv4uc_encode_reach_pa,
    .encode_reach_nlri = ipv4uc_encode_reach_nlri,
    .encode_unreach_wd = ipv4uc_encode_unreach_wd,
    .encode_unreach_pa = ipv4uc_encode_unreach_pa,
};

void bgp_pkt_build_ipv4uc_register(void)
{
    bgp_pkt_af_enc_register(&g_ipv4uc_enc);
}
