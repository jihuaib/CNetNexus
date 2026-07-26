/**
 * @file   if_pub.c
 * @brief  IF 模块事件发布实现
 * @author jhb
 * @date   2026/03/15
 */
#include "if_pub.h"

#include <arpa/inet.h>
#include <string.h>

#include "errcode.h"
#include "if.h"
#include "if_main.h"
#include "if_netlink.h"
#include "if_worker.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "snmp.h"
#include "syslog_report.h"

static const char *if_event_name(uint32_t event)
{
    switch (event)
    {
        case IF_EVENT_LINK_UP:
            return "link-up";
        case IF_EVENT_LINK_DOWN:
            return "link-down";
        case IF_EVENT_PROTO_UP:
            return "proto-up";
        case IF_EVENT_PROTO_DOWN:
            return "proto-down";
        case IF_EVENT_VRF_CHANGE:
            return "vrf-change";
        default:
            return "unknown";
    }
}

static void if_pub_syslog_event(const if_map_entry_t *entry, uint32_t event, uint8_t link_up,
                                const net_prefix_t *prefix, uint32_t out_ifindex)
{
    if (!entry)
    {
        return;
    }

    const char *vrf = (entry->vrf_name[0] != '\0') ? entry->vrf_name : "public";
    const char *evt = if_event_name(event);
    syslog_report_severity_t sev =
        (event == IF_EVENT_LINK_DOWN || event == IF_EVENT_PROTO_DOWN) ? SYSLOG_REPORT_WARNING : SYSLOG_REPORT_NOTICE;

    if ((event == IF_EVENT_PROTO_UP || event == IF_EVENT_PROTO_DOWN) && prefix)
    {
        char addr[INET6_ADDRSTRLEN] = {0};
        net_addr_to_str(&prefix->addr, addr, sizeof(addr));
        syslog_report(sev, "if", evt, "interface=%s physical=%s vrf=%s addr=%s/%u ifindex=%u", entry->logical_name,
                      entry->physical_name, vrf, addr, (unsigned)prefix->prefix_len, (unsigned)out_ifindex);
        return;
    }

    syslog_report(sev, "if", evt, "interface=%s physical=%s vrf=%s link=%s ifindex=%u", entry->logical_name,
                  entry->physical_name, vrf, link_up ? "up" : "down", (unsigned)out_ifindex);
}

static uint32_t if_pub_snmp_ifindex(const if_map_entry_t *entry, uint32_t out_ifindex)
{
    if (out_ifindex != 0)
    {
        return out_ifindex;
    }
    if (!entry || entry->logical_name[0] == '\0')
    {
        return 10000;
    }
    return 10000u + (g_str_hash(entry->logical_name) % 50000u);
}

static uint8_t if_pub_snmp_admin_status(const if_map_entry_t *entry)
{
    return (entry && !entry->shutdown) ? 1u : 2u;
}

static uint8_t if_pub_snmp_oper_status(const if_map_entry_t *entry, uint8_t event_link_up)
{
    if (!entry || entry->shutdown)
    {
        return 2u;
    }
    if (if_map_is_virtual_entry(entry->logical_name))
    {
        return 1u;
    }
    if (entry->link_up >= 0)
    {
        return entry->link_up ? 1u : 2u;
    }

    if_info_t info;
    if (entry->physical_name[0] != '\0' && if_get_info(entry->physical_name, &info) == ERRCODE_SUCCESS)
    {
        return (info.state == IF_STATE_UP) ? 1u : 2u;
    }
    return event_link_up ? 1u : 2u;
}

static void if_pub_snmp_value(dev_ipc_context_t *ctx, const char *oid, snmp_value_type_t type, const char *value)
{
    if (!ctx || !oid || !value || !dev_ipc_is_connected(ctx, DEV_MODULE_ID_SNMP))
    {
        return;
    }

    snmp_value_msg_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.owner_module_id = DEV_MODULE_ID_IF;
    payload.value_type = (uint32_t)type;
    g_strlcpy(payload.oid, oid, sizeof(payload.oid));
    g_strlcpy(payload.value, value, sizeof(payload.value));

    snmp_value_msg_t *dup = (snmp_value_msg_t *)g_memdup2(&payload, sizeof(payload));
    if (!dup)
    {
        return;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(SNMP_MSG_TYPE_VALUE_SET, DEV_MODULE_ID_IF, DEV_MODULE_ID_SNMP, 0,
                                                    dup, sizeof(*dup), g_free);
    if (!msg)
    {
        g_free(dup);
        return;
    }

    if (dev_ipc_send(ctx, DEV_MODULE_ID_SNMP, msg) != 0)
    {
        LOG_DEBUG("IF: skip SNMP value report oid=%s", oid);
    }
    dev_ipc_message_free(msg);
}

static void if_pub_snmp_if_table(dev_ipc_context_t *ctx, const if_map_entry_t *entry, uint8_t link_up,
                                 uint32_t out_ifindex)
{
    if (!ctx || !entry)
    {
        return;
    }

    const uint32_t ifindex = if_pub_snmp_ifindex(entry, out_ifindex);
    char oid[SNMP_OID_MAX_LEN];
    char value[SNMP_VALUE_MAX_LEN];
    const uint8_t admin_status = if_pub_snmp_admin_status(entry);
    const uint8_t oper_status = if_pub_snmp_oper_status(entry, link_up);

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.2.2.1.1.%u", ifindex);
    snprintf(value, sizeof(value), "%u", ifindex);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_INTEGER, value);

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.2.2.1.2.%u", ifindex);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_STRING, entry->logical_name);

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.31.1.1.1.1.%u", ifindex);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_STRING, entry->logical_name);

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.2.2.1.3.%u", ifindex);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_INTEGER, "6");

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.2.2.1.4.%u", ifindex);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_INTEGER, "1500");

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.2.2.1.5.%u", ifindex);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_GAUGE, "1000000000");

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.2.2.1.7.%u", ifindex);
    snprintf(value, sizeof(value), "%u", (unsigned)admin_status);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_INTEGER, value);

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.2.2.1.8.%u", ifindex);
    snprintf(value, sizeof(value), "%u", (unsigned)oper_status);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_INTEGER, value);

    snprintf(oid, sizeof(oid), ".1.3.6.1.2.1.2.2.1.9.%u", ifindex);
    if_pub_snmp_value(ctx, oid, SNMP_VALUE_TIMETICKS, "0");
}

static gboolean if_pub_snmp_refresh_entry_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    dev_ipc_context_t *ctx = (dev_ipc_context_t *)user_data;
    if_map_entry_t *entry = (if_map_entry_t *)value;
    if (!entry || entry->logical_name[0] == '\0')
    {
        return FALSE;
    }
    if_pub_snmp_if_table(ctx, entry, entry->link_up > 0 ? 1u : 0u, entry->ifindex);
    return FALSE;
}

void if_pub_snmp_refresh_all(void)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    if (!ctx || !dev_ipc_is_connected(ctx, DEV_MODULE_ID_SNMP) || !g_if_work_local ||
        !g_if_work_local->interface_map.all_entries)
    {
        return;
    }
    g_tree_foreach(g_if_work_local->interface_map.all_entries, if_pub_snmp_refresh_entry_cb, ctx);
}

static void if_pub_snmp_trap(dev_ipc_context_t *ctx, const if_map_entry_t *entry, uint32_t event, uint8_t link_up,
                             uint32_t out_ifindex)
{
    if (!ctx || !entry || !dev_ipc_is_connected(ctx, DEV_MODULE_ID_SNMP))
    {
        return;
    }
    if (event != IF_EVENT_LINK_UP && event != IF_EVENT_LINK_DOWN)
    {
        return;
    }

    const uint32_t ifindex = if_pub_snmp_ifindex(entry, out_ifindex);
    snmp_trap_msg_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.owner_module_id = DEV_MODULE_ID_IF;
    g_strlcpy(payload.trap_oid, event == IF_EVENT_LINK_DOWN ? ".1.3.6.1.6.3.1.1.5.3" : ".1.3.6.1.6.3.1.1.5.4",
              sizeof(payload.trap_oid));
    payload.var_count = 3;

    snprintf(payload.vars[0].oid, sizeof(payload.vars[0].oid), ".1.3.6.1.2.1.2.2.1.1.%u", ifindex);
    payload.vars[0].value_type = SNMP_VALUE_INTEGER;
    snprintf(payload.vars[0].value, sizeof(payload.vars[0].value), "%u", ifindex);

    snprintf(payload.vars[1].oid, sizeof(payload.vars[1].oid), ".1.3.6.1.2.1.2.2.1.2.%u", ifindex);
    payload.vars[1].value_type = SNMP_VALUE_STRING;
    g_strlcpy(payload.vars[1].value, entry->logical_name, sizeof(payload.vars[1].value));

    snprintf(payload.vars[2].oid, sizeof(payload.vars[2].oid), ".1.3.6.1.2.1.2.2.1.8.%u", ifindex);
    payload.vars[2].value_type = SNMP_VALUE_INTEGER;
    g_strlcpy(payload.vars[2].value, link_up ? "1" : "2", sizeof(payload.vars[2].value));

    snmp_trap_msg_t *dup = (snmp_trap_msg_t *)g_memdup2(&payload, sizeof(payload));
    if (!dup)
    {
        return;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(SNMP_MSG_TYPE_TRAP_SEND, DEV_MODULE_ID_IF, DEV_MODULE_ID_SNMP, 0,
                                                    dup, sizeof(*dup), g_free);
    if (!msg)
    {
        g_free(dup);
        return;
    }

    if (dev_ipc_send(ctx, DEV_MODULE_ID_SNMP, msg) != 0)
    {
        LOG_DEBUG("IF: skip SNMP trap report if=%s", entry->logical_name);
    }
    dev_ipc_message_free(msg);
}

static void if_pub_snmp_event(dev_ipc_context_t *ctx, const if_map_entry_t *entry, uint32_t event, uint8_t link_up,
                              uint32_t out_ifindex)
{
    if (!ctx || !entry)
    {
        return;
    }

    if_pub_snmp_if_table(ctx, entry, link_up, out_ifindex);
    if_pub_snmp_trap(ctx, entry, event, link_up, out_ifindex);
}

void if_pub_snmp_admin_state_change(const if_map_entry_t *entry, uint8_t admin_up, uint32_t out_ifindex)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    if (!ctx || !entry)
    {
        return;
    }

    const uint8_t link_up = admin_up ? 1u : 0u;
    const uint32_t event = admin_up ? IF_EVENT_LINK_UP : IF_EVENT_LINK_DOWN;
    if_pub_snmp_if_table(ctx, entry, link_up, out_ifindex);
    if_pub_snmp_trap(ctx, entry, event, link_up, out_ifindex);
}

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
        g_strlcpy(base_proto.vrf_name, entry->vrf_name, sizeof(base_proto.vrf_name));
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
        g_strlcpy(base_link.vrf_name, entry->vrf_name, sizeof(base_link.vrf_name));

        base_ptr = &base_link;
        payload_sz = sizeof(base_link);
    }

    if_pub_syslog_event(entry, event, link_up, prefix, out_ifindex);
    if_pub_snmp_event(ctx, entry, event, link_up, out_ifindex);

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
