/**
 * @file   bgp_pkt_build_vpn.c
 * @brief  BGP UPDATE 报文编码器：VPNv4 / VPNv6 Unicast
 * @author jhb
 * @date   2026/06/01
 *
 * 线格式（RFC 4364，与 lib/bgp_parse_vpn.c 对称）：
 *   NLRI：Length(1B,bits) = label(24) + rd(64) + ip_prefix_bits
 *         Label Stack Entry(3B)：label(20b)+Exp(3b)+BoS(1b)
 *         Route Distinguisher(8B)
 *         IP 前缀(ceil(ip_bits/8) B)
 *   MP_REACH nexthop：
 *     VPNv4 + IPv4 NH: RD(8B,全 0) + IPv4(4B) = 12B
 *     VPNv4/v6 + IPv6 NH: RD(8B,全 0) + IPv6(16B) = 24B
 *     可选 link-local: 再跟 RD(8B,全 0) + IPv6(16B) = 48B
 */
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

#include "bgp_conn.h"
#include "bgp_pkt_build.h"

/** VPN 前缀固定开销 bit 数：label(24) + RD(64) */
#define VPN_FIXED_BITS 88u
/** VPN MP_REACH nexthop 长度 */
#define VPN_IPV4_NH_LEN 12u
#define VPN_IPV6_NH_LEN 24u
#define VPN_IPV6_DUAL_NH_LEN 48u

static int vpn_nlri_valid(const bgp_nlri_entry_t *nlri, uint16_t afi)
{
    /* label 非必须：reach 通告在发送时注入标签；withdraw 的 loc-rib NLRI 不带标签，
     * 编码 label=0 即可（接收方按 RD+前缀匹配撤销，忽略 withdraw 标签）。 */
    if (!nlri || nlri->type != BGP_NLRI_PREFIX || nlri->afi != afi || nlri->safi != BGP_SAFI_VPN_UNICAST ||
        !nlri->prefix.has_rd || nlri->prefix.label > 0xFFFFFu)
    {
        return 0;
    }
    if (afi == BGP_AFI_IPV4)
    {
        return nlri->prefix.prefix.addr.family == AF_INET && nlri->prefix.prefix.prefix_len <= 32u;
    }
    if (afi == BGP_AFI_IPV6)
    {
        return nlri->prefix.prefix.addr.family == AF_INET6 && nlri->prefix.prefix.prefix_len <= 128u;
    }
    return 0;
}

/**
 * @brief 编码单条 VPN NLRI：length(1B) + label(3B) + RD(8B) + prefix
 * @return 写入字节数，-1=空间不足/非法
 */
static int encode_vpn_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, uint16_t afi)
{
    if (!buf || !vpn_nlri_valid(nlri, afi))
    {
        return -1;
    }

    uint8_t plen = nlri->prefix.prefix.prefix_len;
    int nbytes = (plen + 7) / 8;
    /* 1B length + 3B label + 8B RD + nbytes 前缀 */
    if (buf_size < 1 + 3 + 8 + nbytes)
    {
        return -1;
    }

    uint32_t label = nlri->prefix.label & 0xFFFFFu;
    buf[0] = (uint8_t)(VPN_FIXED_BITS + plen); /* total bits */
    buf[1] = (uint8_t)(label >> 12);
    buf[2] = (uint8_t)(label >> 4);
    buf[3] = (uint8_t)(((label & 0xFu) << 4) | 0x01u); /* Exp=0, BoS=1 */
    memcpy(buf + 4, nlri->prefix.rd.bytes, 8);
    const uint8_t *prefix_bytes =
        (afi == BGP_AFI_IPV6) ? nlri->prefix.prefix.addr.u.v6.s6_addr : (const uint8_t *)&nlri->prefix.prefix.addr.u.v4;
    memcpy(buf + 12, prefix_bytes, nbytes);
    return 1 + 3 + 8 + nbytes;
}

static int encode_mp_reach_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                  const bgp_nexthop_t *nexthop, uint16_t afi, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!buf || !nlri_list || nlri_count <= 0 || !nexthop ||
        (nexthop->global.family != AF_INET && nexthop->global.family != AF_INET6) ||
        (afi == BGP_AFI_IPV6 && nexthop->global.family != AF_INET6) || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return -1;
    }

    uint8_t nh_len;
    if (nexthop->global.family == AF_INET)
    {
        nh_len = VPN_IPV4_NH_LEN;
    }
    else
    {
        if (nexthop->has_link_local && nexthop->link_local.family != AF_INET6)
        {
            return -1;
        }
        nh_len = nexthop->has_link_local ? VPN_IPV6_DUAL_NH_LEN : VPN_IPV6_NH_LEN;
    }

    /* PA 头(3) + AFI(2) + SAFI(1) + NHLen(1) + NH + SNPA(1) */
    int header_len = 3 + 2 + 1 + 1 + (int)nh_len + 1;
    if (buf_size < header_len)
    {
        return -1;
    }

    int pos = 3;
    uint16_t afi_be = htons(afi);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_VPN_UNICAST;
    buf[pos++] = nh_len;
    memset(buf + pos, 0, 8); /* nexthop RD 全 0 */
    pos += 8;
    if (nexthop->global.family == AF_INET)
    {
        memcpy(buf + pos, &nexthop->global.u.v4, 4);
        pos += 4;
    }
    else
    {
        memcpy(buf + pos, nexthop->global.u.v6.s6_addr, 16);
        pos += 16;
        if (nexthop->has_link_local)
        {
            memset(buf + pos, 0, 8); /* link-local 前置的 RD */
            pos += 8;
            memcpy(buf + pos, nexthop->link_local.u.v6.s6_addr, 16);
            pos += 16;
        }
    }
    buf[pos++] = 0; /* SNPA count */

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!vpn_nlri_valid(nlri, afi))
        {
            continue;
        }
        int n = encode_vpn_nlri(buf + pos, buf_size - pos, nlri, afi);
        if (n < 0 || (pos - 3) + n > 255)
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

    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_REACH;
    buf[2] = (uint8_t)(pos - 3);
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

static int encode_mp_unreach_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                    int nlri_count, uint16_t afi, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!buf || !nlri_list || nlri_count <= 0 || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return -1;
    }

    int header_len = 3 + 2 + 1;
    if (buf_size < header_len)
    {
        return -1;
    }

    int pos = 3;
    uint16_t afi_be = htons(afi);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_VPN_UNICAST;

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!vpn_nlri_valid(nlri, afi))
        {
            continue;
        }
        int n = encode_vpn_nlri(buf + pos, buf_size - pos, nlri, afi);
        if (n < 0 || (pos - 3) + n > 255)
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

    buf[0] = BGP_PA_FLAG_OPTIONAL;
    buf[1] = BGP_PA_TYPE_MP_UNREACH;
    buf[2] = (uint8_t)(pos - 3);
    if (out_packed)
    {
        *out_packed = packed;
    }
    return pos;
}

/* ------------------------------------------------------------------------- */
/* 单条版回调                                                                */
/* ------------------------------------------------------------------------- */

static int vpn4_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_nexthop_t *nexthop)
{
    const bgp_nlri_entry_t *list[1] = {nlri};
    int packed = 0;
    return encode_mp_reach_packed(buf, buf_size, list, 1, nexthop, BGP_AFI_IPV4, &packed);
}

static int vpn6_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_nexthop_t *nexthop)
{
    const bgp_nlri_entry_t *list[1] = {nlri};
    int packed = 0;
    return encode_mp_reach_packed(buf, buf_size, list, 1, nexthop, BGP_AFI_IPV6, &packed);
}

static int vpn_encode_reach_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_nexthop_t *nexthop)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    (void)nexthop;
    return 0; /* NLRI 已随 MP_REACH 携带 */
}

static int vpn_encode_unreach_wd(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)buf;
    (void)buf_size;
    (void)nlri;
    (void)conn;
    return 0; /* 撤销前缀通过 MP_UNREACH_NLRI 携带 */
}

static int vpn4_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)conn;
    const bgp_nlri_entry_t *list[1] = {nlri};
    int packed = 0;
    return encode_mp_unreach_packed(buf, buf_size, list, 1, BGP_AFI_IPV4, &packed);
}

static int vpn6_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)conn;
    const bgp_nlri_entry_t *list[1] = {nlri};
    int packed = 0;
    return encode_mp_unreach_packed(buf, buf_size, list, 1, BGP_AFI_IPV6, &packed);
}

/* ------------------------------------------------------------------------- */
/* 打包版回调                                                                */
/* ------------------------------------------------------------------------- */

static int vpn4_encode_reach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                       int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    return encode_mp_reach_packed(buf, buf_size, nlri_list, nlri_count, nexthop, BGP_AFI_IPV4, out_packed);
}

static int vpn6_encode_reach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                       int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    return encode_mp_reach_packed(buf, buf_size, nlri_list, nlri_count, nexthop, BGP_AFI_IPV6, out_packed);
}

static int vpn_encode_reach_nlri_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
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

static int vpn_encode_unreach_wd_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
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

static int vpn4_encode_unreach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                         int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)conn;
    return encode_mp_unreach_packed(buf, buf_size, nlri_list, nlri_count, BGP_AFI_IPV4, out_packed);
}

static int vpn6_encode_unreach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                         int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)conn;
    return encode_mp_unreach_packed(buf, buf_size, nlri_list, nlri_count, BGP_AFI_IPV6, out_packed);
}

static const bgp_pkt_af_enc_t g_vpn_ipv4_enc = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_VPN_UNICAST,
    .encode_reach_pa = vpn4_encode_reach_pa,
    .encode_reach_nlri = vpn_encode_reach_nlri,
    .encode_unreach_wd = vpn_encode_unreach_wd,
    .encode_unreach_pa = vpn4_encode_unreach_pa,
    .encode_reach_pa_packed = vpn4_encode_reach_pa_packed,
    .encode_reach_nlri_packed = vpn_encode_reach_nlri_packed,
    .encode_unreach_wd_packed = vpn_encode_unreach_wd_packed,
    .encode_unreach_pa_packed = vpn4_encode_unreach_pa_packed,
};

static const bgp_pkt_af_enc_t g_vpn_ipv6_enc = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_VPN_UNICAST,
    .encode_reach_pa = vpn6_encode_reach_pa,
    .encode_reach_nlri = vpn_encode_reach_nlri,
    .encode_unreach_wd = vpn_encode_unreach_wd,
    .encode_unreach_pa = vpn6_encode_unreach_pa,
    .encode_reach_pa_packed = vpn6_encode_reach_pa_packed,
    .encode_reach_nlri_packed = vpn_encode_reach_nlri_packed,
    .encode_unreach_wd_packed = vpn_encode_unreach_wd_packed,
    .encode_unreach_pa_packed = vpn6_encode_unreach_pa_packed,
};

void bgp_pkt_build_vpn_register(void)
{
    bgp_pkt_af_enc_register(&g_vpn_ipv4_enc);
    bgp_pkt_af_enc_register(&g_vpn_ipv6_enc);
}
