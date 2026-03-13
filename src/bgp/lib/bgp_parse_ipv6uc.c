/**
 * @file   bgp_parse_ipv6uc.c
 * @brief  AFI=2 SAFI=1（IPv6 Unicast）NLRI 处理器
 * @author jhb
 * @date   2026/03/11
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_parse_priv.h"

/* ============================================================================
 * 前缀数量预统计
 * ========================================================================== */

static uint32_t count_ipv6_prefixes(const uint8_t *data, uint16_t len)
{
    uint16_t pos = 0;
    uint32_t count = 0;

    while (pos < len)
    {
        uint8_t plen = data[pos];
        uint8_t nbytes = (plen + 7) / 8;

        if (plen > 128 || pos + 1 + nbytes > len)
        {
            break;
        }

        pos += (uint16_t)(1 + nbytes);
        count++;
    }

    return count;
}

/* ============================================================================
 * 公共解析逻辑
 * ========================================================================== */

static int parse_prefixes(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    uint32_t count = count_ipv6_prefixes(data, len);
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
        uint16_t consumed;

        if (bgp_read_prefix(data + pos, len - pos, AF_INET6, &e->prefix.prefix, &consumed) < 0)
        {
            break;
        }

        e->afi = BGP_AFI_IPV6;
        e->safi = BGP_SAFI_UNICAST;
        e->type = BGP_NLRI_PREFIX;

        char ip[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &e->prefix.prefix.addr.u.v6, ip, sizeof(ip));
        snprintf(e->key, BGP_NLRI_KEY_MAX, "%s/%u", ip, e->prefix.prefix.prefix_len);

        pos += consumed;
        idx++;
    }

    *out_len = idx;
    return 0;
}

/* ============================================================================
 * nexthop 解析
 * 16 字节 = 仅全局地址
 * 32 字节 = 全局地址（16B）+ link-local 地址（16B）
 * ========================================================================== */

static int parse_nexthop(const uint8_t *nh_data, uint8_t nh_len, bgp_nexthop_t *nexthop)
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

/* ============================================================================
 * entry_to_str
 * ========================================================================== */

static void entry_to_str(bgp_nlri_entry_t *entry)
{
    char ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &entry->prefix.prefix.addr.u.v6, ip, sizeof(ip));
    snprintf(entry->key, BGP_NLRI_KEY_MAX, "%s/%u", ip, entry->prefix.prefix.prefix_len);
}

/* ============================================================================
 * 处理器描述符与注册
 * ========================================================================== */

static const bgp_af_parser_t g_ipv6uc = {
    .afi = BGP_AFI_IPV6,
    .safi = BGP_SAFI_UNICAST,
    .parse_reach = parse_prefixes,
    .parse_unreach = parse_prefixes,
    .parse_nexthop = parse_nexthop,
    .entry_to_str = entry_to_str,
};

void bgp_parse_ipv6uc_register(void)
{
    bgp_af_parser_register(&g_ipv6uc);
}
