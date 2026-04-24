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

static void write_ipv4_mapped_ipv6(uint8_t *dst, const struct in_addr *v4)
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (!dst || !v4)
    {
        return;
    }
    memcpy(dst, prefix, sizeof(prefix));
    memcpy(dst + sizeof(prefix), &v4->s_addr, sizeof(v4->s_addr));
}

/**
 * @brief 编码 MP_REACH_NLRI 路径属性（RFC 4760）
 *
 * 格式：AFI(2) + SAFI(1) + NHLen(1) + NH(16 or 32) + SNPA(1) + 前缀(1+n)
 * 对 IPv4 nexthop，线上按 6PE 形式编码为 IPv4-mapped IPv6（16B）。
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
        nh_len = 16;
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
        write_ipv4_mapped_ipv6(buf + pos, &nexthop->global.u.v4);
        pos += 16;
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

// ============================================================================
// Packed 版本（Phase 3）
// ============================================================================

/**
 * @brief Packed 宣告 PA：MP_REACH_NLRI 嵌入多条 IPv6 前缀
 */
static int ipv6uc_encode_reach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                         int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!nexthop || nexthop->global.family == 0)
    {
        return -1;
    }

    uint8_t nh_len = 0;
    if (nexthop->global.family == AF_INET)
    {
        nh_len = 16;
    }
    else if (nexthop->global.family == AF_INET6)
    {
        nh_len = nexthop->has_link_local ? 32 : 16;
    }
    else
    {
        return -1;
    }

    int header_len = 3 + 2 + 1 + 1 + (int)nh_len + 1;
    if (buf_size < header_len)
    {
        return -1;
    }

    int pos = 3;
    uint16_t afi_be = htons(BGP_AFI_IPV6);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_UNICAST;
    buf[pos++] = nh_len;
    if (nexthop->global.family == AF_INET)
    {
        write_ipv4_mapped_ipv6(buf + pos, &nexthop->global.u.v4);
        pos += 16;
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
    buf[pos++] = 0; /* SNPA count */

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!nlri || nlri->type != BGP_NLRI_PREFIX)
        {
            continue;
        }
        uint8_t plen = nlri->prefix.prefix.prefix_len;
        int pfx_len = 1 + (plen + 7) / 8;
        if ((pos - 3) + pfx_len > 255)
        {
            break;
        }
        int n = bgp_pkt_encode_prefix(buf + pos, buf_size - pos, &nlri->prefix.prefix);
        if (n < 0)
        {
            break;
        }
        pos += n;
        packed++;
    }
    if (packed == 0)
    {
        return -1;
    }

    int value_len = pos - 3;
    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_REACH;
    buf[2] = (uint8_t)value_len;
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

/**
 * @brief Packed NLRI：IPv6 UC 的 NLRI 已嵌入 MP_REACH，NLRI 段为空
 */
static int ipv6uc_encode_reach_nlri_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    (void)buf;
    (void)buf_size;
    (void)nlri_list;
    (void)nexthop;
    if (out_packed)
    {
        *out_packed = nlri_count;
    }
    return 0;
}

/**
 * @brief Packed 撤销 Withdrawn：IPv6 UC 不使用 Withdrawn 段
 */
static int ipv6uc_encode_unreach_wd_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)buf;
    (void)buf_size;
    (void)nlri_list;
    (void)conn;
    if (out_packed)
    {
        *out_packed = nlri_count;
    }
    return 0;
}

/**
 * @brief Packed 撤销 PA：MP_UNREACH_NLRI 嵌入多条 IPv6 前缀
 */
static int ipv6uc_encode_unreach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                           int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)conn;
    if (out_packed)
    {
        *out_packed = 0;
    }

    int header_len = 3 + 2 + 1;
    if (buf_size < header_len)
    {
        return -1;
    }
    int pos = 3;
    uint16_t afi_be = htons(BGP_AFI_IPV6);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_UNICAST;

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!nlri || nlri->type != BGP_NLRI_PREFIX)
        {
            continue;
        }
        uint8_t plen = nlri->prefix.prefix.prefix_len;
        int pfx_len = 1 + (plen + 7) / 8;
        if ((pos - 3) + pfx_len > 255)
        {
            break;
        }
        int n = bgp_pkt_encode_prefix(buf + pos, buf_size - pos, &nlri->prefix.prefix);
        if (n < 0)
        {
            break;
        }
        pos += n;
        packed++;
    }
    if (packed == 0)
    {
        return -1;
    }

    int value_len = pos - 3;
    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_UNREACH;
    buf[2] = (uint8_t)value_len;
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

/** IPv6 单播编码器描述符 */
static const bgp_pkt_af_enc_t g_ipv6uc_enc = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_UNICAST,
    .encode_reach_pa = ipv6uc_encode_reach_pa,
    .encode_reach_nlri = ipv6uc_encode_reach_nlri,
    .encode_unreach_wd = ipv6uc_encode_unreach_wd,
    .encode_unreach_pa = ipv6uc_encode_unreach_pa,
    .encode_reach_pa_packed = ipv6uc_encode_reach_pa_packed,
    .encode_reach_nlri_packed = ipv6uc_encode_reach_nlri_packed,
    .encode_unreach_wd_packed = ipv6uc_encode_unreach_wd_packed,
    .encode_unreach_pa_packed = ipv6uc_encode_unreach_pa_packed,
};

void bgp_pkt_build_ipv6uc_register(void)
{
    bgp_pkt_af_enc_register(&g_ipv6uc_enc);
}
