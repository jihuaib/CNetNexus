/**
 * @file   bgp_route_flush.c
 * @brief  BGP → ROUTE 模块路由下刷队列实现
 * @author jhb
 * @date   2026/04/17
 */
#include "bgp_route_flush.h"

#include <limits.h>
#include <string.h>

#include "bgp_import_rib.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_nexthop.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_relay.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "srv6.h"

// ============================================================================
// 内部辅助
// ============================================================================

static void bgp_route_flush_schedule(bgp_instance_t *inst);
static int bgp_route_flush_process_event(bgp_instance_t *inst, gboolean allow_reschedule);

static int bgp_route_flush_send_withdraw(dev_ipc_context_t *ctx, const route_msg_entry_t *entry, gboolean wait_for_ack)
{
    return wait_for_ack ? route_rpc_del_wait(ctx, entry, 0u) : route_rpc_del(ctx, entry);
}

static gboolean route_is_srv6_be(const bgp_route_node_t *route)
{
    return route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_REMOTE_CROSS) &&
           BIT_TEST(route->flags, BGP_ROUTE_FLAG_SRV6_BE);
}

static gboolean route_srv6_service_usable(const bgp_nlri_entry_t *nlri, const bgp_route_node_t *route)
{
    if (!nlri || !route_is_srv6_be(route) || !route->attr)
    {
        return FALSE;
    }
    const bgp_attr_t *attr = BGP_ROUTE_ATTR(route);
    uint16_t expected_behavior =
        (nlri->afi == BGP_AFI_IPV4) ? (uint16_t)SRV6_BEHAVIOR_END_DT4 : (uint16_t)SRV6_BEHAVIOR_END_DT6;
    return attr->has_srv6_l3_service && (nlri->afi == BGP_AFI_IPV4 || nlri->afi == BGP_AFI_IPV6) &&
           attr->srv6_behavior == expected_behavior && attr->srv6_sid.family == AF_INET6 &&
           !net_addr_is_zero(&attr->srv6_sid) &&
           (!attr->has_srv6_sid_structure ||
            (attr->transposition_len == 0u && attr->transposition_offset == 0u && attr->argument_len == 0u));
}

/**
 * @brief 将 BGP 路由节点转换为 ROUTE 模块的 route_msg_entry_t
 *
 * 处理前缀与 nexthop 的 family 校验（允许 RFC 8950 双栈）以及 iterative nexthop
 * 解析结果填充。
 *
 * @return 1 成功填充 entry_out；0 无法转换（跳过）
 */
/**
 * @brief 把一个 BGP 路径节点转成下刷 ROUTE 的条目
 *
 * @param for_withdraw TRUE=构造撤销条目。撤销只需 (vrf/afi/前缀/协议/来源) 即可让 ROUTE
 *        按键删除，不依赖下一跳是否还能解析——这点很关键：路由被撤销时其隧道 watch/源
 *        节点可能已拆除，下一跳不再可解析，但撤销仍必须送达 ROUTE。FALSE=构造新增条目，
 *        要求下一跳可解析（IP 对象或隧道），否则返回 0 不下刷。
 * @return 1=成功填充 entry_out；0=不应下刷该条目
 */
static int route_node_to_route_entry(uint32_t vrf_id, const bgp_nlri_entry_t *nlri, const bgp_route_node_t *route,
                                     gboolean for_withdraw, route_msg_entry_t *entry_out)
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

    /* 撤销：只填键字段即可，跳过下一跳解析（此刻可能已不可解析）。 */
    if (for_withdraw)
    {
        memset(entry_out, 0, sizeof(*entry_out));
        entry_out->vrf_id = vrf_id;
        entry_out->afi = (prefix->family == AF_INET) ? ROUTE_AFI_IPV4 : ROUTE_AFI_IPV6;
        entry_out->safi = ROUTE_SAFI_UNICAST;
        entry_out->prefix_len = nlri->prefix.prefix.prefix_len;
        entry_out->protocol = ROUTE_PROTOCOL_BGP;
        entry_out->is_withdraw = 1u;
        entry_out->prefix_addr = *prefix;
        entry_out->source_addr = route->source;
        return 1;
    }

    const gboolean srv6_be = route_is_srv6_be(route);
    const gboolean use_srv6 = route_srv6_service_usable(nlri, route);
    /* 已选定的 SRv6 BE incarnation 不符合整 SID/behavior 约束时
     * fail closed，不得因 attr 变化自动回退到 MPLS tunnel。 */
    if (srv6_be && !use_srv6)
    {
        return 0;
    }
    /* REMOTE_CROSS 的 MPLS incarnation 必须携带真实非保留 VPN service label。
     * label 3 是 whole-SID SRv6 VPN NLRI 的 Implicit-NULL 占位，不能用于 VRF demux。 */
    if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_REMOTE_CROSS) && !srv6_be && (!route->has_label || route->label < 16u))
    {
        return 0;
    }

    net_addr_t nexthop_addr;
    memset(&nexthop_addr, 0, sizeof(nexthop_addr));
    if (use_srv6)
    {
        /* 合成路由的自有 nexthop 对象以 service SID 为 key；通用
         * bgp_nexthop_get_route_addr() 会回溯 src_route，因此这里直接使用 attr SID。 */
        nexthop_addr = BGP_ROUTE_ATTR(route)->srv6_sid;
    }
    else if (bgp_nexthop_get_route_addr(route, &nexthop_addr) != ERRCODE_SUCCESS ||
             (nexthop_addr.family != AF_INET && nexthop_addr.family != AF_INET6))
    {
        return 0;
    }

    bgp_nexthop_value_t nh_value;
    memset(&nh_value, 0, sizeof(nh_value));
    (void)bgp_relay_get_route_iter_value(route, &nh_value);
    route_nhobj_key_t nh_key;
    memset(&nh_key, 0, sizeof(nh_key));
    if (!use_srv6)
    {
        (void)bgp_nexthop_get_route_key(route, &nh_key);
    }
    gboolean use_tunnel = (nh_value.iter_watched && nh_value.iter_resolved && nh_value.tunnel_id != 0u);
    gboolean local_cross_local_delivery =
        BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) && BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_DELIVERY);

    if (use_srv6 && (route->nexthop_id == 0u || !nh_value.iter_watched || !nh_value.iter_resolved ||
                     nh_value.iter_out_ifindex == 0u ||
                     (nh_value.iter_relay_addr.family != AF_INET && nh_value.iter_relay_addr.family != AF_INET6)))
    {
        return 0;
    }
    if (!use_srv6 && !use_tunnel && (route->nexthop_id == 0u || nlri->safi == BGP_SAFI_LABELED))
    {
        return 0;
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
    if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_DELIVERY) && !local_cross_local_delivery)
    {
        entry_out->flags |= ROUTE_ENTRY_FLAG_LOCAL;
    }
    if (use_srv6)
    {
        entry_out->nh_type = ROUTE_NH_TYPE_SRV6;
        entry_out->srv6_sid = BGP_ROUTE_ATTR(route)->srv6_sid;
        entry_out->tunnel_id = 0u;
        entry_out->out_label = 0u;
        entry_out->iter_nexthop_addr = nh_value.iter_relay_addr;
        entry_out->iter_out_ifindex = nh_value.iter_out_ifindex;
    }
    else if (nh_key.nh_type == ROUTE_NH_TYPE_BLACKHOLE)
    {
        entry_out->nh_type = ROUTE_NH_TYPE_BLACKHOLE;
        entry_out->tunnel_id = 0u;
    }
    else if (use_tunnel)
    {
        entry_out->nh_type = ROUTE_NH_TYPE_TUNNEL;
        entry_out->tunnel_id = nh_value.tunnel_id;
        /* L3VPN 私网标签：导入的 vpnv4 路由（REMOTE_CROSS）携带对端 PE 通告的 VPN 标签，
         * 转发时必须压入此标签，对端 PE 才能据此 demux 到正确 VRF。隧道（BGP 邻接假隧道）
         * 仅提供到 PE 的传输路径，VPN 标签是每路由/每 VRF 的服务标签，随路由下发。 */
        entry_out->out_label = route->has_label ? route->label : 0u;
        /* 隧道路由不申请 ROUTE nexthop 对象（nexthop_id=0），ROUTE 不会自行迭代隧道，
         * 故把已解析的迭代结果（relay 端点 + 出接口）随条目带过去，由 ROUTE 据此 set_relay，
         * 使 show route 的 Iter NH / Iter OIF 正确显示。 */
        entry_out->iter_nexthop_addr = nh_value.iter_relay_addr;
        entry_out->iter_out_ifindex = nh_value.iter_out_ifindex;
    }
    else
    {
        entry_out->nh_type = ROUTE_NH_TYPE_IP;
        entry_out->tunnel_id = 0u;
    }
    entry_out->out_ifindex = 0u;
    entry_out->prefix_addr = *prefix;
    /* 原始 BGP 下一跳地址：ROUTE 用它作 nexthop 对象身份键并显示为 Nexthop；
     * 隧道分支下还兜底用作 Iter NH（当 iter_relay_addr 为空时）。 */
    entry_out->nexthop_addr = nexthop_addr;
    entry_out->nexthop_id = use_tunnel ? 0u : route->nexthop_id;
    /*
     * LOCAL_CROSS 泄漏本地地址时不能下成 RTN_LOCAL，否则回包从源 VRF 的出接口进入后会被
     * 交付到源 VRF，而不是目标 VRF socket。内核可通模型是：
     *   <local-prefix> dev <target-vrf-master> scope link
     * 所以清 gateway/nexthop_id，仅保留 nexthop-vrf 解析出的目标 VRF master ifindex。
     */
    if (local_cross_local_delivery && nh_value.iter_watched && nh_value.iter_resolved &&
        nh_value.iter_out_ifindex != 0u)
    {
        memset(&entry_out->nexthop_addr, 0, sizeof(entry_out->nexthop_addr));
        memset(&entry_out->iter_nexthop_addr, 0, sizeof(entry_out->iter_nexthop_addr));
        entry_out->out_ifindex = nh_value.iter_out_ifindex;
        entry_out->iter_out_ifindex = nh_value.iter_out_ifindex;
        entry_out->nexthop_id = 0u;
    }
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

static int bgp_route_flush_queue_process_internal(bgp_route_flush_queue_t *q, bgp_instance_t *inst, int batch_size,
                                                  gboolean wait_withdraw_ack)
{
    if (!q || !inst || batch_size <= 0)
    {
        return 0;
    }
    if (q->defer_until_ready)
    {
        return 0;
    }

    /* labeled instance（或其他设了 no_route_flush 的 inst）不下刷 ROUTE，
     * 路由停留在 BGP RIB 内由 unicast 通过 import-rib 接管下刷。 */
    if (bgp_import_rib_should_skip_flush(inst))
    {
        bgp_rthead_t *skip_head = NULL;
        int drained = 0;
        while ((skip_head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
        {
            q->count--;
            bgp_rib_head_unref(skip_head);
            drained++;
        }
        return drained;
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
        gboolean rpc_failed = FALSE;
        bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, &head->nlri);
        const bgp_route_node_t *best = rib ? bgp_rib_find_best(rib, &head->nlri) : NULL;
        /* import-route 仅用于 BGP 内部参考，不下刷到 ROUTE。 */
        /* 不下刷到 ROUTE：仅重分发(IMPORT，本就源自 ROUTE)。REMOTE_CROSS(vrf-import) 走隧道转发，
         * 需下刷进 VRF FIB。 */
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
                if (!route_node_to_route_entry(vrf_id, &head->nlri, route, TRUE, &withdraw_entry))
                {
                    continue;
                }

                if (bgp_route_flush_send_withdraw(ctx, &withdraw_entry, wait_withdraw_ack) != ERRCODE_SUCCESS)
                {
                    LOG_WARN("BGP: route flush withdraw failed nlri=%s", nlri_str);
                    rpc_failed = TRUE;
                    break;
                }
                BIT_CLR(route->flags, BGP_ROUTE_FLAG_FLUSHED);
                BIT_CLR(route->flags, BGP_ROUTE_FLAG_FIB_DIRTY);
            }

            /* 仅下刷当前 best+valid 路由 */
            if (!rpc_failed && flush_best)
            {
                bgp_route_node_t *best_mut = (bgp_route_node_t *)flush_best;
                if (!BIT_TEST(best_mut->flags, BGP_ROUTE_FLAG_FLUSHED) ||
                    BIT_TEST(best_mut->flags, BGP_ROUTE_FLAG_FIB_DIRTY))
                {
                    route_msg_entry_t add_entry;
                    gboolean entry_built =
                        route_node_to_route_entry(vrf_id, &head->nlri, flush_best, FALSE, &add_entry) ? TRUE : FALSE;
                    if (entry_built && route_rpc_add(ctx, &add_entry) == ERRCODE_SUCCESS)
                    {
                        BIT_SET(best_mut->flags, BGP_ROUTE_FLAG_FLUSHED);
                        BIT_CLR(best_mut->flags, BGP_ROUTE_FLAG_FIB_DIRTY);
                    }
                    else
                    {
                        LOG_WARN("BGP: route flush add failed nlri=%s", nlri_str);
                        if (entry_built)
                        {
                            rpc_failed = TRUE;
                        }
                        else if (BIT_TEST(best_mut->flags, BGP_ROUTE_FLAG_FLUSHED))
                        {
                            /* 新 desired state 已无法安全序列化，不能继续把旧
                             * incarnation 当成当前 best 留在 FIB；按稳定 identity
                             * 撤销旧项，DIRTY 保留，等后续状态变化重新 calc。 */
                            route_msg_entry_t withdraw_entry;
                            if (!route_node_to_route_entry(vrf_id, &head->nlri, best_mut, TRUE, &withdraw_entry) ||
                                bgp_route_flush_send_withdraw(ctx, &withdraw_entry, wait_withdraw_ack) !=
                                    ERRCODE_SUCCESS)
                            {
                                rpc_failed = TRUE;
                            }
                            else
                            {
                                BIT_CLR(best_mut->flags, BGP_ROUTE_FLAG_FLUSHED);
                            }
                        }
                    }
                }
            }

            /* 删除时机统一放在 unref 之后，避免 cleanup 提前回收。 */
        }

        if (rpc_failed)
        {
            /* 保留出队时持有的 head 引用及 FLUSHED 状态，等 ROUTE
             * READY 解锁重放；故障期间不立即重排，避免 worker 自旋。 */
            g_queue_push_head(q->q, head);
            q->count++;
            q->defer_until_ready = dev_ipc_is_connected(ctx, DEV_MODULE_ID_ROUTE) ? FALSE : TRUE;
            if (!q->defer_until_ready && !wait_withdraw_ack)
            {
                /* 连接仍在时视为瞬时投递故障，允许事件循环重试；
                 * 连接已断则等 READY，不在 worker 中忙循环。同步配置
                 * drain 中的 NACK/超时不是有效进展，必须返回 0 让调用方停止。 */
                processed++;
            }
            break;
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

int bgp_route_flush_queue_process(bgp_route_flush_queue_t *q, bgp_instance_t *inst, int batch_size)
{
    return bgp_route_flush_queue_process_internal(q, inst, batch_size, FALSE);
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
    if (!inst || !inst->route_flush_queue)
    {
        return 0;
    }

    /* 配置删除在同一 worker 线程内抽干队列：撤销必须等 ROUTE ACK，
     * 否则不能清 FLUSHED 并继续销毁实例。普通工作事件仍使用异步快速路径。 */
    return bgp_route_flush_queue_process_internal(inst->route_flush_queue, inst, BGP_WORK_BATCH_SIZE, TRUE);
}

static void bgp_route_flush_replay_best_cb(const bgp_rthead_t *head, const bgp_route_node_t *route, gpointer user_data)
{
    uint32_t *queued = (uint32_t *)user_data;
    if (!head || !route || !BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID) ||
        BIT_TEST(route->flags, BGP_ROUTE_FLAG_IMPORT) || !head->inst || !head->inst->route_flush_queue)
    {
        return;
    }

    /* ROUTE 重启后旧 RIB 为空；旧 FLUSHED 和故障窗口内从未成功
     * 下刷的 best 都必须重放。 */
    BIT_CLR(((bgp_route_node_t *)route)->flags, BGP_ROUTE_FLAG_FLUSHED);
    BIT_SET(((bgp_route_node_t *)route)->flags, BGP_ROUTE_FLAG_FIB_DIRTY);
    if (bgp_route_flush_queue_push(head->inst->route_flush_queue, (bgp_rthead_t *)head) == 0 && queued)
    {
        (*queued)++;
    }
}

static void bgp_route_flush_replay_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib,
                                          gpointer user_data)
{
    (void)inst;
    (void)entry;
    if (!rib)
    {
        return;
    }
    bgp_rib_foreach_best(rib, bgp_route_flush_replay_best_cb, user_data);
}

uint32_t bgp_route_flush_replay_flushed_all(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol || !g_bgp_work_local->protocol->vrf_hash)
    {
        return 0;
    }

    uint32_t queued = 0;
    GHashTableIter vrf_iter;
    gpointer vrf_key = NULL;
    gpointer vrf_val = NULL;
    g_hash_table_iter_init(&vrf_iter, g_bgp_work_local->protocol->vrf_hash);
    while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
    {
        (void)vrf_key;
        bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
        if (!vrf || !vrf->inst_hash)
        {
            continue;
        }

        GHashTableIter inst_iter;
        gpointer inst_key = NULL;
        gpointer inst_val = NULL;
        g_hash_table_iter_init(&inst_iter, vrf->inst_hash);
        while (g_hash_table_iter_next(&inst_iter, &inst_key, &inst_val))
        {
            (void)inst_key;
            bgp_instance_t *inst = (bgp_instance_t *)inst_val;
            if (!inst || bgp_import_rib_should_skip_flush(inst))
            {
                continue;
            }
            if (inst->route_flush_queue)
            {
                inst->route_flush_queue->defer_until_ready = FALSE;
                if (inst->route_flush_queue->count > 0u)
                {
                    bgp_route_flush_schedule(inst);
                }
            }
            bgp_inst_foreach_rib(inst, bgp_route_flush_replay_rib_cb, &queued);
        }
    }

    if (queued > 0)
    {
        LOG_INFO("BGP: replay queued %u best route(s) to ROUTE", queued);
    }
    return queued;
}

// ============================================================================
// 配置销毁前的同步撤销屏障
// ============================================================================

typedef struct withdraw_instance_sync_ctx
{
    dev_ipc_context_t *ctx;
    uint32_t vrf_id;
    uint32_t withdrawn;
    gboolean failed;
} withdraw_instance_sync_ctx_t;

static gboolean withdraw_instance_sync_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    withdraw_instance_sync_ctx_t *wctx = (withdraw_instance_sync_ctx_t *)user_data;
    if (!head || !wctx || wctx->failed)
    {
        return wctx && wctx->failed;
    }

    for (GList *l = head->route_list; l; l = l->next)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)l->data;
        if (!route || !BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED))
        {
            continue;
        }

        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&head->nlri, nlri_str, sizeof(nlri_str));
        route_msg_entry_t withdraw_entry;
        if (!route_node_to_route_entry(wctx->vrf_id, &head->nlri, route, TRUE, &withdraw_entry))
        {
            LOG_ERROR("BGP delete barrier: cannot build ROUTE withdraw nlri=%s vrf=%u", nlri_str, wctx->vrf_id);
            wctx->failed = TRUE;
            return TRUE;
        }
        if (!wctx->ctx || route_rpc_del_wait(wctx->ctx, &withdraw_entry, 0u) != ERRCODE_SUCCESS)
        {
            LOG_WARN("BGP delete barrier: ROUTE withdraw not acknowledged nlri=%s vrf=%u", nlri_str, wctx->vrf_id);
            wctx->failed = TRUE;
            return TRUE;
        }

        BIT_CLR(route->flags, BGP_ROUTE_FLAG_FLUSHED);
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_FIB_DIRTY);
        wctx->withdrawn++;
    }
    return FALSE;
}

static void withdraw_instance_sync_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib,
                                          gpointer user_data)
{
    (void)inst;
    (void)entry;
    withdraw_instance_sync_ctx_t *wctx = (withdraw_instance_sync_ctx_t *)user_data;
    if (!rib || !rib->head_tree || !wctx || wctx->failed)
    {
        return;
    }
    g_tree_foreach(rib->head_tree, withdraw_instance_sync_head_cb, wctx);
}

int bgp_route_flush_withdraw_instance_sync(bgp_instance_t *inst)
{
    if (!inst)
    {
        return ERRCODE_SUCCESS;
    }
    if (bgp_import_rib_should_skip_flush(inst))
    {
        return ERRCODE_SUCCESS;
    }

    withdraw_instance_sync_ctx_t wctx = {
        .ctx = bgp_local_ipc_ctx(),
        .vrf_id = inst->vrf ? inst->vrf->vrf_id : ROUTE_VRF_DEFAULT,
        .withdrawn = 0u,
        .failed = FALSE,
    };
    bgp_inst_foreach_rib(inst, withdraw_instance_sync_rib_cb, &wctx);
    if (wctx.withdrawn > 0u)
    {
        LOG_INFO("BGP delete barrier: acknowledged %u ROUTE withdraw(s) vrf=%u afi=%u safi=%u%s", wctx.withdrawn,
                 wctx.vrf_id, (unsigned)inst->afi, (unsigned)inst->safi, wctx.failed ? " before failure" : "");
    }
    return wctx.failed ? ERRCODE_FAIL : ERRCODE_SUCCESS;
}

// ============================================================================
// shutdown 撤销
// ============================================================================

typedef struct withdraw_all_ctx
{
    dev_ipc_context_t *ctx;
    uint32_t vrf_id;
    uint32_t withdrawn;
} withdraw_all_ctx_t;

static gboolean shutdown_withdraw_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    withdraw_all_ctx_t *fctx = (withdraw_all_ctx_t *)user_data;
    if (!head || !fctx || !fctx->ctx)
    {
        return FALSE;
    }
    for (GList *l = head->route_list; l; l = l->next)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)l->data;
        if (!route || !BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED))
        {
            continue;
        }
        route_msg_entry_t withdraw_entry;
        if (!route_node_to_route_entry(fctx->vrf_id, &head->nlri, route, TRUE, &withdraw_entry))
        {
            continue;
        }
        if (route_rpc_del(fctx->ctx, &withdraw_entry) == ERRCODE_SUCCESS)
        {
            BIT_CLR(route->flags, BGP_ROUTE_FLAG_FLUSHED);
            BIT_CLR(route->flags, BGP_ROUTE_FLAG_FIB_DIRTY);
            fctx->withdrawn++;
        }
    }
    return FALSE; /* 继续遍历 GTree */
}

static void shutdown_withdraw_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer user_data)
{
    (void)inst;
    (void)entry;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    g_tree_foreach(rib->head_tree, shutdown_withdraw_head_cb, user_data);
}

uint32_t bgp_route_flush_withdraw_all_for_shutdown(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol || !g_bgp_work_local->protocol->vrf_hash)
    {
        return 0;
    }
    dev_ipc_context_t *ipc_ctx = bgp_local_ipc_ctx();
    if (!ipc_ctx)
    {
        LOG_WARN("BGP shutdown withdraw: IPC ctx already gone, skip");
        return 0;
    }

    uint32_t total = 0;
    GHashTableIter vrf_iter;
    gpointer vrf_key = NULL;
    gpointer vrf_val = NULL;
    g_hash_table_iter_init(&vrf_iter, g_bgp_work_local->protocol->vrf_hash);
    while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
    {
        (void)vrf_key;
        bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
        if (!vrf || !vrf->inst_hash)
        {
            continue;
        }
        GHashTableIter inst_iter;
        gpointer inst_key = NULL;
        gpointer inst_val = NULL;
        g_hash_table_iter_init(&inst_iter, vrf->inst_hash);
        while (g_hash_table_iter_next(&inst_iter, &inst_key, &inst_val))
        {
            (void)inst_key;
            bgp_instance_t *inst = (bgp_instance_t *)inst_val;
            if (!inst || bgp_import_rib_should_skip_flush(inst))
            {
                continue;
            }
            withdraw_all_ctx_t fctx = {
                .ctx = ipc_ctx,
                .vrf_id = (inst->vrf) ? inst->vrf->vrf_id : ROUTE_VRF_DEFAULT,
                .withdrawn = 0,
            };
            bgp_inst_foreach_rib(inst, shutdown_withdraw_rib_cb, &fctx);
            if (fctx.withdrawn > 0)
            {
                LOG_INFO("BGP shutdown: withdrew %u route(s) to ROUTE (vrf=%u afi=%u safi=%u)", fctx.withdrawn,
                         fctx.vrf_id, (unsigned)inst->afi, (unsigned)inst->safi);
                total += fctx.withdrawn;
            }
        }
    }
    return total;
}
