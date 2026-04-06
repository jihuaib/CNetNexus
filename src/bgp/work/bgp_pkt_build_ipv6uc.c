/**
 * @file   bgp_pkt_build_ipv6uc.c
 * @brief  BGP UPDATE 报文 IPv6 单播（AFI=2, SAFI=1）AF 编码器
 * @author jhb
 * @date   2026/03/15
 */
#include <arpa/inet.h>
#include <string.h>

#include "bgp_conn.h"
#include "bgp_pkt_build.h"

// ============================================================================
// IPv6 单播编码器实现
// ============================================================================

/**
 * @brief 编码 MP_REACH_NLRI 路径属性（RFC 4760）
 *
 * 格式：AFI(2) + SAFI(1) + NHLen(1) + NH(4 or 16 or 32) + SNPA(1) + 前缀(1+n)
 * nexthop 支持 IPv4(4B) 或 IPv6(16/32B)。
 */
static int encode_mp_reach(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_nexthop_t *nexthop)
{
    if (!nlri || !nexthop || nexthop->global.family == 0)
    {
        return -1;
    }

    const net_prefix_t *pfx = &nlri->prefix.prefix;
    uint8_t plen = pfx->prefix_len;
    int pfx_bytes = 1 + (plen + 7) / 8;

    uint8_t nh_len = 0;
    if (nexthop->global.family == AF_INET)
    {
        nh_len = 4;
    }
    else if (nexthop->global.family == AF_INET6)
    {
        nh_len = nexthop->has_link_local ? 32 : 16;
    }
    else
    {
        return -1;
    }

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

    if (nexthop->global.family == AF_INET)
    {
        memcpy(buf + pos, &nexthop->global.u.v4.s_addr, 4);
        pos += 4;
    }
    else
    {
        memcpy(buf + pos, &nexthop->global.u.v6, 16);
        pos += 16;
        if (nexthop->has_link_local)
        {
            memcpy(buf + pos, &nexthop->link_local.u.v6, 16);
            pos += 16;
        }
    }

    buf[pos++] = 0; /* SNPA count = 0 */
    buf[pos++] = plen;
    int nbytes = (plen + 7) / 8;
    memcpy(buf + pos, &pfx->addr.u.v6, nbytes);
    pos += nbytes;
    return pos;
}

/**
 * @brief 编码 MP_UNREACH_NLRI 路径属性（RFC 4760）
 *
 * 格式：AFI(2) + SAFI(1) + 前缀(1+n)
 */
static int encode_mp_unreach(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
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
    memcpy(buf + pos, &pfx->addr.u.v6, nbytes);
    pos += nbytes;
    return pos;
}

/**
 * @brief 宣告 PA：MP_REACH_NLRI（nexthop + 前缀均编入属性）
 */
static int ipv6uc_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                  const bgp_nexthop_t *nexthop)
{
    return encode_mp_reach(buf, buf_size, nlri, nexthop);
}

/**
 * @brief 宣告 NLRI：前缀已在 MP_REACH_NLRI 中携带，此字段为空
 */
static int ipv6uc_encode_reach_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri,
                                    const bgp_nexthop_t *nexthop)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    (void)nexthop;
    return 0;
}

/**
 * @brief 撤销 Withdrawn Routes：IPv6 不使用此字段
 */
static int ipv6uc_encode_unreach_wd(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    (void)conn;
    return 0;
}

/**
 * @brief 撤销 PA：MP_UNREACH_NLRI
 */
static int ipv6uc_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)conn;
    return encode_mp_unreach(buf, buf_size, nlri);
}

/** IPv6 单播编码器描述符 */
static const bgp_pkt_af_enc_t g_ipv6uc_enc = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_UNICAST,
    .encode_reach_pa = ipv6uc_encode_reach_pa,
    .encode_reach_nlri = ipv6uc_encode_reach_nlri,
    .encode_unreach_wd = ipv6uc_encode_unreach_wd,
    .encode_unreach_pa = ipv6uc_encode_unreach_pa,
};

void bgp_pkt_build_ipv6uc_register(void)
{
    bgp_pkt_af_enc_register(&g_ipv6uc_enc);
}
