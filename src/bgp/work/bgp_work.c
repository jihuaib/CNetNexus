/**
 * @file   bgp_work.c
 * @brief  BGP 路由处理工作队列实现
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_work.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bgp_adj_rib_out.h"
#include "bgp_attr_intern.h"
#include "bgp_calc.h"
#include "bgp_fsm.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_rib.h"
#include "bgp_update_group.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

// ============================================================================
// 工作事件内部辅助
// ============================================================================

static int bgp_work_process_calc_event(bgp_instance_t *inst, gboolean allow_reschedule);
static int bgp_work_process_route_flush_event(bgp_instance_t *inst, gboolean allow_reschedule);
static int bgp_work_process_session_pub_event(bgp_instance_t *inst, gboolean allow_reschedule);
static void bgp_work_schedule_calc(bgp_instance_t *inst);
static void bgp_work_schedule_route_flush(bgp_instance_t *inst);
static void bgp_work_schedule_session_pub(bgp_instance_t *inst);

static gboolean bgp_session_is_publish_ready(const bgp_session_t *sess)
{
    return sess && sess->pri_conn && sess->pri_conn->fd >= 0 && sess->fsm_state == BGP_FSM_STATE_ESTABLISHED;
}

static uint32_t bgp_work_local_as_number(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        return 0u;
    }
    return g_bgp_work_local->protocol->as_number;
}

static gboolean bgp_as_path_contains_as(const char *as_path, uint32_t asn)
{
    if (!as_path || as_path[0] == '\0' || asn == 0u)
    {
        return FALSE;
    }

    const char *p = as_path;
    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t' || *p == '{' || *p == '}' || *p == ',')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }

        char *end = NULL;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p)
        {
            p++;
            continue;
        }
        if ((uint32_t)v == asn)
        {
            return TRUE;
        }
        p = end;
    }

    return FALSE;
}

static const bgp_session_t *bgp_best_source_session(const bgp_route_node_t *best)
{
    if (!best || BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT) || !best->head || !best->head->inst ||
        !best->head->inst->vrf)
    {
        return NULL;
    }
    return bgp_vrf_find_session(best->head->inst->vrf, &best->source);
}

static gboolean bgp_work_on_worker_thread(void)
{
    return g_bgp_work_local && g_bgp_work_local->worker_thread != 0 &&
           pthread_equal(pthread_self(), g_bgp_work_local->worker_thread);
}

static int route_node_to_route_entry(uint32_t vrf_id, const bgp_nlri_entry_t *nlri, const bgp_route_node_t *route,
                                     route_msg_entry_t *entry_out)
{
    if (!nlri || !route || !entry_out)
    {
        return 0;
    }
    if (nlri->type != BGP_NLRI_PREFIX || nlri->safi != BGP_SAFI_UNICAST)
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
    entry_out->out_ifindex = 0u;
    entry_out->iter_out_ifindex = iter_oif;
    entry_out->prefix_addr = *prefix;
    entry_out->nexthop_addr = route->nexthop.global;
    entry_out->iter_nexthop_addr = iter_nh;
    entry_out->source_addr = route->source;
    return 1;
}

// ============================================================================
// 优选队列
// ============================================================================

bgp_calc_queue_t *bgp_calc_queue_create(void)
{
    bgp_calc_queue_t *q = g_malloc0(sizeof(bgp_calc_queue_t));
    q->q = g_queue_new();
    return q;
}

void bgp_calc_queue_destroy(bgp_calc_queue_t *q, bgp_instance_t *inst)
{
    if (!q)
    {
        return;
    }
    bgp_rthead_t *head = NULL;
    while ((head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        if (inst && inst->rib)
        {
            bgp_rib_head_unref(head);
        }
    }
    g_queue_free(q->q);
    g_free(q);
}

int bgp_calc_queue_push(bgp_calc_queue_t *q, bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!q || !inst || !inst->rib || !nlri)
    {
        return -1;
    }

    bgp_rthead_t *head = bgp_rib_ensure_head(inst->rib, nlri);
    if (!head)
    {
        return -1;
    }

    bgp_rib_head_ref(head);
    g_queue_push_tail(q->q, head);
    q->count++;
    bgp_work_schedule_calc(inst);
    return 0;
}

int bgp_calc_queue_process(bgp_calc_queue_t *q, bgp_instance_t *inst, int batch_size)
{
    if (!q || !inst || batch_size <= 0)
    {
        return 0;
    }
    int processed = 0;
    bgp_rthead_t *head = NULL;
    while (processed < batch_size && (head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        q->count--;
        bgp_calc_run_one(inst, &head->nlri);
        bgp_rib_head_unref(head);
        processed++;
    }
    if (processed > 0)
    {
        LOG_DEBUG("BGP: calc_queue afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", (unsigned)inst->afi,
                  (unsigned)inst->safi, processed, q->count);
    }
    return processed;
}

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
        if (inst && inst->rib)
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
        bgp_work_schedule_route_flush(head->inst);
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
        const bgp_route_node_t *best = bgp_rib_find_best(inst->rib, &head->nlri);
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
        (void)bgp_rib_gc_head(inst->rib, head);
        processed++;
    }

    if (processed > 0)
    {
        LOG_DEBUG("BGP: route_flush_queue afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", (unsigned)inst->afi,
                  (unsigned)inst->safi, processed, q->count);
    }

    return processed;
}

static bgp_instance_t *bgp_work_lookup_instance(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        return NULL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, vrf_id);
    if (!vrf || !vrf->inst_hash)
    {
        return NULL;
    }

    return (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));
}

static int bgp_work_process_calc_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst || !inst->calc_queue)
    {
        return 0;
    }

    int processed = bgp_calc_queue_process(inst->calc_queue, inst, BGP_WORK_BATCH_SIZE);
    if (allow_reschedule && processed > 0 && inst->calc_queue->count > 0u)
    {
        bgp_work_schedule_calc(inst);
    }
    return processed;
}

static int bgp_work_process_route_flush_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst || !inst->route_flush_queue)
    {
        return 0;
    }

    int processed = bgp_route_flush_queue_process(inst->route_flush_queue, inst, BGP_WORK_BATCH_SIZE);
    if (allow_reschedule && processed > 0 && inst->route_flush_queue->count > 0u)
    {
        bgp_work_schedule_route_flush(inst);
    }
    return processed;
}

static int bgp_work_process_session_pub_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst)
    {
        return 0;
    }

    int total_processed = 0;
    gboolean need_more = FALSE;

    for (GList *ul = inst->update_groups; ul; ul = ul->next)
    {
        bgp_update_group_t *ug = (bgp_update_group_t *)ul->data;
        if (!ug)
        {
            continue;
        }
        for (GList *sl = ug->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (!sg)
            {
                continue;
            }
            int processed = bgp_subgroup_process_queues(sg, inst, BGP_WORK_BATCH_SIZE);
            if (processed > 0)
            {
                total_processed += processed;
            }
            if ((sg->announce_queue && !g_queue_is_empty(sg->announce_queue)) ||
                (sg->withdraw_queue && !g_queue_is_empty(sg->withdraw_queue)))
            {
                need_more = TRUE;
            }
        }
    }

    if (allow_reschedule && total_processed > 0 && need_more)
    {
        bgp_work_schedule_session_pub(inst);
    }

    return total_processed;
}

static void bgp_work_schedule_calc(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
    if (bgp_worker_post_calc_event(vrf_id, inst->afi, inst->safi) == 0)
    {
        return;
    }

    if (bgp_work_on_worker_thread())
    {
        (void)bgp_work_process_calc_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue calc work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

static void bgp_work_schedule_route_flush(bgp_instance_t *inst)
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

    if (bgp_work_on_worker_thread())
    {
        (void)bgp_work_process_route_flush_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue route-flush work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

static void bgp_work_schedule_session_pub(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
    if (bgp_worker_post_session_pub_event(vrf_id, inst->afi, inst->safi) == 0)
    {
        return;
    }

    if (bgp_work_on_worker_thread())
    {
        (void)bgp_work_process_session_pub_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue session-pub work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

void bgp_work_handle_calc_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_work_lookup_instance(vrf_id, afi, safi);
    (void)bgp_work_process_calc_event(inst, TRUE);
}

void bgp_work_handle_route_flush_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_work_lookup_instance(vrf_id, afi, safi);
    (void)bgp_work_process_route_flush_event(inst, TRUE);
}

void bgp_work_handle_session_pub_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_work_lookup_instance(vrf_id, afi, safi);
    (void)bgp_work_process_session_pub_event(inst, TRUE);
}

void bgp_work_process_pending(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    for (;;)
    {
        int processed = 0;
        processed += bgp_work_process_calc_event(inst, FALSE);
        processed += bgp_work_process_route_flush_event(inst, FALSE);
        processed += bgp_work_process_session_pub_event(inst, FALSE);
        if (processed <= 0)
        {
            break;
        }
    }
}

// ============================================================================
// Subgroup 级发布通路（Phase 2）
// ============================================================================

/**
 * @brief 按子组 rule + effective_local_addr 计算 out_nh
 *
 * R_LOCAL：使用子组缓存的本端连接地址替换 nexthop（eBGP 的 next-hop-self、iBGP
 *          对 IMPORT 路由的反射均走此路径）；处理双栈/RFC 8950 family 转换。
 * R_PASS： 保留 best->nexthop 原值。
 * R_CONFIG：预留未实现，当前回退到 PASS。
 */
static void bgp_subgroup_apply_nexthop(const bgp_nh_subgroup_t *sg, const bgp_route_node_t *best, bgp_nexthop_t *out_nh)
{
    memcpy(out_nh, &best->nexthop, sizeof(*out_nh));

    if (!sg || sg->key.rule != BGP_NH_RULE_LOCAL)
    {
        return; /* PASS / CONFIG：保留原 nh */
    }

    const net_addr_t *local = &sg->key.effective_local_addr;
    if (local->family == 0 || net_addr_is_zero(local))
    {
        return; /* 本端地址无效：无法替换，保留原 nh */
    }

    /* 同族 nexthop 替换（传统场景） */
    if (local->family == out_nh->global.family || out_nh->global.family == 0)
    {
        out_nh->global = *local;
        out_nh->has_link_local = false;
        memset(&out_nh->link_local, 0, sizeof(out_nh->link_local));
        return;
    }

    /* 双栈：IPv6 路由 + IPv4 本端 → 使用本端 IPv4 */
    if (local->family == AF_INET && out_nh->global.family == AF_INET6)
    {
        out_nh->global = *local;
        out_nh->has_link_local = false;
        memset(&out_nh->link_local, 0, sizeof(out_nh->link_local));
        return;
    }

    /* RFC 8950：IPv4 路由 + IPv6 本端（需 EXT_NEXTHOP 能力）
     * 子组级无法精确判断每个 session 的能力；保守做法——只要本端是 IPv6 就替换，
     * 具体 session 若未协商 EXT_NEXTHOP 将由发送阶段检查过滤。 */
    if (local->family == AF_INET6 && out_nh->global.family == AF_INET)
    {
        out_nh->global = *local;
        out_nh->has_link_local = false;
        memset(&out_nh->link_local, 0, sizeof(out_nh->link_local));
    }
}

bool bgp_subgroup_eval_export(const bgp_nh_subgroup_t *sg, const bgp_route_node_t *best, const bgp_nlri_entry_t *nlri,
                              bgp_attr_t *out_attr, bgp_nexthop_t *out_nh)
{
    (void)nlri;
    if (!sg || !best || !out_attr || !out_nh)
    {
        return false;
    }

    bgp_sess_type_t stype = sg->parent ? sg->parent->key.sess_type : BGP_SESS_TYPE_UNKNOWN;

    /* iBGP→iBGP 反射检查（子组级，sess_type 统一） */
    if (stype == BGP_SESS_TYPE_IBGP && !BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT))
    {
        const bgp_session_t *src_sess = bgp_best_source_session(best);
        if (src_sess && src_sess->sess_type == BGP_SESS_TYPE_IBGP)
        {
            return false;
        }
    }

    /* 属性准备：复制 + eBGP AS_PATH prepend */
    memcpy(out_attr, BGP_ROUTE_ATTR(best), sizeof(*out_attr));

    if (stype == BGP_SESS_TYPE_EBGP)
    {
        uint32_t local_as = bgp_work_local_as_number();
        int n;
        if (BGP_ROUTE_ATTR(best)->as_path[0] != '\0')
        {
            n = g_snprintf(out_attr->as_path, sizeof(out_attr->as_path), "%u %s", local_as,
                           BGP_ROUTE_ATTR(best)->as_path);
        }
        else
        {
            n = g_snprintf(out_attr->as_path, sizeof(out_attr->as_path), "%u", local_as);
        }
        if (n < 0 || (size_t)n >= sizeof(out_attr->as_path))
        {
            LOG_WARN("BGP: subgroup eval_export AS_PATH prepend overflow ug_id=%u local_as=%u",
                     sg->parent ? sg->parent->group_id : 0u, local_as);
            return false;
        }
    }

    /* nexthop 策略应用 */
    bgp_subgroup_apply_nexthop(sg, best, out_nh);
    return true;
}

void bgp_work_enqueue_announce_to_subgroups(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !nlri || !inst->rib)
    {
        return;
    }

    const bgp_route_node_t *best = bgp_rib_find_best(inst->rib, nlri);
    if (!best)
    {
        /* 无 best：走 withdraw 通路 */
        return;
    }

    bgp_route_src_class_t src_class = bgp_classify_route_src(best);

    uint32_t scheduled = 0;
    for (GList *ul = inst->update_groups; ul; ul = ul->next)
    {
        bgp_update_group_t *ug = (bgp_update_group_t *)ul->data;
        if (!ug)
        {
            continue;
        }

        /* 为本 (ug, route) 选择 rule；若拒绝（如 iBGP split-horizon），确保 ug
         * 内已有的 ARO 条目被撤销。 */
        bgp_nh_rule_t rule;
        bool accepted = bgp_select_nh_rule(&ug->key, src_class, &rule);
        if (!accepted)
        {
            /* 对本 ug 下所有子组尝试撤销该 NLRI（若存在于其中） */
            for (GList *sl = ug->subgroups; sl; sl = sl->next)
            {
                bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
                if (!sg || sg->peer_count == 0U || !sg->adj_rib_out)
                {
                    continue;
                }
                if (bgp_adj_rib_out_remove(sg->adj_rib_out, nlri))
                {
                    bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                    memcpy(copy, nlri, sizeof(*copy));
                    g_queue_push_tail(sg->withdraw_queue, copy);
                    sg->withdraw_count++;
                    scheduled++;
                }
            }
            continue;
        }

        bgp_nh_subgroup_t *sg = bgp_update_group_find_subgroup_by_rule(ug, rule);
        if (!sg || sg->peer_count == 0U || !sg->adj_rib_out)
        {
            continue;
        }

        /* 本次分派到的目标子组是 sg；其它 rule 的子组若残留该 NLRI 需撤销
         * （例如一条路由 best 变化后 src_class 改变、从 PASS 迁到 LOCAL）。 */
        for (GList *sl = ug->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *other = (bgp_nh_subgroup_t *)sl->data;
            if (!other || other == sg || !other->adj_rib_out)
            {
                continue;
            }
            if (bgp_adj_rib_out_remove(other->adj_rib_out, nlri))
            {
                bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                memcpy(copy, nlri, sizeof(*copy));
                g_queue_push_tail(other->withdraw_queue, copy);
                other->withdraw_count++;
                scheduled++;
            }
        }

        bgp_attr_t out_attr;
        bgp_nexthop_t out_nh;
        if (!bgp_subgroup_eval_export(sg, best, nlri, &out_attr, &out_nh))
        {
            if (bgp_adj_rib_out_remove(sg->adj_rib_out, nlri))
            {
                bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                memcpy(copy, nlri, sizeof(*copy));
                g_queue_push_tail(sg->withdraw_queue, copy);
                sg->withdraw_count++;
                scheduled++;
            }
            continue;
        }

        bgp_attr_ref_t *ref = bgp_attr_intern(&out_attr);
        if (!ref)
        {
            continue;
        }

        bgp_aro_change_t ch = bgp_adj_rib_out_update(sg->adj_rib_out, nlri, ref, &out_nh);
        bgp_attr_release(ref); /* adj_rib_out 已持有自己的引用 */

        if (ch != BGP_ARO_UNCHANGED)
        {
            bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
            memcpy(copy, nlri, sizeof(*copy));
            g_queue_push_tail(sg->announce_queue, copy);
            sg->announce_count++;
            scheduled++;
        }
    }

    if (scheduled > 0u)
    {
        bgp_work_schedule_session_pub(inst);
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: enqueue announce to subgroups nlri=%s afi=%u safi=%u scheduled=%u", nlri_str,
                  (unsigned)inst->afi, (unsigned)inst->safi, scheduled);
    }
}

void bgp_work_enqueue_withdraw_to_subgroups(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !nlri)
    {
        return;
    }

    uint32_t scheduled = 0;
    for (GList *ul = inst->update_groups; ul; ul = ul->next)
    {
        bgp_update_group_t *ug = (bgp_update_group_t *)ul->data;
        if (!ug)
        {
            continue;
        }
        for (GList *sl = ug->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (!sg || sg->peer_count == 0u || !sg->adj_rib_out)
            {
                continue;
            }

            if (bgp_adj_rib_out_remove(sg->adj_rib_out, nlri))
            {
                bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
                memcpy(copy, nlri, sizeof(*copy));
                g_queue_push_tail(sg->withdraw_queue, copy);
                sg->withdraw_count++;
                scheduled++;
            }
        }
    }

    if (scheduled > 0u)
    {
        bgp_work_schedule_session_pub(inst);
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: enqueue withdraw to subgroups nlri=%s afi=%u safi=%u scheduled=%u", nlri_str,
                  (unsigned)inst->afi, (unsigned)inst->safi, scheduled);
    }
}

/** bgp_rib_foreach_best 回调：对每条 best 触发 announce 入队 */
static void catchup_populate_best_cb(const bgp_rthead_t *head, const bgp_route_node_t *route, gpointer user_data)
{
    (void)route;
    bgp_instance_t *inst = (bgp_instance_t *)user_data;
    if (!head || !inst)
    {
        return;
    }
    bgp_work_enqueue_announce_to_subgroups(inst, &head->nlri);
}

/** bgp_adj_rib_out_foreach 回调：将 NLRI 重新推入该 subgroup 的 announce_queue（补发） */
static void catchup_requeue_aro_cb(const bgp_nlri_entry_t *nlri, const bgp_adj_rib_out_entry_t *entry,
                                   gpointer user_data)
{
    (void)entry;
    bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)user_data;
    if (!nlri || !sg || !sg->announce_queue)
    {
        return;
    }
    bgp_nlri_entry_t *copy = g_malloc(sizeof(*copy));
    memcpy(copy, nlri, sizeof(*copy));
    g_queue_push_tail(sg->announce_queue, copy);
    sg->announce_count++;
}

void bgp_work_subgroup_catchup_session(bgp_session_t *sess)
{
    if (!sess)
    {
        return;
    }

    uint32_t total_scheduled = 0;
    for (GList *l = sess->peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        if (!peer || !peer->inst)
        {
            continue;
        }
        bgp_instance_t *inst = peer->inst;

        /* 加入子组（幂等）；peer 可能归属多个 rule 的子组 */
        bgp_subgroup_peer_join(peer, sess);
        if (!peer->subgroups)
        {
            continue;
        }

        /* 统计是否所有子组都是空——是则做一次 full RIB 扫描；否则对非空子组做
         * ARO 补发。全量 RIB 扫描只需触发一次：enqueue_announce_to_subgroups 内部
         * 会分派到正确的 rule 子组。 */
        bool any_empty = false;
        bool any_populated = false;
        for (GList *sl = peer->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (!sg || !sg->adj_rib_out)
            {
                continue;
            }
            if (bgp_adj_rib_out_count(sg->adj_rib_out) == 0U)
            {
                any_empty = true;
            }
            else
            {
                any_populated = true;
            }
        }

        uint32_t before_total = 0;
        for (GList *sl = peer->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (sg)
            {
                before_total += sg->announce_count;
            }
        }

        if (any_empty && inst->rib)
        {
            bgp_rib_foreach_best(inst->rib, catchup_populate_best_cb, inst);
        }
        if (any_populated)
        {
            for (GList *sl = peer->subgroups; sl; sl = sl->next)
            {
                bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
                if (sg && sg->adj_rib_out && bgp_adj_rib_out_count(sg->adj_rib_out) > 0U)
                {
                    bgp_adj_rib_out_foreach(sg->adj_rib_out, catchup_requeue_aro_cb, sg);
                }
            }
        }

        uint32_t after_total = 0;
        for (GList *sl = peer->subgroups; sl; sl = sl->next)
        {
            bgp_nh_subgroup_t *sg = (bgp_nh_subgroup_t *)sl->data;
            if (sg)
            {
                after_total += sg->announce_count;
            }
        }
        if (after_total > before_total)
        {
            total_scheduled += (after_total - before_total);
            bgp_work_schedule_session_pub(inst);
        }
    }

    if (total_scheduled > 0u)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: neighbor=%s subgroup catchup scheduled %u NLRI(s) across %u peer(s)", addr_str, total_scheduled,
                 (unsigned)g_list_length(sess->peer_list));
    }
}

/* ---------------------------------------------------------------------------
 * Packed 发送辅助（Phase 3）
 * -------------------------------------------------------------------------*/

/** 打包发布的单条 NLRI 及 per-session 过滤所需的来源信息 */
typedef struct announce_item
{
    const bgp_nlri_entry_t *nlri; /**< 借用指针（指向 g_queue 出队后堆上 NLRI 副本） */
    net_addr_t source;            /**< best->source（family=0 表示无来源/is_import） */
    bool is_import;               /**< best 是否为 IMPORT 路由（IMPORT 不做 split-horizon） */
} announce_item_t;

/** 按 (attr_ref, nexthop) 聚合的宣告桶 */
typedef struct announce_bucket
{
    bgp_attr_ref_t *attr_ref; /**< 借用（不增减引用） */
    bgp_nexthop_t nexthop;
    GPtrArray *items; /**< announce_item_t*（堆副本，调用方释放） */
} announce_bucket_t;

static announce_bucket_t *announce_bucket_find_or_create(GList **buckets, bgp_attr_ref_t *attr_ref,
                                                         const bgp_nexthop_t *nh)
{
    for (GList *l = *buckets; l; l = l->next)
    {
        announce_bucket_t *bk = (announce_bucket_t *)l->data;
        if (bk->attr_ref == attr_ref && memcmp(&bk->nexthop, nh, sizeof(*nh)) == 0)
        {
            return bk;
        }
    }
    announce_bucket_t *bk = g_new0(announce_bucket_t, 1);
    bk->attr_ref = attr_ref;
    bk->nexthop = *nh;
    bk->items = g_ptr_array_new();
    *buckets = g_list_prepend(*buckets, bk);
    return bk;
}

static void announce_bucket_free(announce_bucket_t *bk)
{
    if (!bk)
    {
        return;
    }
    if (bk->items)
    {
        for (guint i = 0; i < bk->items->len; i++)
        {
            announce_item_t *it = g_ptr_array_index(bk->items, i);
            g_free(it);
        }
        g_ptr_array_free(bk->items, TRUE);
    }
    g_free(bk);
}

/**
 * @brief 向单个 session 按 4096 字节报文上限循环发送 packed WITHDRAW
 */
static void subgroup_send_packed_withdraws(bgp_session_t *sess, uint16_t afi, uint8_t safi,
                                           const bgp_nlri_entry_t *const *nlri_list, int nlri_count)
{
    if (!sess || !sess->pri_conn || nlri_count <= 0)
    {
        return;
    }
    uint8_t msg[4096];
    int remaining = nlri_count;
    const bgp_nlri_entry_t *const *cursor = nlri_list;
    while (remaining > 0)
    {
        int packed = 0;
        int len =
            bgp_pkt_build_packed_withdraw(msg, (int)sizeof(msg), cursor, remaining, sess->pri_conn, afi, safi, &packed);
        if (len <= 0 || packed <= 0)
        {
            LOG_WARN("BGP: packed WITHDRAW build failed afi=%u safi=%u remaining=%d", (unsigned)afi, (unsigned)safi,
                     remaining);
            break;
        }
        ssize_t sent = send(sess->pri_conn->fd, msg, (size_t)len, MSG_NOSIGNAL);
        if (sent != (ssize_t)len)
        {
            LOG_WARN("BGP: packed WITHDRAW send incomplete (sent=%zd want=%d)", sent, len);
            break;
        }
        cursor += packed;
        remaining -= packed;
    }
}

/**
 * @brief 向单个 session 按 4096 字节报文上限循环发送 packed UPDATE
 */
static void subgroup_send_packed_updates(bgp_session_t *sess, uint16_t afi, uint8_t safi,
                                         const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                         const bgp_attr_t *attr, const bgp_nexthop_t *nh)
{
    if (!sess || !sess->pri_conn || nlri_count <= 0 || !attr || !nh)
    {
        return;
    }
    uint8_t msg[4096];
    int remaining = nlri_count;
    const bgp_nlri_entry_t *const *cursor = nlri_list;
    while (remaining > 0)
    {
        int packed = 0;
        int len = bgp_pkt_build_packed_update(msg, (int)sizeof(msg), cursor, remaining, attr, nh, afi, safi, &packed);
        if (len <= 0 || packed <= 0)
        {
            LOG_WARN("BGP: packed UPDATE build failed afi=%u safi=%u remaining=%d", (unsigned)afi, (unsigned)safi,
                     remaining);
            break;
        }
        ssize_t sent = send(sess->pri_conn->fd, msg, (size_t)len, MSG_NOSIGNAL);
        if (sent != (ssize_t)len)
        {
            LOG_WARN("BGP: packed UPDATE send incomplete (sent=%zd want=%d)", sent, len);
            break;
        }
        cursor += packed;
        remaining -= packed;
    }
}

int bgp_subgroup_process_queues(bgp_nh_subgroup_t *sg, bgp_instance_t *inst, int batch_size)
{
    if (!sg || !inst || batch_size <= 0)
    {
        return 0;
    }

    uint16_t afi = (uint16_t)inst->afi;
    uint8_t safi = (uint8_t)inst->safi;
    int processed = 0;

    /* --- Packed WITHDRAW --- */
    GPtrArray *wd_nlris = g_ptr_array_new();
    while (processed < batch_size && sg->withdraw_queue && !g_queue_is_empty(sg->withdraw_queue))
    {
        bgp_nlri_entry_t *nlri = (bgp_nlri_entry_t *)g_queue_pop_head(sg->withdraw_queue);
        if (!nlri)
        {
            break;
        }
        g_ptr_array_add(wd_nlris, nlri);
        processed++;
    }
    if (wd_nlris->len > 0)
    {
        for (GList *l = sg->peer_list; l; l = l->next)
        {
            bgp_peer_t *peer = (bgp_peer_t *)l->data;
            if (!peer || !peer->vrf)
            {
                continue;
            }
            bgp_session_t *sess = bgp_vrf_find_session(peer->vrf, &peer->addr);
            if (!bgp_session_is_publish_ready(sess))
            {
                continue;
            }
            subgroup_send_packed_withdraws(sess, afi, safi, (const bgp_nlri_entry_t *const *)wd_nlris->pdata,
                                           (int)wd_nlris->len);
        }
    }
    for (guint i = 0; i < wd_nlris->len; i++)
    {
        g_free(g_ptr_array_index(wd_nlris, i));
    }
    g_ptr_array_free(wd_nlris, TRUE);

    /* --- Packed ANNOUNCE：按 (attr_ref, nh) 分桶 --- */
    GList *buckets = NULL;
    GPtrArray *ann_nlris = g_ptr_array_new(); /* 记录所有出队 NLRI 堆副本（最终统一释放） */
    while (processed < batch_size && sg->announce_queue && !g_queue_is_empty(sg->announce_queue))
    {
        bgp_nlri_entry_t *nlri = (bgp_nlri_entry_t *)g_queue_pop_head(sg->announce_queue);
        if (!nlri)
        {
            break;
        }
        processed++;
        g_ptr_array_add(ann_nlris, nlri);

        const bgp_adj_rib_out_entry_t *entry = bgp_adj_rib_out_lookup(sg->adj_rib_out, nlri);
        if (!entry || !entry->attr_ref)
        {
            continue;
        }
        announce_bucket_t *bk = announce_bucket_find_or_create(&buckets, entry->attr_ref, &entry->nexthop);

        announce_item_t *item = g_new0(announce_item_t, 1);
        item->nlri = nlri;
        const bgp_route_node_t *best = inst->rib ? bgp_rib_find_best(inst->rib, nlri) : NULL;
        if (best)
        {
            item->source = best->source;
            item->is_import = BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT);
        }
        else
        {
            item->source.family = 0;
            item->is_import = false;
        }
        g_ptr_array_add(bk->items, item);
    }

    /* 遍历桶并发送 */
    for (GList *bl = buckets; bl; bl = bl->next)
    {
        announce_bucket_t *bk = (announce_bucket_t *)bl->data;
        if (!bk || !bk->attr_ref || !bk->items || bk->items->len == 0)
        {
            continue;
        }
        for (GList *sl = sg->peer_list; sl; sl = sl->next)
        {
            bgp_peer_t *peer = (bgp_peer_t *)sl->data;
            if (!peer || !peer->vrf)
            {
                continue;
            }
            bgp_session_t *sess = bgp_vrf_find_session(peer->vrf, &peer->addr);
            if (!bgp_session_is_publish_ready(sess))
            {
                continue;
            }
            /* Per-session AS_PATH 防环（同桶共用 attr） */
            if (sess->remote_as != 0U && bgp_as_path_contains_as(bk->attr_ref->attr.as_path, sess->remote_as))
            {
                continue;
            }
            /* 逐条过滤 split-horizon，构建 per-session NLRI 列表 */
            GPtrArray *filtered = g_ptr_array_new();
            for (guint i = 0; i < bk->items->len; i++)
            {
                announce_item_t *it = g_ptr_array_index(bk->items, i);
                if (!it->is_import && it->source.family != 0 && net_addr_equal(&it->source, &sess->neighbor_addr))
                {
                    continue;
                }
                g_ptr_array_add(filtered, (gpointer)it->nlri);
            }
            if (filtered->len > 0)
            {
                subgroup_send_packed_updates(sess, afi, safi, (const bgp_nlri_entry_t *const *)filtered->pdata,
                                             (int)filtered->len, &bk->attr_ref->attr, &bk->nexthop);
            }
            g_ptr_array_free(filtered, TRUE);
        }
        /* 标记 advertised */
        for (guint i = 0; i < bk->items->len; i++)
        {
            announce_item_t *it = g_ptr_array_index(bk->items, i);
            bgp_adj_rib_out_mark_advertised(sg->adj_rib_out, it->nlri);
        }
    }

    /* 清理 */
    for (GList *bl = buckets; bl; bl = bl->next)
    {
        announce_bucket_free((announce_bucket_t *)bl->data);
    }
    g_list_free(buckets);
    for (guint i = 0; i < ann_nlris->len; i++)
    {
        g_free(g_ptr_array_index(ann_nlris, i));
    }
    g_ptr_array_free(ann_nlris, TRUE);

    return processed;
}
