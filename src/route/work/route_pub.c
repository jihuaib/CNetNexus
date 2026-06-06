/**
 * @file   route_pub.c
 * @brief  Route 模块发布/subscribe实现
 * @author jhb
 * @date   2026/02/01
 */
#include "route_pub.h"

#include <stdlib.h>
#include <string.h>

#include "errcode.h"
#include "log.h"
#include "route.h"
#include "route_main.h"
#include "route_nhobj.h"
#include "route_rib.h"
#include "route_worker.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 将 route_head_t + route_path_t 转换为 route_msg_entry_t（直接二进制拷贝）
 */
static void build_entry(route_msg_entry_t *entry, const route_head_t *head, const route_path_t *path, int is_withdraw)
{
    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = head->key.vrf_id;
    entry->afi = head->key.afi;
    entry->safi = ROUTE_SAFI_UNICAST;
    entry->prefix_len = head->key.prefix_len;
    entry->protocol = path->key.protocol;
    entry->metric = path->metric;
    entry->preference = path->preference;
    entry->is_withdraw = (uint8_t)is_withdraw;
    entry->flags = (uint8_t)(path->entry_flags & 0xFFu);
    entry->nh_type = path->nh_type ? path->nh_type : ROUTE_NH_TYPE_IP;
    entry->tunnel_id = (entry->nh_type == ROUTE_NH_TYPE_TUNNEL) ? path->tunnel_id : 0u;
    entry->out_label = (entry->nh_type == ROUTE_NH_TYPE_TUNNEL) ? path->out_label : 0u;
    entry->nexthop_id = path->nexthop_id;
    entry->out_ifindex = path->out_ifindex;
    entry->prefix_addr = head->key.addr;
    /* nexthop / relay 信息从 nexthop 对象读取（route_path 不再各存一份） */
    route_nhobj_info_t info;
    if (route_nhobj_lookup(path->nexthop_id, &info) == 0)
    {
        entry->iter_out_ifindex = info.relay_ifindex;
        entry->nexthop_addr = info.key.nexthop;
        entry->iter_nexthop_addr = info.relay_addr;
    }
    entry->source_addr = path->key.source;
}

// ============================================================================
// subscribe者匹配
// ============================================================================

/**
 * @brief 协议订阅过滤：STATIC 订阅者同时接收 BLACKHOLE（null0）路径。
 *
 * BLACKHOLE 路由由用户通过 `route ipv4 ... null0` 配置，与普通静态路由
 * 同属用户配置的静态路由范畴，因此被归入 STATIC 订阅/过滤口径。
 */
static int proto_filter_matches(uint32_t sub_proto, uint32_t path_proto)
{
    if (sub_proto == ROUTE_PROTOCOL_MAX)
    {
        return 1;
    }
    if (sub_proto == path_proto)
    {
        return 1;
    }
    if (sub_proto == ROUTE_PROTOCOL_STATIC && path_proto == ROUTE_PROTOCOL_BLACKHOLE)
    {
        return 1;
    }
    return 0;
}

static int subscriber_matches(const route_subscriber_t *sub, const route_head_t *head, const route_path_t *path)
{
    /* 协议过滤 */
    if (!proto_filter_matches(sub->protocol, path->key.protocol))
    {
        return 0;
    }
    /* VRF 过滤 */
    if (sub->vrf_id != ROUTE_VRF_ALL && sub->vrf_id != head->key.vrf_id)
    {
        return 0;
    }
    /* AFI 过滤 */
    if (sub->afi != ROUTE_AFI_ALL && sub->afi != head->key.afi)
    {
        return 0;
    }
    return 1;
}

// ============================================================================
// 公共 API
// ============================================================================

void route_pub_notify_module(uint32_t dst_module_id, const route_head_t *head, const route_path_t *path,
                             int is_withdraw)
{
    if (!head || !path || dst_module_id == 0)
    {
        return;
    }
    dev_ipc_context_t *ctx = route_local_ipc_ctx();

    route_msg_entry_t *payload = (route_msg_entry_t *)g_malloc(sizeof(route_msg_entry_t));
    if (!payload)
    {
        return;
    }
    build_entry(payload, head, path, is_withdraw);

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_UPDATE, DEV_MODULE_ID_ROUTE, dst_module_id, 0,
                                                    payload, sizeof(route_msg_entry_t), g_free);
    if (!msg)
    {
        g_free(payload);
        return;
    }

    if (dev_ipc_send(ctx, dst_module_id, msg) != 0)
    {
        LOG_WARN("Failed to send route update to module 0x%08X", dst_module_id);
    }
    dev_ipc_message_free(msg);
}

void route_pub_notify(GList *subscribers, const route_head_t *head, const route_path_t *path, int is_withdraw)
{
    if (!head || !path)
    {
        return;
    }

    for (GList *l = subscribers; l; l = l->next)
    {
        route_subscriber_t *sub = (route_subscriber_t *)l->data;
        if (!subscriber_matches(sub, head, path))
        {
            continue;
        }

        route_pub_notify_module(sub->module_id, head, path, is_withdraw);
    }
}

void route_pub_notify_entry(GList *subscribers, const route_msg_entry_t *entry)
{
    if (!entry)
    {
        return;
    }
    dev_ipc_context_t *ctx = route_local_ipc_ctx();

    for (GList *l = subscribers; l; l = l->next)
    {
        const route_subscriber_t *sub = (const route_subscriber_t *)l->data;

        /* 协议过滤 */
        if (!proto_filter_matches(sub->protocol, entry->protocol))
        {
            continue;
        }
        /* VRF 过滤 */
        if (sub->vrf_id != ROUTE_VRF_ALL && sub->vrf_id != entry->vrf_id)
        {
            continue;
        }
        /* AFI 过滤 */
        if (sub->afi != ROUTE_AFI_ALL && sub->afi != entry->afi)
        {
            continue;
        }

        route_msg_entry_t *payload = (route_msg_entry_t *)g_malloc(sizeof(route_msg_entry_t));
        if (!payload)
        {
            continue;
        }
        *payload = *entry;

        dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_UPDATE, DEV_MODULE_ID_ROUTE, sub->module_id, 0,
                                                        payload, sizeof(route_msg_entry_t), g_free);
        if (!msg)
        {
            g_free(payload);
            continue;
        }

        if (dev_ipc_send(ctx, sub->module_id, msg) != 0)
        {
            LOG_WARN("Failed to send route update to module 0x%08X", sub->module_id);
        }
        dev_ipc_message_free(msg);
    }
}

// ============================================================================
// shutdown 撤销:遍历 RIB 所有 OS-installed 路径 × 订阅者发 withdraw
// ============================================================================

typedef struct
{
    GList *subscribers;
    uint32_t withdrawn;
} pub_shutdown_ctx_t;

static void pub_shutdown_each_path_cb(const route_head_t *head, const route_path_t *path, void *userdata)
{
    pub_shutdown_ctx_t *c = (pub_shutdown_ctx_t *)userdata;
    if (!c || !head || !path)
    {
        return;
    }
    /* 只对当前 OS-installed 的最优路径发 withdraw,匹配订阅者实际见过的 add 事件。 */
    if ((path->flags & ROUTE_PATH_FLAG_OS_INSTALLED) == 0u)
    {
        return;
    }
    route_pub_notify(c->subscribers, head, path, /*is_withdraw=*/1);
    c->withdrawn++;
}

void route_pub_withdraw_all_for_shutdown(void)
{
    if (!g_route_work_local || !g_route_work_local->rib || !g_route_work_local->subscribers)
    {
        return;
    }
    pub_shutdown_ctx_t c = {
        .subscribers = g_route_work_local->subscribers,
        .withdrawn = 0,
    };
    route_rib_walk(g_route_work_local->rib, ROUTE_PROTOCOL_MAX, ROUTE_VRF_ALL, pub_shutdown_each_path_cb, &c);
    if (c.withdrawn > 0)
    {
        LOG_INFO("[route_pub] shutdown: notified subscribers about %u withdrawn best-path(s)", c.withdrawn);
    }
}
