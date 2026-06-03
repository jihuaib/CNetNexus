/**
 * @file   bgp_pkt_build_vpn.c
 * @brief  BGP UPDATE 报文编码器：AFI=1 SAFI=128（VPN-IPv4 Unicast，MPLS L3VPN）
 * @author jhb
 * @date   2026/06/01
 *
 * 线格式（RFC 4364，与 lib/bgp_parse_vpn.c 对称）：
 *   NLRI：Length(1B,bits) = label(24) + rd(64) + ip_prefix_bits
 *         Label Stack Entry(3B)：label(20b)+Exp(3b)+BoS(1b)
 *         Route Distinguisher(8B)
 *         IP 前缀(ceil(ip_bits/8) B)
 *   MP_REACH nexthop(VPN-IPv4)：RD(8B,全 0) + IPv4(4B) = 12B
 */
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

#include "bgp_conn.h"
#include "bgp_pkt_build.h"

/** VPN 前缀固定开销 bit 数：label(24) + RD(64) */
#define VPN_FIXED_BITS 88u
/** VPN-IPv4 MP_REACH nexthop 长度：RD(8) + IPv4(4) */
#define VPN_IPV4_NH_LEN 12u

static int vpn_nlri_valid(const bgp_nlri_entry_t *nlri)
{
    /* label 非必须：reach 通告在发送时注入标签；withdraw 的 loc-rib NLRI 不带标签，
     * 编码 label=0 即可（接收方按 RD+前缀匹配撤销，忽略 withdraw 标签）。 */
    if (!nlri || nlri->type != BGP_NLRI_PREFIX || nlri->afi != BGP_AFI_IPV4 || nlri->safi != BGP_SAFI_VPN_UNICAST ||
        !nlri->prefix.has_rd || nlri->prefix.label > 0xFFFFFu)
    {
        return 0;
    }
    return nlri->prefix.prefix.addr.family == AF_INET && nlri->prefix.prefix.prefix_len <= 32u;
}

/**
 * @brief 编码单条 VPN NLRI：length(1B) + label(3B) + RD(8B) + prefix
 * @return 写入字节数，-1=空间不足/非法
 */
static int encode_vpn_nlri(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri)
{
    if (!buf || !vpn_nlri_valid(nlri))
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
    memcpy(buf + 12, &nlri->prefix.prefix.addr.u.v4, nbytes);
    return 1 + 3 + 8 + nbytes;
}

static int encode_mp_reach_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                  const bgp_nexthop_t *nexthop, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!buf || !nlri_list || nlri_count <= 0 || !nexthop || nexthop->global.family != AF_INET)
    {
        return -1;
    }

    /* PA 头(3) + AFI(2) + SAFI(1) + NHLen(1) + NH(12) + SNPA(1) */
    int header_len = 3 + 2 + 1 + 1 + (int)VPN_IPV4_NH_LEN + 1;
    if (buf_size < header_len)
    {
        return -1;
    }

    int pos = 3;
    uint16_t afi_be = htons(BGP_AFI_IPV4);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_VPN_UNICAST;
    buf[pos++] = (uint8_t)VPN_IPV4_NH_LEN;
    memset(buf + pos, 0, 8); /* nexthop RD 全 0 */
    pos += 8;
    memcpy(buf + pos, &nexthop->global.u.v4, 4);
    pos += 4;
    buf[pos++] = 0; /* SNPA count */

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!vpn_nlri_valid(nlri))
        {
            continue;
        }
        int n = encode_vpn_nlri(buf + pos, buf_size - pos, nlri);
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
                                    int nlri_count, int *out_packed)
{
    if (out_packed)
    {
        *out_packed = 0;
    }
    if (!buf || !nlri_list || nlri_count <= 0)
    {
        return -1;
    }

    int header_len = 3 + 2 + 1;
    if (buf_size < header_len)
    {
        return -1;
    }

    int pos = 3;
    uint16_t afi_be = htons(BGP_AFI_IPV4);
    memcpy(buf + pos, &afi_be, 2);
    pos += 2;
    buf[pos++] = BGP_SAFI_VPN_UNICAST;

    int packed = 0;
    for (int i = 0; i < nlri_count; i++)
    {
        const bgp_nlri_entry_t *nlri = nlri_list[i];
        if (!vpn_nlri_valid(nlri))
        {
            continue;
        }
        int n = encode_vpn_nlri(buf + pos, buf_size - pos, nlri);
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

static int vpn_encode_reach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_nexthop_t *nexthop)
{
    const bgp_nlri_entry_t *list[1] = {nlri};
    int packed = 0;
    return encode_mp_reach_packed(buf, buf_size, list, 1, nexthop, &packed);
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

static int vpn_encode_unreach_pa(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *nlri, const bgp_conn_t *conn)
{
    (void)conn;
    const bgp_nlri_entry_t *list[1] = {nlri};
    int packed = 0;
    return encode_mp_unreach_packed(buf, buf_size, list, 1, &packed);
}

/* ------------------------------------------------------------------------- */
/* 打包版回调                                                                */
/* ------------------------------------------------------------------------- */

static int vpn_encode_reach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                      int nlri_count, const bgp_nexthop_t *nexthop, int *out_packed)
{
    return encode_mp_reach_packed(buf, buf_size, nlri_list, nlri_count, nexthop, out_packed);
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

static int vpn_encode_unreach_pa_packed(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list,
                                        int nlri_count, const bgp_conn_t *conn, int *out_packed)
{
    (void)conn;
    return encode_mp_unreach_packed(buf, buf_size, nlri_list, nlri_count, out_packed);
}

static const bgp_pkt_af_enc_t g_vpn_ipv4_enc = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_VPN_UNICAST,
    .encode_reach_pa = vpn_encode_reach_pa,
    .encode_reach_nlri = vpn_encode_reach_nlri,
    .encode_unreach_wd = vpn_encode_unreach_wd,
    .encode_unreach_pa = vpn_encode_unreach_pa,
    .encode_reach_pa_packed = vpn_encode_reach_pa_packed,
    .encode_reach_nlri_packed = vpn_encode_reach_nlri_packed,
    .encode_unreach_wd_packed = vpn_encode_unreach_wd_packed,
    .encode_unreach_pa_packed = vpn_encode_unreach_pa_packed,
};

void bgp_pkt_build_vpn_register(void)
{
    bgp_pkt_af_enc_register(&g_vpn_ipv4_enc);
}
