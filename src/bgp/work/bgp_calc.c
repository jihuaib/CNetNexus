/**
 * @file   bgp_calc.c
 * @brief  BGP 路由优选实现（best-path 计算）
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_calc.h"

#include <stdbool.h>
#include <sys/socket.h>

#include "bgp_rib.h"
#include "bgp_work.h"
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
    bgp_rthead_t *head = (bgp_rthead_t *)bgp_rib_lookup_head(inst->rib, nlri);
    const bgp_route_node_t *old_best = head ? bgp_rib_find_best(inst->rib, nlri) : NULL;
    int had_flushed = head ? head_has_flushed_route(head) : 0;

    /* 无路由（rthead 不存在或路径列表为空）：同步发送 WITHDRAW */
    if (!head || !head->route_list)
    {
        if (inst->route_flush_queue && (old_best || had_flushed))
        {
            bgp_route_flush_queue_push(inst->route_flush_queue, head);
        }
        bgp_work_send_withdraw_to_all(inst, nlri);
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
        for (GList *l = ((bgp_rthead_t *)head)->route_list; l; l = l->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)l->data;
            if (route)
            {
                BIT_CLR(route->flags, BGP_ROUTE_FLAG_BEST);
            }
        }
        if (inst->route_flush_queue && (old_best || had_flushed))
        {
            bgp_route_flush_queue_push(inst->route_flush_queue, head);
        }
        bgp_work_send_withdraw_to_all(inst, nlri);
        char key[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, key, sizeof(key));
        LOG_DEBUG("BGP: calc_run_one WITHDRAW(all-invalid) key=%s afi=%u safi=%u", key, (unsigned)inst->afi,
                  (unsigned)inst->safi);
        return;
    }

    /* 将最优路径移至链表首位 */
    bgp_rib_mark_best(inst->rib, &head->nlri, best);

    /* 将 NLRI 挂入各 ESTABLISHED 邻居的 session 发布队列 */
    bgp_work_enqueue_announce_to_established(inst, &head->nlri);

    const bgp_route_node_t *new_best = bgp_rib_find_best(inst->rib, &head->nlri);
    int best_switched = (old_best != new_best);
    int best_need_flush = (new_best && !BIT_TEST(new_best->flags, BGP_ROUTE_FLAG_FLUSHED));
    if (inst->route_flush_queue && (best_switched || best_need_flush))
    {
        bgp_route_flush_queue_push(inst->route_flush_queue, head);
    }

    char key[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(&head->nlri, key, sizeof(key));
    LOG_DEBUG("BGP: calc_run_one ANNOUNCE key=%s afi=%u safi=%u", key, (unsigned)inst->afi, (unsigned)inst->safi);
}
