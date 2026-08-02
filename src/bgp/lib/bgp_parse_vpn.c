/**
 * @file   bgp_parse_vpn.c
 * @brief  AFI=1/2 SAFI=128（VPN-IPv4/IPv6 Unicast，MPLS L3VPN）NLRI 处理器
 * @author jhb
 * @date   2026/03/11
 *
 * 编码格式（RFC 4364）：
 *   Length（1B）= label_bits(24) + rd_bits(64) + ip_prefix_bits
 *   Label Stack Entry（3B）：label(20b) + Exp(3b) + BoS(1b)
 *   Route Distinguisher（8B）
 *   IP 前缀（剩余字节）
 *
 * MP_REACH nexthop（VPN-IPv4）：RD(8B) + IPv4(4B) = 12B
 * MP_REACH nexthop（VPN-IPv6）：RD(8B) + IPv6(16B) = 24B
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_parse_priv.h"

/** VPN 前缀中固定开销：label(24bit) + RD(64bit) = 88 bit */
#define VPN_FIXED_BITS 88

/* ============================================================================
 * 通用 VPN 前缀计数
 * ========================================================================== */

static uint32_t count_vpn(const uint8_t *data, uint16_t len, uint8_t max_ip_bits)
{
    uint16_t pos = 0;
    uint32_t count = 0;

    while (pos < len)
    {
        uint8_t total_bits = data[pos];

        if (total_bits < VPN_FIXED_BITS)
        {
            break;
        }

        uint8_t ip_bits = total_bits - VPN_FIXED_BITS;
        if (ip_bits > max_ip_bits)
        {
            break;
        }

        /* 1B length + 3B label + 8B RD + ceil(ip_bits/8) B prefix */
        uint8_t nbytes = (ip_bits + 7) / 8;
        if (pos + 1 + 3 + 8 + nbytes > len)
        {
            break;
        }

        pos += (uint16_t)(1 + 3 + 8 + nbytes);
        count++;
    }

    return count;
}

/* ============================================================================
 * 通用 VPN 前缀解析
 * ========================================================================== */

static int parse_vpn(const uint8_t *data, uint16_t len, int af, uint16_t afi, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    uint8_t max_ip_bits = (af == AF_INET6) ? 128 : 32;
    uint32_t count = count_vpn(data, len, max_ip_bits);

    if (count == 0)
    {
        return 0;
    }

    *out = calloc(count, sizeof(bgp_nlri_entry_t));
    if (!*out)
    {
        return -1;
    }

    uint16_t pos = 0;
    uint32_t idx = 0;

    while (pos < len && idx < count)
    {
        bgp_nlri_entry_t *e = &(*out)[idx];

        uint8_t total_bits = data[pos++];
        if (total_bits < VPN_FIXED_BITS)
        {
            break;
        }

        uint8_t ip_bits = total_bits - VPN_FIXED_BITS;
        if (ip_bits > max_ip_bits)
        {
            break;
        }

        /* label（3B） */
        if (pos + 3 > len)
        {
            break;
        }
        /* 当前实现只支持单标签 VPN NLRI；BoS=0 表示后续仍有标签，
         * 不能把首标签误当成完整服务标签（尤其不能误接受 label 3）。 */
        if ((data[pos + 2] & 0x01u) == 0u)
        {
            break;
        }
        uint32_t label = bgp_label_decode(data + pos);
        pos += 3;

        /* RD（8B） */
        if (pos + 8 > len)
        {
            break;
        }
        memcpy(e->prefix.rd.bytes, data + pos, 8);
        e->prefix.has_rd = true;
        e->prefix.label = label;
        e->prefix.has_label = true;
        pos += 8;

        /* IP 前缀 */
        uint8_t nbytes = (ip_bits + 7) / 8;
        if (pos + nbytes > len)
        {
            break;
        }

        e->afi = afi;
        e->safi = BGP_SAFI_VPN_UNICAST;
        e->type = BGP_NLRI_PREFIX;
        e->prefix.prefix.addr.family = (sa_family_t)af;
        e->prefix.prefix.prefix_len = ip_bits;

        /* 填充地址字节，末字节低位补零 */
        if (af == AF_INET6)
        {
            memcpy(e->prefix.prefix.addr.u.v6.s6_addr, data + pos, nbytes);
            if (ip_bits % 8 != 0)
            {
                e->prefix.prefix.addr.u.v6.s6_addr[nbytes - 1] &= (uint8_t)(0xFF << (8 - (ip_bits % 8)));
            }
        }
        else
        {
            uint8_t tmp[4] = {0};
            memcpy(tmp, data + pos, nbytes);
            if (ip_bits % 8 != 0)
            {
                tmp[nbytes - 1] &= (uint8_t)(0xFF << (8 - (ip_bits % 8)));
            }
            memcpy(&e->prefix.prefix.addr.u.v4, tmp, 4);
        }

        pos += nbytes;
        idx++;
    }

    *out_len = idx;
    return 0;
}

/* ============================================================================
 * VPN nexthop 解析
 * VPN-IPv4 + IPv4 NH: RD(8B) + IPv4(4B) = 12B
 * VPN-IPv4 + IPv6 NH: RD(8B) + IPv6(16B) = 24B（RFC 8950）
 * VPN-IPv6 + IPv6 NH: RD(8B) + IPv6(16B) = 24B（RFC 4659）
 * 双 IPv6 NH 为 48B：RD+global 后跟 RD+link-local。
 * ========================================================================== */

static int vpn_ipv4_nexthop(const uint8_t *nh_data, uint8_t nh_len, uint32_t flags, bgp_nexthop_t *nexthop)
{
    if (!nh_data || !nexthop)
    {
        return -1;
    }
    memset(nexthop, 0, sizeof(*nexthop));

    if (nh_len == 12u)
    {
        nexthop->global.family = AF_INET;
        memcpy(&nexthop->global.u.v4, nh_data + 8, 4);
        return 0;
    }
    if ((nh_len == 24u || nh_len == 48u) && (flags & BGP_PARSE_FLAG_EXT_NEXTHOP))
    {
        nexthop->global.family = AF_INET6;
        memcpy(nexthop->global.u.v6.s6_addr, nh_data + 8, 16);
        if (nh_len == 48u)
        {
            nexthop->link_local.family = AF_INET6;
            memcpy(nexthop->link_local.u.v6.s6_addr, nh_data + 32, 16);
            nexthop->has_link_local = true;
        }
        return 0;
    }
    return -1;
}

static int vpn_ipv6_nexthop(const uint8_t *nh_data, uint8_t nh_len, uint32_t flags, bgp_nexthop_t *nexthop)
{
    (void)flags;
    if (!nh_data || !nexthop || (nh_len != 24u && nh_len != 48u))
    {
        return -1;
    }
    memset(nexthop, 0, sizeof(*nexthop));
    nexthop->global.family = AF_INET6;
    memcpy(nexthop->global.u.v6.s6_addr, nh_data + 8, 16);
    if (nh_len == 48u)
    {
        nexthop->link_local.family = AF_INET6;
        memcpy(nexthop->link_local.u.v6.s6_addr, nh_data + 32, 16);
        nexthop->has_link_local = true;
    }
    return 0;
}

/* ============================================================================
 * entry_to_str
 * ========================================================================== */

static void vpn_ipv4_to_str(const bgp_nlri_entry_t *e, char *buf, size_t sz)
{
    if (!e || !buf || sz == 0)
    {
        return;
    }
    char rd_str[48];
    char ip[INET_ADDRSTRLEN];
    bgp_rd_to_str(&e->prefix.rd, rd_str, sizeof(rd_str));
    inet_ntop(AF_INET, &e->prefix.prefix.addr.u.v4, ip, sizeof(ip));
    snprintf(buf, sz, "vpn:%s:%s/%u label=%u", rd_str, ip, e->prefix.prefix.prefix_len, e->prefix.label);
}

static void vpn_ipv6_to_str(const bgp_nlri_entry_t *e, char *buf, size_t sz)
{
    if (!e || !buf || sz == 0)
    {
        return;
    }
    char rd_str[48];
    char ip[INET6_ADDRSTRLEN];
    bgp_rd_to_str(&e->prefix.rd, rd_str, sizeof(rd_str));
    inet_ntop(AF_INET6, &e->prefix.prefix.addr.u.v6, ip, sizeof(ip));
    snprintf(buf, sz, "vpn:%s:%s/%u label=%u", rd_str, ip, e->prefix.prefix.prefix_len, e->prefix.label);
}

/* ============================================================================
 * AFI=1, SAFI=128 封装
 * ========================================================================== */

static int vpn_ipv4_reach(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    return parse_vpn(data, len, AF_INET, BGP_AFI_IPV4, out, out_len);
}

/* ============================================================================
 * AFI=2, SAFI=128 封装
 * ========================================================================== */

static int vpn_ipv6_reach(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    return parse_vpn(data, len, AF_INET6, BGP_AFI_IPV6, out, out_len);
}

/* ============================================================================
 * 处理器描述符与注册
 * ========================================================================== */

static const bgp_af_parser_t g_vpn_ipv4 = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_VPN_UNICAST,
    .parse_reach = vpn_ipv4_reach,
    .parse_unreach = vpn_ipv4_reach,
    .parse_nexthop = vpn_ipv4_nexthop,
    .entry_to_str = vpn_ipv4_to_str,
};

static const bgp_af_parser_t g_vpn_ipv6 = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_VPN_UNICAST,
    .parse_reach = vpn_ipv6_reach,
    .parse_unreach = vpn_ipv6_reach,
    .parse_nexthop = vpn_ipv6_nexthop,
    .entry_to_str = vpn_ipv6_to_str,
};

void bgp_parse_vpn_register(void)
{
    bgp_af_parser_register(&g_vpn_ipv4);
    bgp_af_parser_register(&g_vpn_ipv6);
}
