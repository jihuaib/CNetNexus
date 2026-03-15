/**
 * @file   bgp_calc.c
 * @brief  BGP 路由优选实现（Loc-RIB 最优路径计算）
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_calc.h"

#include <stdbool.h>

#include "bgp_rib.h"
#include "bgp_work.h"
#include "log.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 释放单条优选路由条目
 *
 * 当前所有字段均为值语义（固定数组），g_free 足以完成清理。
 * 若将来在 bgp_bestpath_entry_t 中加入堆指针字段，在此统一释放。
 */
static void bestpath_entry_free(bgp_bestpath_entry_t *entry)
{
    if (!entry)
    {
        return;
    }
    /* TODO: 若将来添加堆指针字段（如 char *as_path），在此 g_free 后再释放 */
    g_free(entry);
}

/** 按 NLRI 查找链表节点，未找到返回 NULL */
static GList *bestlist_find_node(const bgp_bestlist_t *list, const bgp_nlri_entry_t *nlri)
{
    for (GList *node = list->entries; node; node = node->next)
    {
        const bgp_bestpath_entry_t *entry = node->data;
        if (bgp_nlri_equal(&entry->nlri, nlri))
        {
            return node;
        }
    }
    return NULL;
}

// ============================================================================
// 链表生命周期
// ============================================================================

bgp_bestlist_t *bgp_bestlist_create(void)
{
    return g_malloc0(sizeof(bgp_bestlist_t));
}

void bgp_bestlist_destroy(bgp_bestlist_t *list)
{
    if (!list)
    {
        return;
    }
    for (GList *node = list->entries; node; node = node->next)
    {
        bestpath_entry_free(node->data);
    }
    g_list_free(list->entries);
    g_free(list);
}

// ============================================================================
// 条目管理
// ============================================================================

int bgp_bestlist_set(bgp_bestlist_t *list, const bgp_nlri_entry_t *nlri, const bgp_attr_t *attr,
                     const bgp_nexthop_t *nexthop, const net_addr_t *source)
{
    if (!list || !nlri || !attr || !nexthop || !source)
    {
        return -1;
    }

    GList *node = bestlist_find_node(list, nlri);

    if (node)
    {
        /* 已存在：更新除 NLRI 以外的全部字段 */
        bgp_bestpath_entry_t *entry = node->data;
        entry->attr = *attr;
        entry->nexthop = *nexthop;
        entry->source = *source;
        char key[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, key, sizeof(key));
        LOG_DEBUG("BGP: bestlist 更新路由 key=%s", key);
    }
    else
    {
        /* 新增 */
        bgp_bestpath_entry_t *entry = g_malloc0(sizeof(bgp_bestpath_entry_t));
        entry->nlri = *nlri;
        entry->attr = *attr;
        entry->nexthop = *nexthop;
        entry->source = *source;
        list->entries = g_list_append(list->entries, entry);
        list->count++;
        char key[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, key, sizeof(key));
        LOG_DEBUG("BGP: bestlist 新增路由 key=%s count=%u", key, list->count);
    }
    return 0;
}

int bgp_bestlist_del(bgp_bestlist_t *list, const bgp_nlri_entry_t *nlri)
{
    if (!list || !nlri)
    {
        return -1;
    }

    GList *node = bestlist_find_node(list, nlri);
    if (!node)
    {
        return -1;
    }

    bestpath_entry_free(node->data);
    list->entries = g_list_delete_link(list->entries, node);
    list->count--;
    char key[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(nlri, key, sizeof(key));
    LOG_DEBUG("BGP: bestlist 删除路由 key=%s count=%u", key, list->count);
    return 0;
}

const bgp_bestpath_entry_t *bgp_bestlist_find(const bgp_bestlist_t *list, const bgp_nlri_entry_t *nlri)
{
    if (!list || !nlri)
    {
        return NULL;
    }
    GList *node = bestlist_find_node(list, nlri);
    return node ? (const bgp_bestpath_entry_t *)node->data : NULL;
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
     *   1. 若该 NLRI 有路由：选出最优 bgp_route_node_t，调用 bgp_bestlist_set()
     *   2. 若该 NLRI 已无路由（全部撤销）：调用 bgp_bestlist_del()
     */
    LOG_DEBUG("BGP: calc_run afi=%u safi=%u（占位，暂未实现）", (unsigned)inst->afi, (unsigned)inst->safi);
    return 0;
}

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

    /* 无路由（rthead 不存在或路径哈希为空）：同步发送 WITHDRAW，再从 bestlist 删除 */
    if (!head || g_hash_table_size(head->route_hash) == 0)
    {
        /* 取出 bestlist 中的条目，用其 NLRI 信息同步发送 WITHDRAW */
        const bgp_bestpath_entry_t *best_entry = bgp_bestlist_find(inst->bestlist, nlri);
        if (best_entry)
        {
            bgp_work_send_withdraw_to_all(inst, &best_entry->nlri);
        }
        bgp_bestlist_del(inst->bestlist, nlri);
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

    /* 更新 Loc-RIB bestlist */
    bgp_bestlist_set(inst->bestlist, &head->nlri, &ctx.best->attr, &ctx.best->nexthop, &ctx.best->source);

    /* 将 NLRI 推入发布队列，异步向所有 ESTABLISHED 邻居宣告 */
    if (inst->pub_queue)
    {
        bgp_pub_queue_push(inst->pub_queue, &head->nlri);
    }

    char key[BGP_NLRI_KEY_MAX];
    bgp_nlri_to_str(&head->nlri, key, sizeof(key));
    LOG_DEBUG("BGP: calc_run_one ANNOUNCE key=%s afi=%u safi=%u", key, (unsigned)inst->afi, (unsigned)inst->safi);
}
