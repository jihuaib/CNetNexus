/**
 * @file   net_addr.c
 * @brief  通用 IP 地址类型实现（net_addr_t）
 * @author jhb
 * @date   2026/03/03
 */
#include "net_addr.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

int net_addr_from_str(const char *str, net_addr_t *out)
{
    if (!str || !out)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    /* 先尝试 IPv4 */
    if (inet_pton(AF_INET, str, &out->u.v4) == 1)
    {
        out->family = AF_INET;
        return 0;
    }

    /* 再尝试 IPv6 */
    if (inet_pton(AF_INET6, str, &out->u.v6) == 1)
    {
        out->family = AF_INET6;
        return 0;
    }

    return -1;
}

void net_addr_to_str(const net_addr_t *addr, char *buf, size_t sz)
{
    if (!addr || !buf || sz == 0)
    {
        return;
    }

    if (addr->family == AF_INET)
    {
        inet_ntop(AF_INET, &addr->u.v4, buf, (socklen_t)sz);
    }
    else if (addr->family == AF_INET6)
    {
        inet_ntop(AF_INET6, &addr->u.v6, buf, (socklen_t)sz);
    }
    else
    {
        snprintf(buf, sz, "unknown");
    }
}

gboolean net_addr_equal(const net_addr_t *a, const net_addr_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }

    if (a->family != b->family)
    {
        return FALSE;
    }

    if (a->family == AF_INET)
    {
        return memcmp(&a->u.v4, &b->u.v4, sizeof(struct in_addr)) == 0 ? TRUE : FALSE;
    }
    else if (a->family == AF_INET6)
    {
        return memcmp(&a->u.v6, &b->u.v6, sizeof(struct in6_addr)) == 0 ? TRUE : FALSE;
    }

    return FALSE;
}

int net_addr_cmp(const net_addr_t *a, const net_addr_t *b)
{
    if (!a && !b)
    {
        return 0;
    }
    if (!a)
    {
        return -1;
    }
    if (!b)
    {
        return 1;
    }

    int r = (int)a->family - (int)b->family;
    if (r)
    {
        return r;
    }

    if (a->family == AF_INET)
    {
        return memcmp(&a->u.v4, &b->u.v4, sizeof(a->u.v4));
    }
    if (a->family == AF_INET6)
    {
        return memcmp(&a->u.v6, &b->u.v6, sizeof(a->u.v6));
    }

    return 0;
}

guint net_addr_hash(gconstpointer key)
{
    const net_addr_t *addr = (const net_addr_t *)key;
    if (!addr)
    {
        return 0;
    }

    if (addr->family == AF_INET)
    {
        /* IPv4：直接对 4 字节整数取 GLib hash */
        return g_int_hash(&addr->u.v4.s_addr);
    }
    else if (addr->family == AF_INET6)
    {
        /* IPv6：按字节滚动哈希 */
        guint h = 0;
        const uint8_t *b = addr->u.v6.s6_addr;
        for (int i = 0; i < 16; i++)
        {
            h = h * 31u + b[i];
        }
        return h;
    }

    return (guint)addr->family;
}

gboolean net_addr_hash_equal(gconstpointer a, gconstpointer b)
{
    return net_addr_equal((const net_addr_t *)a, (const net_addr_t *)b);
}

// ============================================================================
// net_prefix_t 实现
// ============================================================================

gboolean net_prefix_is_set(const net_prefix_t *p)
{
    return (p && p->addr.family != 0) ? TRUE : FALSE;
}

void net_prefix_to_str(const net_prefix_t *p, char *buf, size_t sz)
{
    if (!p || !buf || sz == 0)
    {
        return;
    }

    char addr_str[64] = {0};
    net_addr_to_str(&p->addr, addr_str, sizeof(addr_str));
    snprintf(buf, sz, "%s/%u", addr_str, p->prefix_len);
}

void net_prefix_len_to_mask_str(uint8_t prefix_len, char *buf)
{
    if (!buf)
    {
        return;
    }

    uint32_t mask = (prefix_len == 0) ? 0 : (~0U << (32 - prefix_len));
    struct in_addr in;
    in.s_addr = htonl(mask);
    inet_ntop(AF_INET, &in, buf, 16);
}
