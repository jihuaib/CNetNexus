/**
 * @file   bgp_rib.c
 * @brief  BGP 内存 RIB 通用结构实现（rthead-tree + per-head routes）
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
    /* key: net_addr_t*（堆分配，g_free 释放），value: bgp_route_node_t*（g_free 释放） */
    head->route_hash = g_hash_table_new_full(net_addr_hash, net_addr_hash_equal, g_free, g_free);
    return head;
}

static void rthead_destroy(bgp_rthead_t *head)
{
    if (!head)
    {
        return;
    }
    if (head->route_hash)
    {
        g_hash_table_destroy(head->route_hash);
        head->route_hash = NULL;
    }
    g_free(head);
}

/**
 * @brief 遍历回调：销毁所有 rthead（无 key 堆分配，只 free value）
 */
static gboolean destroy_tree_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key; /* 键指向 head->nlri，随 head 一起释放 */
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

int bgp_rib_reach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source, const bgp_attr_t *attr,
                      const bgp_nexthop_t *nexthop)
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

    bgp_route_node_t *route = g_hash_table_lookup(head->route_hash, source);
    gboolean is_new = (route == NULL);
    if (is_new)
    {
        route = g_malloc0(sizeof(bgp_route_node_t));
        net_addr_t *skey = g_malloc(sizeof(net_addr_t));
        *skey = *source;
        g_hash_table_insert(head->route_hash, skey, route);
        rib->route_count++;
    }

    route->source = *source;
    if (attr)
    {
        memcpy(&route->attr, attr, sizeof(*attr));
    }
    if (nexthop)
    {
        memcpy(&route->nexthop, nexthop, sizeof(*nexthop));
    }
    route->updated_at_usec = g_get_real_time();

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

    if (!g_hash_table_remove(head->route_hash, source))
    {
        return 0;
    }

    if (rib->route_count > 0)
    {
        rib->route_count--;
    }

    if (g_hash_table_size(head->route_hash) == 0)
    {
        /* 传入参数 nlri 与 head->nlri 内容相同，查找时会找到同一节点 */
        remove_head_by_nlri(rib, &head->nlri);
    }

    return 1;
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
        int rc = bgp_rib_reach_one(rib, &upd->reach[i], source, &upd->attr, &upd->nexthop);
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
    GPtrArray *empty_heads; /* bgp_rthead_t*，route_hash 已清空，待从树中移除 */
} source_purge_ctx_t;

static gboolean purge_source_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    source_purge_ctx_t *ctx = (source_purge_ctx_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;

    if (g_hash_table_remove(head->route_hash, ctx->source))
    {
        ctx->removed_routes++;
        if (g_hash_table_size(head->route_hash) == 0)
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
    /* 不需要 free_func：head 内存由 remove_head_by_nlri 释放 */
    ctx.empty_heads = g_ptr_array_new();

    g_tree_foreach(rib->head_tree, purge_source_cb, &ctx);

    for (guint i = 0; i < ctx.empty_heads->len; i++)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)g_ptr_array_index(ctx.empty_heads, i);
        /* 用 head->nlri 作为查找键（内容不变，head 尚未销毁） */
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
    if (g_hash_table_lookup(head->route_hash, ctx->source))
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
    if (!head || !head->route_hash || !source || source->family == 0)
    {
        return NULL;
    }
    return (const bgp_route_node_t *)g_hash_table_lookup(head->route_hash, source);
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
 * is_best 标记操作
 * ========================================================================== */

/** g_hash_table_foreach 回调：清除单条路径的 BGP_ROUTE_FLAG_BEST 标记 */
static void clear_best_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    (void)user_data;
    BIT_CLR(((bgp_route_node_t *)value)->flags, BGP_ROUTE_FLAG_BEST);
}

void bgp_rib_mark_best(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *best_source)
{
    if (!rib || !nlri || !best_source)
    {
        return;
    }
    bgp_rthead_t *head = (bgp_rthead_t *)g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        return;
    }

    /* 先清除该前缀头下所有路径的 BEST 标记 */
    g_hash_table_foreach(head->route_hash, clear_best_cb, NULL);

    /* 再对最优路径置位 */
    bgp_route_node_t *best = (bgp_route_node_t *)g_hash_table_lookup(head->route_hash, best_source);
    if (best)
    {
        BIT_SET(best->flags, BGP_ROUTE_FLAG_BEST);
    }
}

/** g_hash_table_foreach 回调：查找带 BGP_ROUTE_FLAG_BEST 标记的路径 */
static void find_best_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    const bgp_route_node_t **result = user_data;
    const bgp_route_node_t *route = value;
    if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_BEST))
    {
        *result = route;
    }
}

const bgp_route_node_t *bgp_rib_find_best(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    if (!rib || !nlri)
    {
        return NULL;
    }
    const bgp_rthead_t *head = (const bgp_rthead_t *)g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        return NULL;
    }
    const bgp_route_node_t *result = NULL;
    g_hash_table_foreach(head->route_hash, find_best_cb, &result);
    return result;
}

/** foreach_best 遍历时的上下文 */
typedef struct
{
    bgp_rib_best_cb cb;
    gpointer user_data;
} foreach_best_ctx_t;

/** g_tree_foreach 回调：对每个 rthead 查找 is_best 路径并调用外部回调 */
static gboolean foreach_best_tree_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    foreach_best_ctx_t *ctx = user_data;
    const bgp_rthead_t *head = (const bgp_rthead_t *)value;

    const bgp_route_node_t *best = NULL;
    g_hash_table_foreach(head->route_hash, find_best_cb, &best);
    if (best)
    {
        ctx->cb(head, best, ctx->user_data);
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
