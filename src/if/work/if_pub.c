/**
 * @file   if_pub.c
 * @brief  IF 模块事件发布实现
 * @author jhb
 * @date   2026/03/15
 */
#include "if_pub.h"

#include <string.h>

#include "if_event.h"
#include "if_main.h"
#include "log.h"
#include "route.h"

static int subscriber_matches(const if_subscriber_t *sub, uint32_t if_type, uint32_t event)
{
    if (!sub)
    {
        return 0;
    }

    if ((sub->if_type_mask & if_type) == 0)
    {
        return 0;
    }

    if ((sub->event_mask & event) == 0)
    {
        return 0;
    }

    return 1;
}

void if_pub_notify(GList *subscribers, const if_map_entry_t *entry, uint32_t if_type, uint32_t event, uint8_t link_up,
                   const net_prefix_t *prefix, uint32_t out_ifindex)
{
    if (!entry || if_type == 0 || event == 0)
    {
        return;
    }
    dev_ipc_context_t *ctx = if_local_ipc_ctx();

    int is_proto = ((event & IF_EVENT_PROTO_UP) || (event & IF_EVENT_PROTO_DOWN));
    if (is_proto && !prefix)
    {
        return;
    }

    void *base_ptr = NULL;
    size_t payload_sz = 0;
    if_event_msg_t base_link;
    if_addr_event_msg_t base_proto;

    if (is_proto)
    {
        memset(&base_proto, 0, sizeof(base_proto));
        base_proto.if_type = if_type;
        base_proto.event = event;
        g_strlcpy(base_proto.logical_name, entry->logical_name, sizeof(base_proto.logical_name));
        g_strlcpy(base_proto.physical_name, entry->physical_name, sizeof(base_proto.physical_name));
        base_proto.afi = (prefix->addr.family == AF_INET6) ? ROUTE_AFI_IPV6 : ROUTE_AFI_IPV4;
        base_proto.prefix_len = prefix->prefix_len;
        if (prefix->addr.family == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(&prefix->addr.u.v6))
        {
            base_proto.addr_flags |= IF_ADDR_FLAG_LINK_LOCAL;
        }
        base_proto.addr = prefix->addr;
        base_proto.ifindex = out_ifindex;

        base_ptr = &base_proto;
        payload_sz = sizeof(base_proto);
    }
    else
    {
        memset(&base_link, 0, sizeof(base_link));
        base_link.if_type = if_type;
        base_link.event = event;
        base_link.link_up = link_up ? 1 : 0;
        g_strlcpy(base_link.logical_name, entry->logical_name, sizeof(base_link.logical_name));
        g_strlcpy(base_link.physical_name, entry->physical_name, sizeof(base_link.physical_name));

        base_ptr = &base_link;
        payload_sz = sizeof(base_link);
    }

    for (GList *l = subscribers; l; l = l->next)
    {
        if_subscriber_t *sub = (if_subscriber_t *)l->data;
        if (!subscriber_matches(sub, if_type, event))
        {
            continue;
        }

        void *payload = g_memdup2(base_ptr, payload_sz);
        if (!payload)
        {
            continue;
        }

        dev_ipc_message_t *msg = dev_ipc_message_create(IF_MSG_TYPE_EVENT, DEV_MODULE_ID_IF, sub->module_id, 0, payload,
                                                        (uint32_t)payload_sz, g_free);
        if (!msg)
        {
            g_free(payload);
            continue;
        }

        if (dev_ipc_send(ctx, sub->module_id, msg) != 0)
        {
            LOG_WARN("IF: Failed to send event to module 0x%08X", sub->module_id);
        }
        dev_ipc_message_free(msg);
    }
}
