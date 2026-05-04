/**
 * @file   bgp_if_cache.c
 * @brief  BGP IF 缓存包装：底层复用 if_api 统一缓存
 * @author jhb
 * @date   2026/04/11
 */
#include "bgp_if_cache.h"

#include <stdio.h>
#include <string.h>

#include "if.h"
#include "route.h"

/**
 * @brief 检查 IP 地址是否在指定前缀范围内
 */
static int prefix_contains_addr(const net_addr_t *prefix_addr, uint8_t prefix_len, const net_addr_t *addr)
{
    if (!prefix_addr || !addr || prefix_addr->family != addr->family)
    {
        return 0;
    }

    const uint8_t *pfx = NULL;
    const uint8_t *ip = NULL;
    int addr_len = 0;

    if (addr->family == AF_INET)
    {
        pfx = (const uint8_t *)&prefix_addr->u.v4.s_addr;
        ip = (const uint8_t *)&addr->u.v4.s_addr;
        addr_len = 4;
    }
    else if (addr->family == AF_INET6)
    {
        pfx = prefix_addr->u.v6.s6_addr;
        ip = addr->u.v6.s6_addr;
        addr_len = 16;
    }
    else
    {
        return 0;
    }

    if (prefix_len > (uint8_t)(addr_len * 8))
    {
        return 0;
    }

    uint8_t full_bytes = prefix_len / 8;
    uint8_t rem_bits = prefix_len % 8;

    if (full_bytes > 0 && memcmp(pfx, ip, full_bytes) != 0)
    {
        return 0;
    }
    if (rem_bits > 0)
    {
        uint8_t mask = (uint8_t)(0xFFU << (8U - rem_bits));
        if ((pfx[full_bytes] & mask) != (ip[full_bytes] & mask))
        {
            return 0;
        }
    }
    return 1;
}

typedef struct bgp_direct_check_ctx
{
    const net_addr_t *neighbor_addr;
    gboolean found;
    uint32_t ifindex;
} bgp_direct_check_ctx_t;

static gboolean bgp_direct_check_iter(const if_api_cache_entry_t *entry, void *user_data)
{
    bgp_direct_check_ctx_t *ctx = (bgp_direct_check_ctx_t *)user_data;
    if (!entry || !ctx || !ctx->neighbor_addr || ctx->found)
    {
        return ctx ? ctx->found : TRUE;
    }

    if (!entry->proto_up)
    {
        return FALSE;
    }

    const net_addr_t *local_addr = NULL;
    uint8_t prefix_len = 0;

    if (ctx->neighbor_addr->family == AF_INET && entry->ipv4_addr.family == AF_INET)
    {
        local_addr = &entry->ipv4_addr;
        prefix_len = entry->ipv4_prefix_len;
    }
    else if (ctx->neighbor_addr->family == AF_INET6 && entry->ipv6_addr.family == AF_INET6)
    {
        local_addr = &entry->ipv6_addr;
        prefix_len = entry->ipv6_prefix_len;
    }

    if (!local_addr || prefix_len == 0)
    {
        return FALSE;
    }

    if (prefix_contains_addr(local_addr, prefix_len, ctx->neighbor_addr))
    {
        ctx->found = TRUE;
        ctx->ifindex = entry->ifindex;
        return TRUE;
    }

    return FALSE;
}

gboolean bgp_if_cache_is_directly_connected(const net_addr_t *neighbor_addr)
{
    if (!neighbor_addr || neighbor_addr->family == 0)
    {
        return FALSE;
    }

    bgp_direct_check_ctx_t ctx = {
        .neighbor_addr = neighbor_addr,
        .found = FALSE,
    };

    if_api_cache_foreach(bgp_direct_check_iter, &ctx);
    return ctx.found;
}

uint32_t bgp_if_cache_direct_ifindex(const net_addr_t *neighbor_addr)
{
    if (!neighbor_addr || neighbor_addr->family == 0)
    {
        return 0u;
    }

    bgp_direct_check_ctx_t ctx = {
        .neighbor_addr = neighbor_addr,
        .found = FALSE,
        .ifindex = 0u,
    };

    if_api_cache_foreach(bgp_direct_check_iter, &ctx);
    return ctx.ifindex;
}

int bgp_if_cache_resolve_source_addr(const char *if_name, sa_family_t peer_family, net_addr_t *out_addr, char *errmsg,
                                     size_t errmsg_len)
{
    if (!if_name || if_name[0] == '\0' || !out_addr)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Missing source interface name.");
        }
        return -1;
    }

    const if_api_cache_entry_t *entry = if_api_cache_lookup(if_name);
    if (!entry)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Interface '%s' not found.", if_name);
        }
        return -1;
    }

    const net_addr_t *addr = NULL;

    if (peer_family == AF_INET6)
    {
        if (entry->ipv6_addr.family == AF_INET6)
        {
            addr = &entry->ipv6_addr;
        }
    }
    else if (peer_family == AF_INET)
    {
        if (entry->ipv4_addr.family == AF_INET)
        {
            addr = &entry->ipv4_addr;
        }
    }
    else
    {
        /* 未指定对端地址族时，按 IPv4 -> IPv6 回退 */
        if (entry->ipv4_addr.family == AF_INET)
        {
            addr = &entry->ipv4_addr;
        }
        else if (entry->ipv6_addr.family == AF_INET6)
        {
            addr = &entry->ipv6_addr;
        }
    }

    if (!addr)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Interface '%s' has no usable IP address.", if_name);
        }
        return -1;
    }

    *out_addr = *addr;
    return 0;
}
