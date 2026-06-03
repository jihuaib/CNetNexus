/**
 * @file   bgp_parse_ipv4uc.c
 * @brief  AFI=1 SAFI=1（IPv4 Unicast）NLRI 处理器
 * @author jhb
 * @date   2026/03/11
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_parse_priv.h"

/* ============================================================================
 * 前缀数量预统计（避免多次 realloc）
 * ========================================================================== */

static uint32_t count_ipv4_prefixes(const uint8_t *data, uint16_t len)
{
    uint16_t pos = 0;
    uint32_t count = 0;

    while (pos < len)
    {
        uint8_t plen = data[pos];
        uint8_t nbytes = (plen + 7) / 8;

        if (plen > 32 || pos + 1 + nbytes > len)
        {
            break;
        }

        pos += (uint16_t)(1 + nbytes);
        count++;
    }

    return count;
}

/* ============================================================================
 * 公共解析逻辑（reach / unreach 相同）
 * ========================================================================== */

static int parse_prefixes(const uint8_t *data, uint16_t len, bgp_nlri_entry_t **out, uint32_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    uint32_t count = count_ipv4_prefixes(data, len);
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

        if (bgp_read_prefix(data + pos, len - pos, AF_INET, &e->prefix.prefix, &consumed) < 0)
        {
            break;
        }

        e->afi = BGP_AFI_IPV4;
        e->safi = BGP_SAFI_UNICAST;
        e->type = BGP_NLRI_PREFIX;

        pos += consumed;
        idx++;
    }

    *out_len = idx;
    return 0;
}

/* ============================================================================
 * nexthop 解析（来自 MP_REACH）
 *   4B  = IPv4 nexthop（传统）
 *   16B = IPv6 global nexthop（RFC 8950 Extended Next Hop）
 *   32B = IPv6 global + link-local nexthop（RFC 8950）
 * ========================================================================== */

static int parse_nexthop(const uint8_t *nh_data, uint8_t nh_len, uint32_t flags, bgp_nexthop_t *nexthop)
{
    if (nh_len == 4)
    {
        nexthop->global.family = AF_INET;
        memcpy(&nexthop->global.u.v4, nh_data, 4);
        nexthop->has_link_local = false;
        return 0;
    }

    /* IPv4 unicast 携带 IPv6 nexthop（RFC 8950）必须已协商 Extended Next Hop，否则非法。 */
    if (!(flags & BGP_PARSE_FLAG_EXT_NEXTHOP))
    {
        return -1;
    }

    if (nh_len == 16)
    {
        nexthop->global.family = AF_INET6;
        memcpy(&nexthop->global.u.v6, nh_data, 16);
        nexthop->has_link_local = false;
        return 0;
    }

    if (nh_len == 32)
    {
        nexthop->global.family = AF_INET6;
        memcpy(&nexthop->global.u.v6, nh_data, 16);
        nexthop->has_link_local = true;
        nexthop->link_local.family = AF_INET6;
        memcpy(&nexthop->link_local.u.v6, nh_data + 16, 16);
        return 0;
    }

    return -1;
}

/* ============================================================================
 * entry_to_str
 * ========================================================================== */

static void entry_to_str(const bgp_nlri_entry_t *entry, char *buf, size_t sz)
{
    if (!entry || !buf || sz == 0)
    {
        return;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &entry->prefix.prefix.addr.u.v4, ip, sizeof(ip));
    snprintf(buf, sz, "%s/%u", ip, entry->prefix.prefix.prefix_len);
}

/* ============================================================================
 * 处理器描述符与注册
 * ========================================================================== */

static const bgp_af_parser_t g_ipv4uc = {
    .afi = BGP_AFI_IPV4,
    .safi = BGP_SAFI_UNICAST,
    .parse_reach = parse_prefixes,
    .parse_unreach = parse_prefixes,
    .parse_nexthop = parse_nexthop,
    .entry_to_str = entry_to_str,
};

void bgp_parse_ipv4uc_register(void)
{
    bgp_af_parser_register(&g_ipv4uc);
}
