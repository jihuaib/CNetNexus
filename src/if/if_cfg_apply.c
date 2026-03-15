/**
 * @file   if_cfg_apply.c
 * @brief  接口配置内存态应用实现（CLI / DB 恢复共用）
 * @author jhb
 * @date   2026/03/08
 */
#include "if_cfg_apply.h"

#include <arpa/inet.h>
#include <string.h>

#include "errcode.h"
#include "if.h"
#include "if_event.h"
#include "if_main.h"
#include "if_pub.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

// ============================================================================
// 公共 API
// ============================================================================

static uint32_t if_cfg_type_to_mask(if_type_t type)
{
    switch (type)
    {
        case IF_TYPE_ETHERNET:
        case IF_TYPE_VETH:
            /* 现阶段统一对外暴露为 ETH 口 */
            return IF_INTF_TYPE_ETH;
        default:
            return 0;
    }
}

static gboolean if_prefix_equal(const net_prefix_t *a, const net_prefix_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }
    return (a->prefix_len == b->prefix_len && net_addr_equal(&a->addr, &b->addr)) ? TRUE : FALSE;
}

static int if_prefix_to_network(const net_prefix_t *prefix, net_addr_t *out)
{
    if (!prefix || !out || !net_prefix_is_set(prefix))
    {
        return -1;
    }

    if (prefix->addr.family == AF_INET)
    {
        if (prefix->prefix_len > 32)
        {
            return -1;
        }
        uint32_t ip = ntohl(prefix->addr.u.v4.s_addr);
        uint32_t mask = (prefix->prefix_len == 0) ? 0u : (0xFFFFFFFFu << (32 - prefix->prefix_len));
        memset(out, 0, sizeof(*out));
        out->family = AF_INET;
        out->u.v4.s_addr = htonl(ip & mask);
        return 0;
    }

    if (prefix->addr.family == AF_INET6)
    {
        if (prefix->prefix_len > 128)
        {
            return -1;
        }
        memset(out, 0, sizeof(*out));
        out->family = AF_INET6;
        memcpy(out->u.v6.s6_addr, prefix->addr.u.v6.s6_addr, 16);

        uint8_t bits = prefix->prefix_len;
        for (int i = 0; i < 16; i++)
        {
            if (bits >= 8)
            {
                bits -= 8;
                continue;
            }
            if (bits == 0)
            {
                out->u.v6.s6_addr[i] = 0;
            }
            else
            {
                uint8_t mask = (uint8_t)(0xFFu << (8 - bits));
                out->u.v6.s6_addr[i] &= mask;
                bits = 0;
            }
        }
        return 0;
    }

    return -1;
}

static void if_make_zero_addr(sa_family_t family, net_addr_t *out)
{
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->family = family;
}

static void if_sync_connected_host_routes(const net_prefix_t *prefix, gboolean is_withdraw)
{
    if (!prefix || !net_prefix_is_set(prefix))
    {
        return;
    }

    uint16_t afi = 0;
    uint8_t host_prefix_len = 0;
    if (prefix->addr.family == AF_INET)
    {
        afi = ROUTE_AFI_IPV4;
        host_prefix_len = 32;
    }
    else if (prefix->addr.family == AF_INET6)
    {
        afi = ROUTE_AFI_IPV6;
        host_prefix_len = 128;
    }
    else
    {
        return;
    }

    net_addr_t network_addr;
    if (if_prefix_to_network(prefix, &network_addr) != 0)
    {
        return;
    }

    net_addr_t zero_nh;
    if_make_zero_addr(prefix->addr.family, &zero_nh);
    dev_ipc_context_t *ctx = g_if_local ? g_if_local->dev_ipc_ctx : NULL;
    if (!ctx)
    {
        return;
    }

    /* 直连网络路由 */
    if (is_withdraw)
    {
        (void)route_rpc_del(ctx, ROUTE_VRF_DEFAULT, afi, &network_addr, prefix->prefix_len, ROUTE_PROTOCOL_CONNECTED,
                            &prefix->addr);
    }
    else
    {
        (void)route_rpc_add(ctx, ROUTE_VRF_DEFAULT, afi, &network_addr, prefix->prefix_len, ROUTE_PROTOCOL_CONNECTED,
                            &prefix->addr, &zero_nh, 0, ROUTE_ADMIN_DIST_CONNECTED);
    }

    /* 本机主机路由 */
    if (!(host_prefix_len == prefix->prefix_len && net_addr_equal(&network_addr, &prefix->addr)))
    {
        if (is_withdraw)
        {
            (void)route_rpc_del(ctx, ROUTE_VRF_DEFAULT, afi, &prefix->addr, host_prefix_len, ROUTE_PROTOCOL_CONNECTED,
                                &prefix->addr);
        }
        else
        {
            (void)route_rpc_add(ctx, ROUTE_VRF_DEFAULT, afi, &prefix->addr, host_prefix_len, ROUTE_PROTOCOL_CONNECTED,
                                &prefix->addr, &zero_nh, 0, ROUTE_ADMIN_DIST_CONNECTED);
        }
    }
}

if_map_entry_t *if_cfg_find_entry(const char *logical_name)
{
    if (!logical_name || !g_if_local)
    {
        return NULL;
    }

    if_map_t *map = &g_if_local->interface_map;
    for (int i = 0; i < map->count; i++)
    {
        if (strcmp(map->entries[i].logical_name, logical_name) == 0)
        {
            return &map->entries[i];
        }
    }

    return NULL;
}

int if_cfg_apply_ip(gboolean is_no, const char *logical_name, const net_prefix_t *prefix)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_ERROR("IF: Interface %s not found", logical_name);
        return ERRCODE_FAIL;
    }

    net_prefix_t old_prefix = entry->prefix;
    gboolean had_old = net_prefix_is_set(&old_prefix);

    if (is_no)
    {
        if (had_old)
        {
            if_sync_connected_host_routes(&old_prefix, TRUE);
        }
        memset(&entry->prefix, 0, sizeof(entry->prefix));
        if_set_ip(entry->physical_name, "0.0.0.0", "0.0.0.0");
        LOG_INFO("IF: %s IP cleared", logical_name);
        return ERRCODE_SUCCESS;
    }

    if (!prefix || !net_prefix_is_set(prefix))
    {
        return ERRCODE_FAIL;
    }
    if (prefix->addr.family != AF_INET || prefix->prefix_len > 32)
    {
        LOG_ERROR("IF: Only IPv4 address is supported currently");
        return ERRCODE_FAIL;
    }

    /* 转字符串用于 ioctl */
    char ip_str[64];
    net_addr_to_str(&prefix->addr, ip_str, sizeof(ip_str));

    char mask_str[16];
    net_prefix_len_to_mask_str(prefix->prefix_len, mask_str);

    if (if_set_ip(entry->physical_name, ip_str, mask_str) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: Failed to configure physical interface %s IP", entry->physical_name);
        return ERRCODE_FAIL;
    }

    if (had_old && !if_prefix_equal(&old_prefix, prefix))
    {
        if_sync_connected_host_routes(&old_prefix, TRUE);
    }

    entry->prefix = *prefix;
    if (!entry->shutdown)
    {
        if_sync_connected_host_routes(&entry->prefix, FALSE);
    }

    LOG_INFO("IF: %s IP=%s/%u configured", logical_name, ip_str, prefix->prefix_len);
    return ERRCODE_SUCCESS;
}

int if_cfg_apply_shutdown(gboolean is_no, const char *logical_name)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_ERROR("IF: Interface %s not found", logical_name);
        return ERRCODE_FAIL;
    }

    /* is_no=TRUE → no shutdown → up；is_no=FALSE → shutdown → down */
    int up = is_no ? 1 : 0;
    int old_shutdown = entry->shutdown;

    if (if_set_state(entry->physical_name, up) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: Failed to set interface %s state", entry->physical_name);
        return ERRCODE_FAIL;
    }

    entry->shutdown = up ? 0 : 1;

    if (old_shutdown != entry->shutdown)
    {
        uint32_t if_type = if_cfg_type_to_mask(if_detect_type(entry->physical_name));
        uint32_t event = up ? IF_EVENT_UP : IF_EVENT_DOWN;
        if (if_type != 0)
        {
            if_pub_notify(g_if_local->dev_ipc_ctx, g_if_local->subscribers, entry, if_type, event, (uint8_t)up);
        }

        if (net_prefix_is_set(&entry->prefix))
        {
            if_sync_connected_host_routes(&entry->prefix, up ? FALSE : TRUE);
        }
    }

    LOG_INFO("IF: %s %s", logical_name, up ? "no shutdown" : "shutdown");
    return ERRCODE_SUCCESS;
}
