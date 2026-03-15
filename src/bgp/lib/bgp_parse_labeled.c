/**
 * @file   bgp_parse_labeled.c
 * @brief  AFI=1/2 SAFI=4（MPLS Labeled Unicast）NLRI 处理器
 * @author jhb
 * @date   2026/03/11
 *
 * 编码格式（RFC 3107）：
 *   Length（1B）= label_bits(24) + ip_prefix_bits
 *   Label Stack Entry（3B）：label(20b) + Exp(3b) + BoS(1b)
 *   IP 前缀（剩余字节，= (Length - 24 + 7) / 8 字节）
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_parse_priv.h"

/* ============================================================================
 * 通用 labeled 前缀解析
 * ========================================================================== */

static uint32_t count_labeled(const uint8_t *data, uint16_t len, int af)
{
    uint16_t pos = 0;
    uint32_t count = 0;
    uint8_t max_ip_bits = (af == AF_INET6) ? 128 : 32;

    while (pos < len)
    {
        uint8_t total_bits = data[pos];

        /* 至少包含 1 个 label（24 bit） */
        if (total_bits < 24)
        {
            break;
        }

        uint8_t ip_bits = total_bits - 24;
        if (ip_bits > max_ip_bits)
        {
            break;
        }

        /* 1B length + 3B label + ceil(ip_bits/8) B prefix */
        uint8_t nbytes = (ip_bits + 7) / 8;
        if (pos + 1 + 3 + nbytes > len)
        {
            break;
        }

        pos += (uint16_t)(1 + 3 + nbytes);
        count++;
    }

    return count;
}

static int parse_labeled(const uint8_t *data, uint16_t len, int af, uint16_t afi, bgp_nlri_entry_t **out,
                         uint32_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    uint32_t count = count_labeled(data, len, af);
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
    uint8_t max_ip_bits = (af == AF_INET6) ? 128 : 32;

    while (pos < len && idx < count)
    {
        bgp_nlri_entry_t *e = &(*out)[idx];

        uint8_t total_bits = data[pos++];
        if (total_bits < 24)
        {
            break;
        }

        uint8_t ip_bits = total_bits - 24;
        if (ip_bits > max_ip_bits)
        {
            break;
        }

        /* 解码 label（BoS 位忽略，只取高 20 位） */
        if (pos + 3 > len)
        {
            break;
        }
        uint32_t label = bgp_label_decode(data + pos);
        pos += 3;

        /* 读取 IP 前缀 */
        uint8_t nbytes = (ip_bits + 7) / 8;
        if (pos + nbytes > len)
        {
            break;
        }

        e->afi = afi;
        e->safi = BGP_SAFI_LABELED;
        e->type = BGP_NLRI_PREFIX;

        e->prefix.prefix.addr.family = (sa_family_t)af;
        e->prefix.prefix.prefix_len = ip_bits;
        e->prefix.label = label;
        e->prefix.has_label = true;

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
 * IPv4 labeled（AFI=1, SAFI=4）
 * ========================================================================== */

static int ipv4_labeled_reach(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    return parse_labeled(data, len, AF_INET, BGP_AFI_IPV4, out, out_len);
}

static int ipv4_labeled_nexthop(const uint8_t *nh_data, uint8_t nh_len, bgp_nexthop_t *nexthop)
{
    if (nh_len < 4)
    {
        return -1;
    }
    nexthop->global.family = AF_INET;
    memcpy(&nexthop->global.u.v4, nh_data, 4);
    return 0;
}

static void ipv4_labeled_to_str(const bgp_nlri_entry_t *e, char *buf, size_t sz)
{
    if (!e || !buf || sz == 0)
    {
        return;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &e->prefix.prefix.addr.u.v4, ip, sizeof(ip));
    snprintf(buf, sz, "%s/%u label=%u", ip, e->prefix.prefix.prefix_len, e->prefix.label);
}

/* ============================================================================
 * IPv6 labeled（AFI=2, SAFI=4）
 * ========================================================================== */

static int ipv6_labeled_reach(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    return parse_labeled(data, len, AF_INET6, BGP_AFI_IPV6, out, out_len);
}

static int ipv6_labeled_nexthop(const uint8_t *nh_data, uint8_t nh_len, bgp_nexthop_t *nexthop)
{
    if (nh_len < 16)
    {
        return -1;
    }
    nexthop->global.family = AF_INET6;
    memcpy(nexthop->global.u.v6.s6_addr, nh_data, 16);
    if (nh_len >= 32)
    {
        nexthop->link_local.family = AF_INET6;
        memcpy(nexthop->link_local.u.v6.s6_addr, nh_data + 16, 16);
        nexthop->has_link_local = true;
    }
    return 0;
}

static void ipv6_labeled_to_str(const bgp_nlri_entry_t *e, char *buf, size_t sz)
{
    if (!e || !buf || sz == 0)
    {
        return;
    }
    char ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &e->prefix.prefix.addr.u.v6, ip, sizeof(ip));
    snprintf(buf, sz, "%s/%u label=%u", ip, e->prefix.prefix.prefix_len, e->prefix.label);
}

/* ============================================================================
 * 处理器描述符与注册
 * ========================================================================== */

static const bgp_af_parser_t g_ipv4_labeled = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_LABELED,
    .parse_reach = ipv4_labeled_reach,
    .parse_unreach = ipv4_labeled_reach,
    .parse_nexthop = ipv4_labeled_nexthop,
    .entry_to_str = ipv4_labeled_to_str,
};

static const bgp_af_parser_t g_ipv6_labeled = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_LABELED,
    .parse_reach = ipv6_labeled_reach,
    .parse_unreach = ipv6_labeled_reach,
    .parse_nexthop = ipv6_labeled_nexthop,
    .entry_to_str = ipv6_labeled_to_str,
};

void bgp_parse_labeled_register(void)
{
    bgp_af_parser_register(&g_ipv4_labeled);
    bgp_af_parser_register(&g_ipv6_labeled);
}
