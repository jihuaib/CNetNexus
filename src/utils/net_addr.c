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
