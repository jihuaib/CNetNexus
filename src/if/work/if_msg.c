/**
 * @file   if_msg.c
 * @brief  IF worker 线程订阅/查询消息处理（subscribe/unsubscribe/get_intf_map + 初始状态回放）
 * @author jhb
 * @date   2026/04/21
 */
#include "if_msg.h"

#include <glib.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "if.h"
#include "if_main.h"
#include "if_netlink.h"
#include "if_pub.h"
#include "if_worker.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

static void send_if_ack(dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    if (!ctx || !msg)
    {
        return;
    }

    if_msg_ack_t *ack = (if_msg_ack_t *)g_malloc(sizeof(if_msg_ack_t));
    if (!ack)
    {
        dev_ipc_message_free(msg);
        return;
    }
    ack->result = result;

    dev_ipc_message_t *resp = dev_ipc_message_create(IF_MSG_TYPE_ACK, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, ack, sizeof(if_msg_ack_t), g_free);
    if (!resp)
    {
        g_free(ack);
        dev_ipc_message_free(msg);
        return;
    }

    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
    dev_ipc_message_free(msg);
}

typedef struct if_replay_ctx
{
    uint32_t module_id;
    uint32_t if_type_mask;
    uint32_t event_mask;
} if_replay_ctx_t;

static void if_replay_send(uint32_t module_id, uint32_t msg_type, void *payload, uint32_t payload_len)
{
    dev_ipc_context_t *ctx = if_local_ipc_ctx();
    void *dup = g_memdup2(payload, payload_len);
    if (!dup)
    {
        return;
    }
    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, DEV_MODULE_ID_IF, module_id, 0, dup, payload_len, g_free);
    if (!msg)
    {
        g_free(dup);
        return;
    }
    if (dev_ipc_send(ctx, module_id, msg) != 0)
    {
        LOG_WARN("IF: Failed to send replay event to module 0x%08X", module_id);
    }
    dev_ipc_message_free(msg);
}

static gboolean if_replay_initial_state_foreach(gpointer key, gpointer val, gpointer user_data)
{
    (void)key;
    if_map_entry_t *e = (if_map_entry_t *)val;
    if_replay_ctx_t *rctx = (if_replay_ctx_t *)user_data;

    if_type_t raw_type = if_detect_type(e->physical_name);
    uint32_t if_type = 0;
    if (raw_type == IF_TYPE_ETHERNET || raw_type == IF_TYPE_VETH)
    {
        if_type = IF_INTF_TYPE_ETH;
    }
    if (if_type == 0 || (rctx->if_type_mask & if_type) == 0)
    {
        return FALSE;
    }

    uint32_t replay_ifindex = e->ifindex;
    if (replay_ifindex == 0u && strcmp(e->logical_name, "null0") != 0)
    {
        replay_ifindex = (uint32_t)if_nametoindex(e->physical_name);
        if (replay_ifindex != 0u)
        {
            e->ifindex = replay_ifindex;
        }
    }
    if (replay_ifindex == 0u && strcmp(e->logical_name, "null0") != 0 &&
        (net_prefix_is_set(&e->prefix_v4) || net_prefix_is_set(&e->prefix_v6)))
    {
        LOG_WARN("IF: skip replay addr event for %s, ifindex invalid(0)", e->logical_name);
    }

    if ((rctx->event_mask & (IF_EVENT_LINK_UP | IF_EVENT_LINK_DOWN)) != 0)
    {
        uint32_t event = (replay_ifindex != 0u) ? IF_EVENT_LINK_UP : IF_EVENT_LINK_DOWN;
        if ((rctx->event_mask & event) != 0)
        {
            if_event_msg_t evt;
            memset(&evt, 0, sizeof(evt));
            evt.if_type = if_type;
            evt.event = event;
            evt.link_up = (replay_ifindex != 0u) ? 1 : 0;
            g_strlcpy(evt.logical_name, e->logical_name, sizeof(evt.logical_name));
            g_strlcpy(evt.physical_name, e->physical_name, sizeof(evt.physical_name));
            g_strlcpy(evt.vrf_name, e->vrf_name, sizeof(evt.vrf_name));
            if_replay_send(rctx->module_id, IF_MSG_TYPE_EVENT, &evt, sizeof(evt));
        }
    }

    if ((rctx->event_mask & IF_EVENT_VRF_CHANGE) != 0)
    {
        if_event_msg_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.if_type = if_type;
        evt.event = IF_EVENT_VRF_CHANGE;
        evt.link_up = (replay_ifindex != 0u) ? 1 : 0;
        g_strlcpy(evt.logical_name, e->logical_name, sizeof(evt.logical_name));
        g_strlcpy(evt.physical_name, e->physical_name, sizeof(evt.physical_name));
        g_strlcpy(evt.vrf_name, e->vrf_name, sizeof(evt.vrf_name));
        if_replay_send(rctx->module_id, IF_MSG_TYPE_EVENT, &evt, sizeof(evt));
    }

    if ((rctx->event_mask & IF_EVENT_PROTO_UP) != 0 && replay_ifindex != 0u && !e->shutdown && g_if_work_local &&
        g_if_work_local->route_ready && net_prefix_is_set(&e->prefix_v4))
    {
        if_addr_event_msg_t addr_evt;
        memset(&addr_evt, 0, sizeof(addr_evt));
        addr_evt.if_type = if_type;
        addr_evt.event = IF_EVENT_PROTO_UP;
        g_strlcpy(addr_evt.logical_name, e->logical_name, sizeof(addr_evt.logical_name));
        g_strlcpy(addr_evt.physical_name, e->physical_name, sizeof(addr_evt.physical_name));
        g_strlcpy(addr_evt.vrf_name, e->vrf_name, sizeof(addr_evt.vrf_name));
        addr_evt.afi = ROUTE_AFI_IPV4;
        addr_evt.prefix_len = e->prefix_v4.prefix_len;
        addr_evt.addr = e->prefix_v4.addr;
        addr_evt.ifindex = replay_ifindex;
        if_replay_send(rctx->module_id, IF_MSG_TYPE_EVENT, &addr_evt, sizeof(addr_evt));
    }

    if ((rctx->event_mask & IF_EVENT_PROTO_UP) != 0 && replay_ifindex != 0u && !e->shutdown && g_if_work_local &&
        g_if_work_local->route_ready && net_prefix_is_set(&e->prefix_v6))
    {
        if_addr_event_msg_t addr_evt;
        memset(&addr_evt, 0, sizeof(addr_evt));
        addr_evt.if_type = if_type;
        addr_evt.event = IF_EVENT_PROTO_UP;
        g_strlcpy(addr_evt.logical_name, e->logical_name, sizeof(addr_evt.logical_name));
        g_strlcpy(addr_evt.physical_name, e->physical_name, sizeof(addr_evt.physical_name));
        g_strlcpy(addr_evt.vrf_name, e->vrf_name, sizeof(addr_evt.vrf_name));
        addr_evt.afi = ROUTE_AFI_IPV6;
        addr_evt.prefix_len = e->prefix_v6.prefix_len;
        addr_evt.addr = e->prefix_v6.addr;
        addr_evt.ifindex = replay_ifindex;
        if_replay_send(rctx->module_id, IF_MSG_TYPE_EVENT, &addr_evt, sizeof(addr_evt));
    }

    if ((rctx->event_mask & IF_EVENT_PROTO_UP) != 0 && replay_ifindex != 0u &&
        net_prefix_is_set(&e->prefix_v6_linklocal))
    {
        if_addr_event_msg_t addr_evt;
        memset(&addr_evt, 0, sizeof(addr_evt));
        addr_evt.if_type = if_type;
        addr_evt.event = IF_EVENT_PROTO_UP;
        g_strlcpy(addr_evt.logical_name, e->logical_name, sizeof(addr_evt.logical_name));
        g_strlcpy(addr_evt.physical_name, e->physical_name, sizeof(addr_evt.physical_name));
        g_strlcpy(addr_evt.vrf_name, e->vrf_name, sizeof(addr_evt.vrf_name));
        addr_evt.afi = ROUTE_AFI_IPV6;
        addr_evt.prefix_len = e->prefix_v6_linklocal.prefix_len;
        addr_evt.addr_flags = IF_ADDR_FLAG_LINK_LOCAL;
        addr_evt.addr = e->prefix_v6_linklocal.addr;
        addr_evt.ifindex = replay_ifindex;
        if_replay_send(rctx->module_id, IF_MSG_TYPE_EVENT, &addr_evt, sizeof(addr_evt));
    }

    return FALSE;
}

/* 单独投递一条平滑同步标记事件：marker 不受 if_type_mask / event_mask 限制——
 * 订阅者无需 opt-in；REPLAY 路径强制框架。 */
static void if_replay_send_marker(uint32_t module_id, uint32_t event)
{
    if_event_msg_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.if_type = IF_INTF_TYPE_ALL;
    evt.event = event;
    if_replay_send(module_id, IF_MSG_TYPE_EVENT, &evt, sizeof(evt));
}

static void if_replay_initial_state(uint32_t module_id, uint32_t if_type_mask, uint32_t event_mask)
{
    if (!g_if_work_local)
    {
        return;
    }

    if_replay_send_marker(module_id, IF_EVENT_SMOOTHSTART);

    if (g_if_work_local->interface_map.all_entries)
    {
        if_replay_ctx_t rctx = {
            .module_id = module_id,
            .if_type_mask = if_type_mask,
            .event_mask = event_mask,
        };
        g_tree_foreach(g_if_work_local->interface_map.all_entries, if_replay_initial_state_foreach, &rctx);
    }

    if_replay_send_marker(module_id, IF_EVENT_SMOOTHEND);
    LOG_INFO("IF: replayed initial state to module 0x%08X (with smoothstart/smoothend)", module_id);
}

void if_msg_flush_pending_replays(void)
{
    if (!g_if_work_local)
    {
        return;
    }
    for (GList *l = g_if_work_local->subscribers; l; l = l->next)
    {
        if_subscriber_t *sub = (if_subscriber_t *)l->data;
        if (!sub || !sub->pending_replay)
        {
            continue;
        }
        LOG_INFO("IF: flushing deferred REPLAY to module 0x%08X (type=0x%08X event=0x%08X)", sub->module_id,
                 sub->if_type_mask, sub->event_mask);
        if_replay_initial_state(sub->module_id, sub->if_type_mask, sub->event_mask);
        sub->pending_replay = 0;
    }
}

void if_msg_handle_subscribe(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    if (!msg->payload || msg->payload_len < sizeof(if_subscribe_req_t))
    {
        LOG_WARN("IF: subscribe payload invalid, len=%u", msg->payload_len);
        send_if_ack(msg, ERRCODE_FAIL);
        return;
    }

    const if_subscribe_req_t *req = (const if_subscribe_req_t *)msg->payload;
    if (req->if_type_mask == 0 || req->event_mask == 0)
    {
        LOG_WARN("IF: subscribe request invalid: type_mask=0x%08X event_mask=0x%08X", req->if_type_mask,
                 req->event_mask);
        send_if_ack(msg, ERRCODE_FAIL);
        return;
    }

    if_subscriber_t *sub = NULL;
    for (GList *l = g_if_work_local->subscribers; l; l = l->next)
    {
        if_subscriber_t *cur = (if_subscriber_t *)l->data;
        if (cur->module_id == msg->src_module_id && cur->if_type_mask == req->if_type_mask &&
            cur->event_mask == req->event_mask)
        {
            LOG_INFO("IF: duplicate subscribe replay: module=0x%08X type=0x%08X event=0x%08X", msg->src_module_id,
                     req->if_type_mask, req->event_mask);
            sub = cur;
            break;
        }
    }

    if (!sub)
    {
        sub = (if_subscriber_t *)g_malloc0(sizeof(if_subscriber_t));
        if (!sub)
        {
            send_if_ack(msg, ERRCODE_FAIL);
            return;
        }
        sub->module_id = msg->src_module_id;
        sub->if_type_mask = req->if_type_mask;
        sub->event_mask = req->event_mask;
        g_if_work_local->subscribers = g_list_append(g_if_work_local->subscribers, sub);

        LOG_INFO("IF: module 0x%08X subscribed: type=0x%08X event=0x%08X", msg->src_module_id, req->if_type_mask,
                 req->event_mask);
    }

    if (if_worker_is_restore_done())
    {
        if_replay_initial_state(msg->src_module_id, req->if_type_mask, req->event_mask);
    }
    else
    {
        sub->pending_replay = 1;
        LOG_INFO("IF: defer REPLAY to module 0x%08X until db restore done", msg->src_module_id);
    }

    send_if_ack(msg, ERRCODE_SUCCESS);
}

void if_msg_handle_unsubscribe(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    if (!msg->payload || msg->payload_len < sizeof(if_subscribe_req_t))
    {
        LOG_WARN("IF: unsubscribe payload invalid, len=%u", msg->payload_len);
        send_if_ack(msg, ERRCODE_FAIL);
        return;
    }

    const if_subscribe_req_t *req = (const if_subscribe_req_t *)msg->payload;
    uint32_t type_mask = req->if_type_mask;
    uint32_t event_mask = req->event_mask;

    int removed = 0;
    GList *l = g_if_work_local->subscribers;
    while (l)
    {
        if_subscriber_t *sub = (if_subscriber_t *)l->data;
        GList *next = l->next;

        if (sub->module_id == msg->src_module_id)
        {
            gboolean match_exact = (sub->if_type_mask == type_mask && sub->event_mask == event_mask);
            gboolean clear_all = (type_mask == 0 && event_mask == 0);
            if (clear_all || match_exact)
            {
                g_if_work_local->subscribers = g_list_delete_link(g_if_work_local->subscribers, l);
                g_free(sub);
                removed++;
            }
        }
        l = next;
    }

    LOG_INFO("IF: module 0x%08X unsubscribed, removed=%d (type=0x%08X event=0x%08X)", msg->src_module_id, removed,
             type_mask, event_mask);

    send_if_ack(msg, ERRCODE_SUCCESS);
}
