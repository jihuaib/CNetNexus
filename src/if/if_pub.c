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

void if_pub_notify(GList *subscribers, const if_map_entry_t *entry, uint32_t if_type, uint32_t event, uint8_t admin_up)
{
    if (!entry || if_type == 0 || event == 0)
    {
        return;
    }
    dev_ipc_context_t *ctx = if_local_ipc_ctx();

    if_event_msg_t base;
    memset(&base, 0, sizeof(base));
    base.if_type = if_type;
    base.event = event;
    base.admin_up = admin_up ? 1 : 0;
    g_strlcpy(base.logical_name, entry->logical_name, sizeof(base.logical_name));
    g_strlcpy(base.physical_name, entry->physical_name, sizeof(base.physical_name));

    for (GList *l = subscribers; l; l = l->next)
    {
        if_subscriber_t *sub = (if_subscriber_t *)l->data;
        if (!subscriber_matches(sub, if_type, event))
        {
            continue;
        }

        if_event_msg_t *payload = (if_event_msg_t *)g_memdup2(&base, sizeof(base));
        if (!payload)
        {
            continue;
        }

        dev_ipc_message_t *msg = dev_ipc_message_create(IF_MSG_TYPE_EVENT, DEV_MODULE_ID_IF, sub->module_id, 0, payload,
                                                        sizeof(if_event_msg_t), g_free);
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

void if_pub_notify_addr(GList *subscribers, const if_map_entry_t *entry, uint32_t if_type, uint32_t event,
                        const net_prefix_t *prefix, uint32_t out_ifindex)
{
    if (!entry || !prefix || if_type == 0 || event == 0)
    {
        return;
    }
    dev_ipc_context_t *ctx = if_local_ipc_ctx();

    if_addr_event_msg_t base;
    memset(&base, 0, sizeof(base));
    base.if_type = if_type;
    base.event = event;
    g_strlcpy(base.logical_name, entry->logical_name, sizeof(base.logical_name));
    g_strlcpy(base.physical_name, entry->physical_name, sizeof(base.physical_name));
    base.afi = (prefix->addr.family == AF_INET6) ? ROUTE_AFI_IPV6 : ROUTE_AFI_IPV4;
    base.prefix_len = prefix->prefix_len;
    base.addr = prefix->addr;
    base.ifindex = out_ifindex;

    for (GList *l = subscribers; l; l = l->next)
    {
        if_subscriber_t *sub = (if_subscriber_t *)l->data;
        if (!subscriber_matches(sub, if_type, event))
        {
            continue;
        }

        if_addr_event_msg_t *payload = (if_addr_event_msg_t *)g_memdup2(&base, sizeof(base));
        if (!payload)
        {
            continue;
        }

        dev_ipc_message_t *msg = dev_ipc_message_create(IF_MSG_TYPE_EVENT, DEV_MODULE_ID_IF, sub->module_id, 0, payload,
                                                        sizeof(if_addr_event_msg_t), g_free);
        if (!msg)
        {
            g_free(payload);
            continue;
        }

        if (dev_ipc_send(ctx, sub->module_id, msg) != 0)
        {
            LOG_WARN("IF: Failed to send addr event to module 0x%08X", sub->module_id);
        }
        dev_ipc_message_free(msg);
    }
}
