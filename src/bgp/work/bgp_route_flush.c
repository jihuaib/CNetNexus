/**
 * @file   bgp_route_flush.c
 * @brief  BGP → ROUTE 模块路由下刷队列实现
 * @author jhb
 * @date   2026/04/17
 */
#include "bgp_route_flush.h"

#include <limits.h>
#include <string.h>

#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

// ============================================================================
// 内部辅助
// ============================================================================

static void bgp_route_flush_schedule(bgp_instance_t *inst);
static int bgp_route_flush_process_event(bgp_instance_t *inst, gboolean allow_reschedule);

/**
 * @brief 将 BGP 路由节点转换为 ROUTE 模块的 route_msg_entry_t
 *
 * 处理前缀与 nexthop 的 family 校验（允许 RFC 8950 双栈）以及 iterative nexthop
 * 解析结果填充。
 *
 * @return 1 成功填充 entry_out；0 无法转换（跳过）
 */
static int route_node_to_route_entry(uint32_t vrf_id, const bgp_nlri_entry_t *nlri, const bgp_route_node_t *route,
                                     route_msg_entry_t *entry_out)
{
    if (!nlri || !route || !entry_out)
    {
        return 0;
    }
    if (nlri->type != BGP_NLRI_PREFIX || (nlri->safi != BGP_SAFI_UNICAST && nlri->safi != BGP_SAFI_LABELED))
    {
        return 0;
    }
    if (nlri->safi == BGP_SAFI_LABELED && !nlri->prefix.has_label)
    {
        return 0;
    }

    const net_addr_t *prefix = &nlri->prefix.prefix.addr;
    if (prefix->family != AF_INET && prefix->family != AF_INET6)
    {
        return 0;
    }

    uint8_t max_len = (prefix->family == AF_INET) ? 32u : 128u;
    if (nlri->prefix.prefix.prefix_len > max_len)
    {
        return 0;
    }

    /* 允许跨族 nexthop/source（双栈场景：IPv4 前缀 + IPv6 nexthop/source, RFC 8950） */
    if (route->nexthop.global.family != AF_INET && route->nexthop.global.family != AF_INET6)
    {
        return 0;
    }

    net_addr_t iter_nh;
    memset(&iter_nh, 0, sizeof(iter_nh));
    uint32_t iter_oif = 0u;
    if (route->iter_watched && route->iter_resolved)
    {
        if (route->iter_relay_addr.family == AF_INET || route->iter_relay_addr.family == AF_INET6)
        {
            iter_nh = route->iter_relay_addr;
        }
        iter_oif = route->iter_out_ifindex;
    }

    int32_t metric = 0;
    if (BGP_ROUTE_ATTR(route)->has_med)
    {
        metric = (BGP_ROUTE_ATTR(route)->med > (uint32_t)INT32_MAX) ? INT32_MAX : (int32_t)BGP_ROUTE_ATTR(route)->med;
    }

    memset(entry_out, 0, sizeof(*entry_out));
    entry_out->vrf_id = vrf_id;
    entry_out->afi = (prefix->family == AF_INET) ? ROUTE_AFI_IPV4 : ROUTE_AFI_IPV6;
    entry_out->safi = ROUTE_SAFI_UNICAST;
    entry_out->prefix_len = nlri->prefix.prefix.prefix_len;
    entry_out->protocol = ROUTE_PROTOCOL_BGP;
    entry_out->metric = metric;
    entry_out->preference = ROUTE_ADMIN_DIST_BGP;
    entry_out->is_withdraw = 0u;
    entry_out->flags = 0u;
    if (route->iter_watched && route->iter_resolved && route->tunnel_id != 0u)
    {
        entry_out->nh_type = ROUTE_NH_TYPE_TUNNEL;
        entry_out->tunnel_id = route->tunnel_id;
    }
    else
    {
        entry_out->nh_type = ROUTE_NH_TYPE_IP;
        entry_out->tunnel_id = 0u;
    }
    entry_out->out_ifindex = 0u;
    entry_out->iter_out_ifindex = iter_oif;
    entry_out->prefix_addr = *prefix;
    entry_out->nexthop_addr = route->nexthop.global;
    entry_out->iter_nexthop_addr = iter_nh;
    entry_out->source_addr = route->source;
    return 1;
}

// ============================================================================
// 路由下刷队列
// ============================================================================

bgp_route_flush_queue_t *bgp_route_flush_queue_create(void)
{
    bgp_route_flush_queue_t *q = g_malloc0(sizeof(bgp_route_flush_queue_t));
    if (!q)
    {
        return NULL;
    }

    q->q = g_queue_new();
    if (!q->q)
    {
        g_free(q);
        return NULL;
    }

    return q;
}

void bgp_route_flush_queue_destroy(bgp_route_flush_queue_t *q, bgp_instance_t *inst)
{
    if (!q)
    {
        return;
    }

    bgp_rthead_t *head = NULL;
    while ((head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        if (inst)
        {
            bgp_rib_head_unref(head);
        }
    }

    if (q->q)
    {
        g_queue_free(q->q);
        q->q = NULL;
    }
    g_free(q);
}

int bgp_route_flush_queue_push(bgp_route_flush_queue_t *q, bgp_rthead_t *head)
{
    if (!q || !head)
    {
        return -1;
    }

    bgp_rib_head_ref(head);
    g_queue_push_tail(q->q, head);
    q->count++;
    if (head->inst)
    {
        bgp_route_flush_schedule(head->inst);
    }
    return 0;
}

int bgp_route_flush_queue_process(bgp_route_flush_queue_t *q, bgp_instance_t *inst, int batch_size)
{
    if (!q || !inst || batch_size <= 0)
    {
        return 0;
    }

    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return 0;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : ROUTE_VRF_DEFAULT;
    int processed = 0;
    bgp_rthead_t *head = NULL;

    while (processed < batch_size && (head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        q->count--;
        bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, &head->nlri);
        const bgp_route_node_t *best = rib ? bgp_rib_find_best(rib, &head->nlri) : NULL;
        /* import-route 仅用于 BGP 内部参考，不下刷到 ROUTE。 */
        const bgp_route_node_t *flush_best = (best && !BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT)) ? best : NULL;

        if (head)
        {
            char nlri_str[BGP_NLRI_KEY_MAX];
            bgp_nlri_to_str(&head->nlri, nlri_str, sizeof(nlri_str));

            /* 先撤销已下刷但不再是 best+valid 的路径 */
            for (GList *l = head->route_list; l; l = l->next)
            {
                bgp_route_node_t *route = (bgp_route_node_t *)l->data;
                if (!route)
                {
                    continue;
                }

                if (!BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED) || route == flush_best)
                {
                    continue;
                }

                route_msg_entry_t withdraw_entry;
                if (!route_node_to_route_entry(vrf_id, &head->nlri, route, &withdraw_entry))
                {
                    continue;
                }

                if (route_rpc_del(ctx, &withdraw_entry) == ERRCODE_SUCCESS)
                {
                    BIT_CLR(route->flags, BGP_ROUTE_FLAG_FLUSHED);
                }
                else
                {
                    LOG_WARN("BGP: route flush withdraw failed nlri=%s", nlri_str);
                }
            }

            /* 仅下刷当前 best+valid 路由 */
            if (flush_best)
            {
                bgp_route_node_t *best_mut = (bgp_route_node_t *)flush_best;
                if (!BIT_TEST(best_mut->flags, BGP_ROUTE_FLAG_FLUSHED))
                {
                    route_msg_entry_t add_entry;
                    if (route_node_to_route_entry(vrf_id, &head->nlri, flush_best, &add_entry) &&
                        route_rpc_add(ctx, &add_entry) == ERRCODE_SUCCESS)
                    {
                        BIT_SET(best_mut->flags, BGP_ROUTE_FLAG_FLUSHED);
                    }
                    else
                    {
                        LOG_WARN("BGP: route flush add failed nlri=%s", nlri_str);
                    }
                }
            }

            /* 删除时机统一放在 unref 之后，避免 cleanup 提前回收。 */
        }

        bgp_rib_head_unref(head);
        if (rib)
        {
            (void)bgp_rib_gc_head(rib, head);
        }
        processed++;
    }

    if (processed > 0)
    {
        LOG_DEBUG("BGP: route_flush_queue afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", (unsigned)inst->afi,
                  (unsigned)inst->safi, processed, q->count);
    }

    return processed;
}

static int bgp_route_flush_process_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst || !inst->route_flush_queue)
    {
        return 0;
    }

    int processed = bgp_route_flush_queue_process(inst->route_flush_queue, inst, BGP_WORK_BATCH_SIZE);
    if (allow_reschedule && processed > 0 && inst->route_flush_queue->count > 0u)
    {
        bgp_route_flush_schedule(inst);
    }
    return processed;
}

static void bgp_route_flush_schedule(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
    if (bgp_worker_post_route_flush_event(vrf_id, inst->afi, inst->safi) == 0)
    {
        return;
    }

    if (bgp_worker_is_current_thread())
    {
        (void)bgp_route_flush_process_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue route-flush work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

void bgp_route_flush_handle_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_worker_lookup_instance(vrf_id, afi, safi);
    (void)bgp_route_flush_process_event(inst, TRUE);
}

int bgp_route_flush_process_pending(bgp_instance_t *inst)
{
    return bgp_route_flush_process_event(inst, FALSE);
}
