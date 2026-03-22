/**
 * @file   route_api.c
 * @brief  Route 模块对外 API 实现
 * @author jhb
 * @date   2026/03/15
 */
#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "route.h"

int route_rpc_inject(dev_ipc_context_t *ctx, const route_msg_entry_t *entry)
{
    if (!ctx || !entry)
    {
        return ERRCODE_FAIL;
    }

    route_msg_entry_t *payload = (route_msg_entry_t *)g_memdup2(entry, sizeof(route_msg_entry_t));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_INJECT, dev_ipc_get_module_id(ctx),
                                                    DEV_MODULE_ID_ROUTE, 0, payload, sizeof(route_msg_entry_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, msg);
    dev_ipc_message_free(msg);
    return (ret == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int route_rpc_add(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr,
                  uint8_t prefix_len, uint32_t protocol, const net_addr_t *source, const net_addr_t *nexthop,
                  int32_t metric, int32_t preference, uint32_t out_ifindex)
{
    if (!prefix_addr || !source || !nexthop)
    {
        return ERRCODE_FAIL;
    }

    route_msg_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.vrf_id = vrf_id;
    entry.afi = afi;
    entry.safi = ROUTE_SAFI_UNICAST;
    entry.prefix_len = prefix_len;
    entry.protocol = protocol;
    entry.metric = metric;
    entry.preference = preference;
    entry.is_withdraw = 0;
    entry.flags = 0;
    entry.out_ifindex = out_ifindex;
    entry.prefix_addr = *prefix_addr;
    entry.source_addr = *source;
    entry.nexthop_addr = *nexthop;

    return route_rpc_inject(ctx, &entry);
}

static int route_rpc_nh_iter_send(dev_ipc_context_t *ctx, uint32_t msg_type, const route_nh_iter_req_t *req)
{
    if (!ctx || !req)
    {
        return ERRCODE_FAIL;
    }

    route_nh_iter_req_t *payload = (route_nh_iter_req_t *)g_memdup2(req, sizeof(route_nh_iter_req_t));
    if (!payload)
    {
        return ERRCODE_FAIL;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(msg_type, dev_ipc_get_module_id(ctx), DEV_MODULE_ID_ROUTE, 0,
                                                    payload, sizeof(route_nh_iter_req_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, msg);
    dev_ipc_message_free(msg);
    return (ret == 0) ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

int route_rpc_nh_register(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop)
{
    if (!nexthop)
    {
        return ERRCODE_FAIL;
    }

    route_nh_iter_req_t req;
    memset(&req, 0, sizeof(req));
    req.vrf_id = vrf_id;
    req.afi = afi;
    req.safi = ROUTE_SAFI_UNICAST;
    req.nexthop_addr = *nexthop;

    return route_rpc_nh_iter_send(ctx, ROUTE_MSG_TYPE_NH_REGISTER, &req);
}

int route_rpc_del(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr,
                  uint8_t prefix_len, uint32_t protocol, const net_addr_t *source, uint32_t out_ifindex)
{
    if (!prefix_addr || !source)
    {
        return ERRCODE_FAIL;
    }

    route_msg_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.vrf_id = vrf_id;
    entry.afi = afi;
    entry.safi = ROUTE_SAFI_UNICAST;
    entry.prefix_len = prefix_len;
    entry.protocol = protocol;
    entry.metric = 0;
    entry.preference = 0;
    entry.is_withdraw = 1;
    entry.flags = 0;
    entry.out_ifindex = out_ifindex;
    entry.prefix_addr = *prefix_addr;
    entry.source_addr = *source;
    entry.nexthop_addr.family = source->family;

    return route_rpc_inject(ctx, &entry);
}

int route_rpc_nh_unregister(dev_ipc_context_t *ctx, uint32_t vrf_id, uint16_t afi, const net_addr_t *nexthop)
{
    if (!nexthop)
    {
        return ERRCODE_FAIL;
    }

    route_nh_iter_req_t req;
    memset(&req, 0, sizeof(req));
    req.vrf_id = vrf_id;
    req.afi = afi;
    req.safi = ROUTE_SAFI_UNICAST;
    req.nexthop_addr = *nexthop;

    return route_rpc_nh_iter_send(ctx, ROUTE_MSG_TYPE_NH_UNREGISTER, &req);
}
