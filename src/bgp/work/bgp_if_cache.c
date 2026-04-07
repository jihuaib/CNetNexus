/**
 * @file   bgp_if_cache.c
 * @brief  BGP 本地接口缓存实现：通过 IF 事件订阅维护接口地址/状态
 * @author jhb
 * @date   2026/04/07
 */
#include "bgp_if_cache.h"

#include <string.h>

#include "log.h"
#include "route.h"

/** 本地接口缓存表：key=逻辑接口名（直接指向 entry->logical_name），value=bgp_if_cache_entry_t* */
static GHashTable *g_bgp_if_cache = NULL;

/* ============================================================================
 * 内部辅助
 * ========================================================================== */

/**
 * @brief 按逻辑名查找或创建缓存条目
 */
static bgp_if_cache_entry_t *cache_get_or_create(const char *logical_name)
{
    bgp_if_cache_entry_t *entry = (bgp_if_cache_entry_t *)g_hash_table_lookup(g_bgp_if_cache, logical_name);
    if (entry)
    {
        return entry;
    }

    entry = g_malloc0(sizeof(bgp_if_cache_entry_t));
    g_strlcpy(entry->logical_name, logical_name, sizeof(entry->logical_name));
    g_hash_table_insert(g_bgp_if_cache, entry->logical_name, entry);
    return entry;
}

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

/**
 * @brief 处理 UP/DOWN 事件
 */
static void cache_handle_up_down(const if_event_msg_t *evt)
{
    bgp_if_cache_entry_t *entry = cache_get_or_create(evt->logical_name);
    entry->admin_up = evt->admin_up;
    LOG_DEBUG("BGP IF cache: %s admin_%s", evt->logical_name, evt->admin_up ? "up" : "down");
}

/**
 * @brief 处理 ADDR_ADD/ADDR_DEL 事件
 */
static void cache_handle_addr(const if_addr_event_msg_t *evt)
{
    bgp_if_cache_entry_t *entry = cache_get_or_create(evt->logical_name);

    if (evt->event == IF_EVENT_ADDR_ADD)
    {
        if (evt->afi == ROUTE_AFI_IPV4)
        {
            entry->ipv4_addr = evt->addr;
            entry->ipv4_prefix_len = evt->prefix_len;
        }
        else if (evt->afi == ROUTE_AFI_IPV6)
        {
            entry->ipv6_addr = evt->addr;
            entry->ipv6_prefix_len = evt->prefix_len;
        }
        char addr_str[64];
        net_addr_to_str(&evt->addr, addr_str, sizeof(addr_str));
        LOG_DEBUG("BGP IF cache: %s addr_add %s/%u", evt->logical_name, addr_str, evt->prefix_len);
    }
    else if (evt->event == IF_EVENT_ADDR_DEL)
    {
        if (evt->afi == ROUTE_AFI_IPV4)
        {
            memset(&entry->ipv4_addr, 0, sizeof(entry->ipv4_addr));
            entry->ipv4_prefix_len = 0;
        }
        else if (evt->afi == ROUTE_AFI_IPV6)
        {
            memset(&entry->ipv6_addr, 0, sizeof(entry->ipv6_addr));
            entry->ipv6_prefix_len = 0;
        }
        LOG_DEBUG("BGP IF cache: %s addr_del afi=%u", evt->logical_name, evt->afi);
    }
}

/* ============================================================================
 * 公共 API
 * ========================================================================== */

void bgp_if_cache_init(void)
{
    if (g_bgp_if_cache)
    {
        return;
    }
    g_bgp_if_cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    LOG_INFO("BGP IF cache initialized");
}

void bgp_if_cache_cleanup(void)
{
    if (g_bgp_if_cache)
    {
        g_hash_table_destroy(g_bgp_if_cache);
        g_bgp_if_cache = NULL;
    }
}

void bgp_if_cache_on_event(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || !g_bgp_if_cache)
    {
        return;
    }

    if (msg->payload_len >= sizeof(if_addr_event_msg_t))
    {
        const if_addr_event_msg_t *addr_evt = (const if_addr_event_msg_t *)msg->payload;
        if (addr_evt->event == IF_EVENT_ADDR_ADD || addr_evt->event == IF_EVENT_ADDR_DEL)
        {
            cache_handle_addr(addr_evt);
            return;
        }
    }

    if (msg->payload_len >= sizeof(if_event_msg_t))
    {
        const if_event_msg_t *evt = (const if_event_msg_t *)msg->payload;
        if (evt->event == IF_EVENT_UP || evt->event == IF_EVENT_DOWN)
        {
            cache_handle_up_down(evt);
        }
    }
}

gboolean bgp_if_cache_is_directly_connected(const net_addr_t *neighbor_addr)
{
    if (!neighbor_addr || neighbor_addr->family == 0 || !g_bgp_if_cache)
    {
        return FALSE;
    }

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, g_bgp_if_cache);

    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        const bgp_if_cache_entry_t *entry = (const bgp_if_cache_entry_t *)value;
        if (!entry->admin_up)
        {
            continue;
        }

        const net_addr_t *local_addr = NULL;
        uint8_t prefix_len = 0;

        if (neighbor_addr->family == AF_INET && entry->ipv4_addr.family == AF_INET)
        {
            local_addr = &entry->ipv4_addr;
            prefix_len = entry->ipv4_prefix_len;
        }
        else if (neighbor_addr->family == AF_INET6 && entry->ipv6_addr.family == AF_INET6)
        {
            local_addr = &entry->ipv6_addr;
            prefix_len = entry->ipv6_prefix_len;
        }

        if (!local_addr || prefix_len == 0)
        {
            continue;
        }

        if (prefix_contains_addr(local_addr, prefix_len, neighbor_addr))
        {
            return TRUE;
        }
    }

    return FALSE;
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

    if (!g_bgp_if_cache)
    {
        if (errmsg && errmsg_len > 0)
        {
            snprintf(errmsg, errmsg_len, "BGP Error: Interface cache not initialized.");
        }
        return -1;
    }

    const bgp_if_cache_entry_t *entry = (const bgp_if_cache_entry_t *)g_hash_table_lookup(g_bgp_if_cache, if_name);
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
