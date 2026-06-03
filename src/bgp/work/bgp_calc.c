/**
 * @file   bgp_calc.c
 * @brief  BGP 路由优选实现（best-path 计算）
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_calc.h"

#include <stdbool.h>
#include <sys/socket.h>

#include "bgp_import_rib.h"
#include "bgp_rib.h"
#include "bgp_route_flush.h"
#include "bgp_update_group.h"
#include "bgp_vrf_export.h"
#include "bgp_vrf_import.h"
#include "bgp_worker.h"
#include "log.h"

// ============================================================================
// 路由优选辅助
// ============================================================================

/**
 * @brief 计算 AS_PATH 长度（以 AS 编号个数计，集合 {} 中的每个成员单独计数）
 */
static uint32_t as_path_hop_count(const char *path)
{
    if (!path || *path == '\0')
    {
        return 0;
    }
    uint32_t count = 0;
    int in_word = 0;
    for (const char *p = path; *p; p++)
    {
        if (*p == ' ' || *p == '{' || *p == '}')
        {
            in_word = 0;
        }
        else
        {
            if (!in_word)
            {
                count++;
                in_word = 1;
            }
        }
    }
    return count;
}

/**
 * @brief 判断 candidate 路径是否优于 current 路径
 *
 * 优选顺序（与 RFC 4271 §9.1.2 一致的简化版本）：
 *   1. LOCAL_PREF 更高（默认 100）
 *   2. AS_PATH 长度更短
 *   3. ORIGIN 更低（IGP=0 < EGP=1 < INCOMPLETE=2）
 *   4. MED 更低（仅两者均携带时比较）
 *   5. 前缀同族 nexthop 优先（IPv4 前缀优先 IPv4 nexthop，IPv6 前缀优先 IPv6 nexthop）
 *   6. 更晚更新的路径（updated_at_usec 更大）
 */
static bool route_is_better(const bgp_route_node_t *candidate, const bgp_route_node_t *current)
{
    if (!candidate || !BIT_TEST(candidate->flags, BGP_ROUTE_FLAG_VALID))
    {
        return false;
    }
    if (!current || !BIT_TEST(current->flags, BGP_ROUTE_FLAG_VALID))
    {
        return true;
    }

    /* 0. import-rib tiebreak：IP 迭代（非 mirror）优于隧道迭代（mirror） */
    int tb = bgp_import_rib_tiebreak(candidate, current);
    if (tb != 0)
    {
        return tb > 0;
    }

    /* 1. LOCAL_PREF（越高越优，未携带时默认 100） */
    uint32_t ca_lp = BGP_ROUTE_ATTR(candidate)->has_local_pref ? BGP_ROUTE_ATTR(candidate)->local_pref : 100;
    uint32_t cu_lp = BGP_ROUTE_ATTR(current)->has_local_pref ? BGP_ROUTE_ATTR(current)->local_pref : 100;
    if (ca_lp != cu_lp)
    {
        return ca_lp > cu_lp;
    }

    /* 2. AS_PATH 长度（越短越优） */
    uint32_t ca_al = as_path_hop_count(BGP_ROUTE_ATTR(candidate)->as_path);
    uint32_t cu_al = as_path_hop_count(BGP_ROUTE_ATTR(current)->as_path);
    if (ca_al != cu_al)
    {
        return ca_al < cu_al;
    }

    /* 3. ORIGIN（越小越优：IGP < EGP < INCOMPLETE） */
    if (BGP_ROUTE_ATTR(candidate)->origin != BGP_ROUTE_ATTR(current)->origin)
    {
        return BGP_ROUTE_ATTR(candidate)->origin < BGP_ROUTE_ATTR(current)->origin;
    }

    /* 4. MED（仅两者均携带时比较，越小越优） */
    if (BGP_ROUTE_ATTR(candidate)->has_med && BGP_ROUTE_ATTR(current)->has_med &&
        BGP_ROUTE_ATTR(candidate)->med != BGP_ROUTE_ATTR(current)->med)
    {
        return BGP_ROUTE_ATTR(candidate)->med < BGP_ROUTE_ATTR(current)->med;
    }

    /* 5. 前缀同族 nexthop 优先（用于双栈扩展下一跳场景的稳定优选）。 */
    sa_family_t prefix_family = 0;
    if (candidate->head && candidate->head->nlri.type == BGP_NLRI_PREFIX)
    {
        prefix_family = candidate->head->nlri.prefix.prefix.addr.family;
    }
    else if (current->head && current->head->nlri.type == BGP_NLRI_PREFIX)
    {
        prefix_family = current->head->nlri.prefix.prefix.addr.family;
    }
    if (prefix_family == AF_INET || prefix_family == AF_INET6)
    {
        bool ca_same_family = (candidate->nexthop.global.family == prefix_family);
        bool cu_same_family = (current->nexthop.global.family == prefix_family);
        if (ca_same_family != cu_same_family)
        {
            return ca_same_family;
        }
    }

    /* 6. 最近更新时间（越晚越优） */
    return candidate->updated_at_usec > current->updated_at_usec;
}

static int head_has_flushed_route(const bgp_rthead_t *head)
{
    if (!head)
    {
        return 0;
    }

    for (GList *l = head->route_list; l; l = l->next)
    {
        const bgp_route_node_t *route = (const bgp_route_node_t *)l->data;
        if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED))
        {
            return 1;
        }
    }
    return 0;
}

static void head_clear_best_flags(bgp_rthead_t *head)
{
    if (!head)
    {
        return;
    }
    for (GList *l = head->route_list; l; l = l->next)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)l->data;
        if (route)
        {
            BIT_CLR(route->flags, BGP_ROUTE_FLAG_BEST);
        }
    }
}

// ============================================================================
// 路由优选入口（占位）
// ============================================================================

int bgp_calc_run(bgp_instance_t *inst)
{
    if (!inst)
    {
        return -1;
    }
    /*
     * TODO: 遍历 inst->rib 的每个 rthead，对各 NLRI 执行路径优选：
     *   1. 若该 NLRI 有路由：选出最优 bgp_route_node_t，调用 bgp_rib_mark_best()
     *   2. 若该 NLRI 已无路由（全部撤销）：发送 WITHDRAW
     */
    LOG_DEBUG("BGP: calc_run afi=%u safi=%u（占位，暂未实现）", (unsigned)inst->afi, (unsigned)inst->safi);
    return 0;
}

// ============================================================================
// 单条 NLRI 优选入口
// ============================================================================

void bgp_calc_run_one(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !nlri)
    {
        return;
    }

    /* 通过 NLRI 内容在 RIB 中定位前缀头（与指针地址无关） */
    bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, nlri);
    bgp_rthead_t *head = (bgp_rthead_t *)bgp_rib_lookup_head(rib, nlri);
    const bgp_route_node_t *old_best = head ? bgp_rib_find_best(rib, nlri) : NULL;
    int had_flushed = head ? head_has_flushed_route(head) : 0;

    /* 无路由（rthead 不存在或路径列表为空）：同步发送 WITHDRAW */
    if (!head || !head->route_list)
    {
        if (inst->route_flush_queue && (old_best || had_flushed))
        {
            bgp_route_flush_queue_push(inst->route_flush_queue, head);
        }
        bgp_update_group_enqueue_withdraw(inst, nlri);
        bgp_import_rib_on_calc_done(inst, head, old_best, NULL);
        bgp_vrf_export_on_calc_done(inst, head);
        bgp_vrf_import_on_calc_done(inst, head);
        char key[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, key, sizeof(key));
        LOG_DEBUG("BGP: calc_run_one WITHDRAW key=%s afi=%u safi=%u", key, (unsigned)inst->afi, (unsigned)inst->safi);
        return;
    }

    /* 遍历路径列表，选出最优路径 */
    bgp_route_node_t *best = NULL;
    for (GList *l = head->route_list; l; l = l->next)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)l->data;
        if (route_is_better(route, best))
        {
            best = route;
        }
    }
    if (!best)
    {
        /* 全部为 invalid 路径：撤销该 NLRI 对外可达性 */
        head_clear_best_flags(head);
        if (inst->route_flush_queue && (old_best || had_flushed))
        {
            bgp_route_flush_queue_push(inst->route_flush_queue, head);
        }
        bgp_update_group_enqueue_withdraw(inst, nlri);
        bgp_import_rib_on_calc_done(inst, head, old_best, NULL);
        bgp_vrf_export_on_calc_done(inst, head);
        bgp_vrf_import_on_calc_done(inst, head);
        char key[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, key, sizeof(key));
        LOG_DEBUG("BGP: calc_run_one WITHDRAW(all-invalid) key=%s afi=%u safi=%u", key, (unsigned)inst->afi,
                  (unsigned)inst->safi);
        return;
    }

    /* QP 地址族在未启用 route-select 时不对外发布，也不应有 BEST 标记。 */
    if (inst->safi == BGP_SAFI_QP && !inst->route_select_enabled)
    {
        head_clear_best_flags(head);
        if (inst->route_flush_queue && (old_best || had_flushed))
        {
            bgp_route_flush_queue_push(inst->route_flush_queue, head);
        }
        bgp_update_group_enqueue_withdraw(inst, &head->nlri);
        char key[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&head->nlri, key, sizeof(key));
        LOG_DEBUG("BGP: calc_run_one WITHDRAW(qp route-select disabled) key=%s afi=%u safi=%u", key,
                  (unsigned)inst->afi, (unsigned)inst->safi);
        return;
    }

    /* 将最优路径移至链表首位 */
    bgp_rib_mark_best(rib, &head->nlri, best);

    /* 将 NLRI 挂入各 ESTABLISHED 邻居的 session 发布队列 */
    bgp_update_group_enqueue_announce(inst, &head->nlri);

    const bgp_route_node_t *new_best = bgp_rib_find_best(rib, &head->nlri);
    int best_switched = (old_best != new_best);
    int best_need_flush = (new_best && !BIT_TEST(new_best->flags, BGP_ROUTE_FLAG_FLUSHED));
    if (inst->route_flush_queue && (best_switched || best_need_flush))
    {
        bgp_route_flush_queue_push(inst->route_flush_queue, head);
    }

    bgp_import_rib_on_calc_done(inst, head, old_best, new_best);
    /* 私网 VRF 的 ipv4-unicast best 变化时，若 vpnv4 已使能，补推到 vrf-export pending */
    bgp_vrf_export_on_calc_done(inst, head);
    /* public vpnv4 best 变化时，按 import-RT 把 best 导入/撤出命中的私网 VRF */
    bgp_vrf_import_on_calc_done(inst, head);

    char key[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(&head->nlri, key, sizeof(key));
    LOG_DEBUG("BGP: calc_run_one ANNOUNCE key=%s afi=%u safi=%u", key, (unsigned)inst->afi, (unsigned)inst->safi);
}

// ============================================================================
// 优选队列（calc_queue）
// ============================================================================

static void bgp_calc_schedule(bgp_instance_t *inst);
static int bgp_calc_process_event(bgp_instance_t *inst, gboolean allow_reschedule);

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
        if (inst)
        {
            bgp_rib_head_unref(head);
        }
    }
    g_queue_free(q->q);
    g_free(q);
}

int bgp_calc_queue_push(bgp_calc_queue_t *q, bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!q || !inst || !nlri)
    {
        return -1;
    }

    bgp_rib_t *rib = bgp_inst_rib_ensure_for_nlri(inst, nlri);
    if (!rib)
    {
        return -1;
    }
    bgp_rthead_t *head = bgp_rib_ensure_head(rib, nlri);
    if (!head)
    {
        return -1;
    }

    bgp_rib_head_ref(head);
    g_queue_push_tail(q->q, head);
    q->count++;
    bgp_calc_schedule(inst);
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
        bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, &head->nlri);
        bgp_rib_head_unref(head);
        /* 收尾 GC：若 head 已空且队列引用全部释放，立即从 tree 摘除。
         * 否则下次 ensure_head 会命中这个残留空 head 复用——其 NLRI 仍带着旧值（例如 labeled
         * 的 label），导致按 head->nlri 编码的 announce 发出陈旧字段，对端 MPLS NHLFE 不匹配。 */
        if (rib)
        {
            (void)bgp_rib_gc_head(rib, head);
        }
        processed++;
    }
    if (processed > 0)
    {
        LOG_DEBUG("BGP: calc_queue afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", (unsigned)inst->afi,
                  (unsigned)inst->safi, processed, q->count);
    }
    bgp_import_rib_drain_after_calc(inst);
    return processed;
}

static int bgp_calc_process_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst || !inst->calc_queue)
    {
        return 0;
    }

    int processed = bgp_calc_queue_process(inst->calc_queue, inst, BGP_WORK_BATCH_SIZE);
    if (allow_reschedule && processed > 0 && inst->calc_queue->count > 0u)
    {
        bgp_calc_schedule(inst);
    }
    return processed;
}

static void bgp_calc_schedule(bgp_instance_t *inst)
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

    if (bgp_worker_is_current_thread())
    {
        (void)bgp_calc_process_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue calc work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

void bgp_calc_handle_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_worker_lookup_instance(vrf_id, afi, safi);
    (void)bgp_calc_process_event(inst, TRUE);
}

int bgp_calc_process_pending(bgp_instance_t *inst)
{
    return bgp_calc_process_event(inst, FALSE);
}
