/**
 * @file   bgp_rib.c
 * @brief  BGP 内存 RIB 通用结构实现（rthead-tree + per-head routes 双向链表）
 * @author jhb
 * @date   2026/03/13
 */
#include "bgp_rib.h"

#include <string.h>

#include "net_addr.h"

/**
 * @brief GTree 比较函数：按 NLRI 类型分支做二进制字段比较
 *
 * 键类型：const bgp_nlri_entry_t*（指向 bgp_rthead_t.nlri，无需堆分配）
 */
static gint nlri_compare(gconstpointer pa, gconstpointer pb)
{
    const bgp_nlri_entry_t *a = (const bgp_nlri_entry_t *)pa;
    const bgp_nlri_entry_t *b = (const bgp_nlri_entry_t *)pb;
    return (gint)bgp_nlri_cmp(a, b);
}

/* ============================================================================
 * 内部辅助
 * ========================================================================== */

static bgp_rthead_t *rthead_create(const bgp_nlri_entry_t *nlri, bgp_rib_t *rib)
{
    bgp_rthead_t *head = g_malloc0(sizeof(bgp_rthead_t));
    if (nlri)
    {
        memcpy(&head->nlri, nlri, sizeof(*nlri));
    }
    head->inst = rib ? rib->inst : NULL;
    head->route_list = NULL; /* 路径列表，首元素为当前最优路径 */
    return head;
}

static void rthead_destroy(bgp_rthead_t *head)
{
    if (!head)
    {
        return;
    }
    /* 释放所有路径节点 */
    g_list_free_full(head->route_list, g_free);
    head->route_list = NULL;
    g_free(head);
}

/**
 * @brief 遍历回调：销毁所有 rthead
 */
static gboolean destroy_tree_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    (void)user_data;
    rthead_destroy((bgp_rthead_t *)value);
    return FALSE;
}

/**
 * @brief 按 NLRI 从树中移除并销毁对应 rthead
 */
static gboolean remove_head_by_nlri(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    bgp_rthead_t *head = g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        return FALSE;
    }
    /* 键指向 head->nlri，移除后再销毁 head（含 nlri 内存） */
    g_tree_remove(rib->head_tree, nlri);
    rthead_destroy(head);

    if (rib->head_count > 0)
    {
        rib->head_count--;
    }
    return TRUE;
}

/** 在链表中按 source 线性查找路径节点，找到返回节点指针，否则返回 NULL */
static bgp_route_node_t *route_list_find(GList *list, const net_addr_t *source)
{
    for (GList *l = list; l; l = l->next)
    {
        bgp_route_node_t *r = (bgp_route_node_t *)l->data;
        if (net_addr_equal(&r->source, source))
        {
            return r;
        }
    }
    return NULL;
}

/* ============================================================================
 * 生命周期
 * ========================================================================== */

bgp_rib_t *bgp_rib_create(void)
{
    bgp_rib_t *rib = g_malloc0(sizeof(bgp_rib_t));
    rib->head_tree = g_tree_new(nlri_compare);
    return rib;
}

void bgp_rib_destroy(bgp_rib_t *rib)
{
    if (!rib)
    {
        return;
    }

    if (rib->head_tree)
    {
        g_tree_foreach(rib->head_tree, destroy_tree_cb, NULL);
        g_tree_destroy(rib->head_tree);
        rib->head_tree = NULL;
    }

    g_free(rib);
}

/* ============================================================================
 * 单条 reach / unreach
 * ========================================================================== */

int bgp_rib_reach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source, uint32_t import_proto,
                      const bgp_attr_t *attr, const bgp_nexthop_t *nexthop)
{
    if (!rib || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    bgp_rthead_t *head = g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        head = rthead_create(nlri, rib);
        /* 键直接指向 head->nlri，无需额外堆分配 */
        g_tree_insert(rib->head_tree, &head->nlri, head);
        rib->head_count++;
    }

    bgp_route_node_t *route = route_list_find(head->route_list, source);
    gboolean is_new = (route == NULL);
    if (is_new)
    {
        route = g_malloc0(sizeof(bgp_route_node_t));
        route->source = *source;
        /* 新路径追加到链表尾部（不影响当前 best-first 排序） */
        head->route_list = g_list_append(head->route_list, route);
        rib->route_count++;
    }

    /* 按 import_proto 置/清 IMPORT 标记；reach 默认置为 valid，路径变更后清除 BEST（等待 calc 重新评选） */
    if (import_proto != 0)
    {
        BIT_SET(route->flags, BGP_ROUTE_FLAG_IMPORT);
    }
    else
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
    }
    BIT_SET(route->flags, BGP_ROUTE_FLAG_VALID);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_BEST);

    if (attr)
    {
        memcpy(&route->attr, attr, sizeof(*attr));
    }
    if (nexthop)
    {
        memcpy(&route->nexthop, nexthop, sizeof(*nexthop));
    }
    route->updated_at_usec = g_get_real_time();
    if (is_new)
    {
        route->added_at_usec = route->updated_at_usec;
    }

    return is_new ? 1 : 0;
}

int bgp_rib_unreach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source)
{
    if (!rib || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    bgp_rthead_t *head = g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        return 0;
    }

    bgp_route_node_t *route = route_list_find(head->route_list, source);
    if (!route)
    {
        return 0;
    }

    head->route_list = g_list_remove(head->route_list, route);
    g_free(route);

    if (rib->route_count > 0)
    {
        rib->route_count--;
    }

    if (!head->route_list)
    {
        remove_head_by_nlri(rib, &head->nlri);
    }

    return 1;
}

int bgp_rib_set_route_valid(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source, gboolean valid)
{
    if (!rib || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    bgp_rthead_t *head = g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        return 0;
    }

    bgp_route_node_t *route = route_list_find(head->route_list, source);
    if (!route)
    {
        return 0;
    }

    gboolean old_valid = BIT_TEST(route->flags, BGP_ROUTE_FLAG_VALID);
    if (valid)
    {
        BIT_SET(route->flags, BGP_ROUTE_FLAG_VALID);
    }
    else
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
    }

    if (old_valid != valid)
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_BEST);
        route->updated_at_usec = g_get_real_time();
        return 1;
    }
    return 0;
}

/* ============================================================================
 * 批量应用与来源清理
 * ========================================================================== */

void bgp_rib_apply_update(bgp_rib_t *rib, const net_addr_t *source, const bgp_update_result_t *upd,
                          bgp_rib_update_stats_t *stats)
{
    if (stats)
    {
        memset(stats, 0, sizeof(*stats));
    }
    if (!rib || !source || source->family == 0 || !upd)
    {
        return;
    }

    for (uint32_t i = 0; i < upd->reach_len; i++)
    {
        int rc = bgp_rib_reach_one(rib, &upd->reach[i], source, 0, &upd->attr, &upd->nexthop);
        if (!stats)
        {
            continue;
        }
        if (rc == 1)
        {
            stats->reach_new++;
        }
        else if (rc == 0)
        {
            stats->reach_update++;
        }
    }

    for (uint32_t i = 0; i < upd->unreach_len; i++)
    {
        int rc = bgp_rib_unreach_one(rib, &upd->unreach[i], source);
        if (!stats)
        {
            continue;
        }
        if (rc == 1)
        {
            stats->unreach_removed++;
        }
        else if (rc == 0)
        {
            stats->unreach_miss++;
        }
    }
}

typedef struct source_purge_ctx
{
    const net_addr_t *source;
    uint32_t removed_routes;
    GPtrArray *empty_heads; /* bgp_rthead_t*，route_list 已清空，待从树中移除 */
} source_purge_ctx_t;

static gboolean purge_source_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    source_purge_ctx_t *ctx = (source_purge_ctx_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;

    bgp_route_node_t *route = route_list_find(head->route_list, ctx->source);
    if (route)
    {
        head->route_list = g_list_remove(head->route_list, route);
        g_free(route);
        ctx->removed_routes++;
        if (!head->route_list)
        {
            g_ptr_array_add(ctx->empty_heads, head);
        }
    }
    return FALSE;
}

void bgp_rib_remove_source(bgp_rib_t *rib, const net_addr_t *source, uint32_t *removed_routes, uint32_t *removed_heads)
{
    if (removed_routes)
    {
        *removed_routes = 0;
    }
    if (removed_heads)
    {
        *removed_heads = 0;
    }
    if (!rib || !source || source->family == 0)
    {
        return;
    }

    source_purge_ctx_t ctx;
    ctx.source = source;
    ctx.removed_routes = 0;
    ctx.empty_heads = g_ptr_array_new();

    g_tree_foreach(rib->head_tree, purge_source_cb, &ctx);

    for (guint i = 0; i < ctx.empty_heads->len; i++)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)g_ptr_array_index(ctx.empty_heads, i);
        if (remove_head_by_nlri(rib, &head->nlri) && removed_heads)
        {
            (*removed_heads)++;
        }
    }

    if (rib->route_count >= ctx.removed_routes)
    {
        rib->route_count -= ctx.removed_routes;
    }
    else
    {
        rib->route_count = 0;
    }

    if (removed_routes)
    {
        *removed_routes = ctx.removed_routes;
    }

    g_ptr_array_free(ctx.empty_heads, TRUE);
}

/* ============================================================================
 * 查询
 * ========================================================================== */

typedef struct
{
    const net_addr_t *source;
    bgp_rib_source_nlri_cb cb;
    gpointer user_data;
} foreach_source_ctx_t;

static gboolean foreach_source_tree_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    foreach_source_ctx_t *ctx = user_data;
    bgp_rthead_t *head = value;
    if (route_list_find(head->route_list, ctx->source))
    {
        ctx->cb(&head->nlri, ctx->user_data);
    }
    return FALSE; /* 继续遍历 */
}

void bgp_rib_foreach_source(const bgp_rib_t *rib, const net_addr_t *source, bgp_rib_source_nlri_cb cb,
                            gpointer user_data)
{
    if (!rib || !rib->head_tree || !source || !cb)
    {
        return;
    }
    foreach_source_ctx_t ctx = {source, cb, user_data};
    g_tree_foreach(rib->head_tree, foreach_source_tree_cb, &ctx);
}

const bgp_rthead_t *bgp_rib_lookup_head(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    if (!rib || !rib->head_tree || !nlri)
    {
        return NULL;
    }
    return (const bgp_rthead_t *)g_tree_lookup(rib->head_tree, nlri);
}

const bgp_route_node_t *bgp_rthead_lookup_route(const bgp_rthead_t *head, const net_addr_t *source)
{
    if (!head || !source || source->family == 0)
    {
        return NULL;
    }
    return route_list_find((GList *)head->route_list, source);
}

uint32_t bgp_rib_head_count(const bgp_rib_t *rib)
{
    return rib ? rib->head_count : 0;
}

uint32_t bgp_rib_route_count(const bgp_rib_t *rib)
{
    return rib ? rib->route_count : 0;
}

/* ============================================================================
 * best-first 排序
 * ========================================================================== */

void bgp_rib_mark_best(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, bgp_route_node_t *best_route)
{
    if (!rib || !nlri || !best_route)
    {
        return;
    }
    bgp_rthead_t *head = (bgp_rthead_t *)g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        return;
    }

    /* 清除所有路径的 BEST 标记 */
    for (GList *l = head->route_list; l; l = l->next)
    {
        BIT_CLR(((bgp_route_node_t *)l->data)->flags, BGP_ROUTE_FLAG_BEST);
    }

    if (!BIT_TEST(best_route->flags, BGP_ROUTE_FLAG_VALID))
    {
        return;
    }

    /* 置目标路径 BEST 标记，并移至链表首位 */
    BIT_SET(best_route->flags, BGP_ROUTE_FLAG_BEST);
    head->route_list = g_list_remove(head->route_list, best_route);
    head->route_list = g_list_prepend(head->route_list, best_route);
}

const bgp_route_node_t *bgp_rib_find_best(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    if (!rib || !nlri)
    {
        return NULL;
    }
    const bgp_rthead_t *head = (const bgp_rthead_t *)g_tree_lookup(rib->head_tree, nlri);
    if (!head || !head->route_list)
    {
        return NULL;
    }
    /* 最优路径须同时满足：位于首位 且 具有 BEST+VALID 标记 */
    const bgp_route_node_t *first = (const bgp_route_node_t *)head->route_list->data;
    return (BIT_TEST(first->flags, BGP_ROUTE_FLAG_BEST) && BIT_TEST(first->flags, BGP_ROUTE_FLAG_VALID)) ? first : NULL;
}

/** foreach_best 遍历时的上下文 */
typedef struct
{
    bgp_rib_best_cb cb;
    gpointer user_data;
} foreach_best_ctx_t;

/** g_tree_foreach 回调：对每个 rthead 取首元素（最优路径）调用外部回调 */
static gboolean foreach_best_tree_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    foreach_best_ctx_t *ctx = user_data;
    const bgp_rthead_t *head = (const bgp_rthead_t *)value;

    if (head->route_list)
    {
        const bgp_route_node_t *first = (const bgp_route_node_t *)head->route_list->data;
        /* 同 bgp_rib_find_best：首位 且 同时有 BEST+VALID 标记才触发回调 */
        if (BIT_TEST(first->flags, BGP_ROUTE_FLAG_BEST) && BIT_TEST(first->flags, BGP_ROUTE_FLAG_VALID))
        {
            ctx->cb(head, first, ctx->user_data);
        }
    }
    return FALSE; /* 继续遍历 */
}

void bgp_rib_foreach_best(const bgp_rib_t *rib, bgp_rib_best_cb cb, gpointer user_data)
{
    if (!rib || !rib->head_tree || !cb)
    {
        return;
    }
    foreach_best_ctx_t ctx = {cb, user_data};
    g_tree_foreach(rib->head_tree, foreach_best_tree_cb, &ctx);
}
