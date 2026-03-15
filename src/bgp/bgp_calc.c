/**
 * @file   bgp_calc.c
 * @brief  BGP 路由优选实现（Loc-RIB 最优路径计算）
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_calc.h"

#include <string.h>

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

/** 按 NLRI key 查找链表节点，未找到返回 NULL */
static GList *bestlist_find_node(const bgp_bestlist_t *list, const bgp_nlri_entry_t *nlri)
{
    for (GList *node = list->entries; node; node = node->next)
    {
        const bgp_bestpath_entry_t *entry = node->data;
        if (strcmp(entry->nlri.key, nlri->key) == 0)
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
        LOG_DEBUG("BGP: bestlist 更新路由 key=%s", nlri->key);
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
        LOG_DEBUG("BGP: bestlist 新增路由 key=%s count=%u", nlri->key, list->count);
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
    LOG_DEBUG("BGP: bestlist 删除路由 key=%s count=%u", nlri->key, list->count);
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
