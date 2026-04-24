/**
 * @file   if_api.c
 * @brief  IF 模块对外 API 实现（缓存 + 订阅）
 * @author jhb
 * @date   2026/04/11
 */
#include "if_api.h"

#include <net/if.h>
#include <string.h>

#include "errcode.h"
#include "log.h"
#include "route.h"

/* key=entry->logical_name, value=if_api_cache_entry_t* */
static GHashTable *g_if_cache_by_name = NULL;
/* key=GUINT_TO_POINTER(ifindex), value=if_api_cache_entry_t* */
static GHashTable *g_if_cache_by_ifindex = NULL;

static void cache_bind_ifindex(if_api_cache_entry_t *entry, uint32_t ifindex)
{
    if (!entry || !g_if_cache_by_ifindex)
    {
        return;
    }

    if (entry->ifindex != 0 && entry->ifindex != ifindex)
    {
        g_hash_table_remove(g_if_cache_by_ifindex, GUINT_TO_POINTER(entry->ifindex));
    }

    entry->ifindex = ifindex;
    if (ifindex != 0)
    {
        g_hash_table_insert(g_if_cache_by_ifindex, GUINT_TO_POINTER(ifindex), entry);
    }
}

static void cache_update_ifindex(if_api_cache_entry_t *entry, const char *physical_name, uint32_t ifindex)
{
    if (!entry)
    {
        return;
    }

    if (physical_name && physical_name[0] != '\0')
    {
        g_strlcpy(entry->physical_name, physical_name, sizeof(entry->physical_name));
    }

    uint32_t resolved = ifindex;
    if (resolved == 0 && entry->physical_name[0] != '\0')
    {
        resolved = (uint32_t)if_nametoindex(entry->physical_name);
    }

    cache_bind_ifindex(entry, resolved);
}

static if_api_cache_entry_t *cache_get_or_create(const char *logical_name)
{
    if (!g_if_cache_by_name || !g_if_cache_by_ifindex || !logical_name || logical_name[0] == '\0')
    {
        return NULL;
    }

    if_api_cache_entry_t *entry = (if_api_cache_entry_t *)g_hash_table_lookup(g_if_cache_by_name, logical_name);
    if (entry)
    {
        return entry;
    }

    entry = (if_api_cache_entry_t *)g_malloc0(sizeof(*entry));
    if (!entry)
    {
        return NULL;
    }

    g_strlcpy(entry->logical_name, logical_name, sizeof(entry->logical_name));
    g_hash_table_insert(g_if_cache_by_name, entry->logical_name, entry);
    return entry;
}

static int cache_addr_event_is_ipv6_linklocal(const if_addr_event_msg_t *evt)
{
    return (evt && evt->afi == ROUTE_AFI_IPV6 && (evt->addr_flags & IF_ADDR_FLAG_LINK_LOCAL) != 0u) ? 1 : 0;
}

static void cache_handle_addr_event(const if_addr_event_msg_t *evt)
{
    if (!evt)
    {
        return;
    }

    if_api_cache_entry_t *entry = cache_get_or_create(evt->logical_name);
    if (!entry)
    {
        return;
    }

    cache_update_ifindex(entry, evt->physical_name, evt->ifindex);

    int is_ipv6_linklocal = cache_addr_event_is_ipv6_linklocal(evt);

    if (evt->event == IF_EVENT_PROTO_UP)
    {
        if (evt->afi == ROUTE_AFI_IPV4)
        {
            entry->ipv4_addr = evt->addr;
            entry->ipv4_prefix_len = evt->prefix_len;
        }
        else if (evt->afi == ROUTE_AFI_IPV6)
        {
            if (is_ipv6_linklocal)
            {
                entry->ipv6_linklocal_addr = evt->addr;
                entry->ipv6_linklocal_prefix_len = evt->prefix_len;
            }
            else
            {
                entry->ipv6_addr = evt->addr;
                entry->ipv6_prefix_len = evt->prefix_len;
            }
        }
    }
    else if (evt->event == IF_EVENT_PROTO_DOWN)
    {
        if (evt->afi == ROUTE_AFI_IPV4)
        {
            memset(&entry->ipv4_addr, 0, sizeof(entry->ipv4_addr));
            entry->ipv4_prefix_len = 0;
        }
        else if (evt->afi == ROUTE_AFI_IPV6)
        {
            if (is_ipv6_linklocal)
            {
                memset(&entry->ipv6_linklocal_addr, 0, sizeof(entry->ipv6_linklocal_addr));
                entry->ipv6_linklocal_prefix_len = 0;
            }
            else
            {
                memset(&entry->ipv6_addr, 0, sizeof(entry->ipv6_addr));
                entry->ipv6_prefix_len = 0;
            }
        }
    }

    if (!is_ipv6_linklocal)
    {
        /* 协议状态仍由原有的 IPv4/IPv6 主地址事件驱动。 */
        entry->proto_up = (evt->event == IF_EVENT_PROTO_UP) ? 1u : 0u;
    }
}

static void cache_handle_up_down_event(const if_event_msg_t *evt)
{
    if (!evt)
    {
        return;
    }

    if_api_cache_entry_t *entry = cache_get_or_create(evt->logical_name);
    if (!entry)
    {
        return;
    }

    entry->link_up = evt->link_up ? 1u : 0u;
    cache_update_ifindex(entry, evt->physical_name, 0);
}

void if_api_cache_init(void)
{
    if (g_if_cache_by_name)
    {
        return;
    }

    g_if_cache_by_name = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    g_if_cache_by_ifindex = g_hash_table_new(g_direct_hash, g_direct_equal);
    LOG_INFO("IF API cache initialized");
}

void if_api_cache_cleanup(void)
{
    if (!g_if_cache_by_name)
    {
        return;
    }

    if (g_if_cache_by_ifindex)
    {
        g_hash_table_destroy(g_if_cache_by_ifindex);
        g_if_cache_by_ifindex = NULL;
    }

    g_hash_table_destroy(g_if_cache_by_name);
    g_if_cache_by_name = NULL;
}

void if_api_cache_on_event(const dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || !g_if_cache_by_name)
    {
        return;
    }

    if (msg->payload_len >= sizeof(if_addr_event_msg_t))
    {
        const if_addr_event_msg_t *addr_evt = (const if_addr_event_msg_t *)msg->payload;
        if (addr_evt->event == IF_EVENT_PROTO_UP || addr_evt->event == IF_EVENT_PROTO_DOWN)
        {
            cache_handle_addr_event(addr_evt);
            return;
        }
    }

    if (msg->payload_len >= sizeof(if_event_msg_t))
    {
        const if_event_msg_t *evt = (const if_event_msg_t *)msg->payload;
        if (evt->event == IF_EVENT_LINK_UP || evt->event == IF_EVENT_LINK_DOWN)
        {
            cache_handle_up_down_event(evt);
        }
    }
}

const if_api_cache_entry_t *if_api_cache_lookup(const char *logical_name)
{
    if (!g_if_cache_by_name || !logical_name || logical_name[0] == '\0')
    {
        return NULL;
    }

    return (const if_api_cache_entry_t *)g_hash_table_lookup(g_if_cache_by_name, logical_name);
}

void if_api_cache_foreach(if_api_cache_iter_fn iter_fn, void *user_data)
{
    if (!g_if_cache_by_name || !iter_fn)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_if_cache_by_name);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        if (iter_fn((const if_api_cache_entry_t *)value, user_data))
        {
            break;
        }
    }
}

uint32_t if_api_cache_get_ifindex(const char *logical_name)
{
    if_api_cache_entry_t *entry = (if_api_cache_entry_t *)if_api_cache_lookup(logical_name);
    if (!entry)
    {
        return 0;
    }

    if (entry->ifindex == 0 && entry->physical_name[0] != '\0')
    {
        cache_update_ifindex(entry, entry->physical_name, 0);
    }

    return entry->ifindex;
}

const char *if_api_cache_get_logical_name(uint32_t ifindex)
{
    if (!g_if_cache_by_ifindex || ifindex == 0)
    {
        return NULL;
    }

    const if_api_cache_entry_t *entry =
        (const if_api_cache_entry_t *)g_hash_table_lookup(g_if_cache_by_ifindex, GUINT_TO_POINTER(ifindex));
    return entry ? entry->logical_name : NULL;
}

void if_api_cache_hint_ifindex(const char *logical_name, uint32_t ifindex)
{
    if (!logical_name || logical_name[0] == '\0' || ifindex == 0)
    {
        return;
    }

    if_api_cache_entry_t *entry = cache_get_or_create(logical_name);
    if (!entry)
    {
        return;
    }

    cache_update_ifindex(entry, NULL, ifindex);
}

int if_api_subscribe(dev_ipc_context_t *ctx, uint32_t if_type_mask, uint32_t event_mask, uint32_t flags)
{
    if (!ctx || if_type_mask == 0 || event_mask == 0)
    {
        return ERRCODE_FAIL;
    }

    if_subscribe_req_t *req = (if_subscribe_req_t *)g_malloc0(sizeof(*req));
    if (!req)
    {
        return ERRCODE_FAIL;
    }

    req->if_type_mask = if_type_mask;
    req->event_mask = event_mask;
    req->flags = flags;

    dev_ipc_message_t *msg = dev_ipc_message_create(IF_MSG_TYPE_SUBSCRIBE, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_IF,
                                                    0, req, sizeof(*req), g_free);
    if (!msg)
    {
        g_free(req);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_send(ctx, DEV_MODULE_ID_IF, msg);
    dev_ipc_message_free(msg);
    return (ret == ERRCODE_SUCCESS) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int if_api_subscribe_all(dev_ipc_context_t *ctx)
{
    return if_api_subscribe(ctx, IF_INTF_TYPE_ALL, IF_EVENT_ALL, 0);
}
