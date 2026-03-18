/**
 * @file   bgp_calc.c
 * @brief  BGP 路由优选实现（best-path 计算）
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_calc.h"

#include <stdbool.h>

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
 *   5. 更晚更新的路径（updated_at_usec 更大）
 */
static bool route_is_better(const bgp_route_node_t *candidate, const bgp_route_node_t *current)
{
    if (!candidate)
    {
        return false;
    }
    if (!current)
    {
        return true;
    }

    /* 1. LOCAL_PREF（越高越优，未携带时默认 100） */
    uint32_t ca_lp = candidate->attr.has_local_pref ? candidate->attr.local_pref : 100;
    uint32_t cu_lp = current->attr.has_local_pref ? current->attr.local_pref : 100;
    if (ca_lp != cu_lp)
    {
        return ca_lp > cu_lp;
    }

    /* 2. AS_PATH 长度（越短越优） */
    uint32_t ca_al = as_path_hop_count(candidate->attr.as_path);
    uint32_t cu_al = as_path_hop_count(current->attr.as_path);
    if (ca_al != cu_al)
    {
        return ca_al < cu_al;
    }

    /* 3. ORIGIN（越小越优：IGP < EGP < INCOMPLETE） */
    if (candidate->attr.origin != current->attr.origin)
    {
        return candidate->attr.origin < current->attr.origin;
    }

    /* 4. MED（仅两者均携带时比较，越小越优） */
    if (candidate->attr.has_med && current->attr.has_med && candidate->attr.med != current->attr.med)
    {
        return candidate->attr.med < current->attr.med;
    }

    /* 5. 最近更新时间（越晚越优） */
    return candidate->updated_at_usec > current->updated_at_usec;
}

/** g_hash_table_foreach 回调：在 route_hash 中选出最优路径 */
typedef struct
{
    const bgp_route_node_t *best;
} best_select_ctx_t;

static void select_best_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    best_select_ctx_t *ctx = user_data;
    const bgp_route_node_t *route = value;
    if (route_is_better(route, ctx->best))
    {
        ctx->best = route;
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
    const bgp_rthead_t *head = bgp_rib_lookup_head(inst->rib, nlri);

    /* 无路由（rthead 不存在或路径哈希为空）：同步发送 WITHDRAW */
    if (!head || g_hash_table_size(head->route_hash) == 0)
    {
        bgp_work_send_withdraw_to_all(inst, nlri);
        char key[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, key, sizeof(key));
        LOG_DEBUG("BGP: calc_run_one WITHDRAW key=%s afi=%u safi=%u", key, (unsigned)inst->afi, (unsigned)inst->safi);
        return;
    }

    /* 在所有来源路径中选出最优 */
    best_select_ctx_t ctx = {.best = NULL};
    g_hash_table_foreach(head->route_hash, select_best_cb, &ctx);
    if (!ctx.best)
    {
        return; /* 不应发生 */
    }

    /* 在 RIB 路由节点上置 BGP_ROUTE_FLAG_BEST 标记 */
    bgp_rib_mark_best(inst->rib, &head->nlri, &ctx.best->source);

    /* 将 NLRI 推入发布队列，异步向所有 ESTABLISHED 邻居宣告 */
    if (inst->pub_queue)
    {
        bgp_pub_queue_push(inst->pub_queue, &head->nlri);
    }

    char key[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(&head->nlri, key, sizeof(key));
    LOG_DEBUG("BGP: calc_run_one ANNOUNCE key=%s afi=%u safi=%u", key, (unsigned)inst->afi, (unsigned)inst->safi);
}
