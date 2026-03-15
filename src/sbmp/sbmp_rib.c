/**
 * @file   sbmp_rib.c
 * @brief  SBMP 内存路由表实现（独立于 bgp_rib，按 NLRI 二进制比较索引）
 * @author jhb
 * @date   2026/03/15
 */
#include "sbmp_rib.h"

#include <string.h>

#include "net_addr.h"

/* ============================================================================
 * NLRI 二进制比较
 * ============================================================================
 * 树键直接使用 &head->nlri（bgp_nlri_entry_t*），无需额外堆分配。
 * 按 NLRI 类型分支做字段级二进制比较，保证全序关系。
 * ========================================================================== */

/**
 * @brief GTree 比较函数：按 NLRI 类型分支做二进制字段比较
 *
 * 键类型：const bgp_nlri_entry_t*（指向 sbmp_rthead_t.nlri，无需堆分配）
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

static sbmp_rthead_t *rthead_create(const bgp_nlri_entry_t *nlri)
{
    sbmp_rthead_t *head = g_malloc0(sizeof(sbmp_rthead_t));
    if (nlri)
    {
        memcpy(&head->nlri, nlri, sizeof(*nlri));
    }
    /* key: net_addr_t*（堆分配，g_free 释放），value: sbmp_route_t*（g_free 释放） */
    head->route_hash = g_hash_table_new_full(net_addr_hash, net_addr_hash_equal, g_free, g_free);
    return head;
}

static void rthead_destroy(sbmp_rthead_t *head)
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
    rthead_destroy((sbmp_rthead_t *)value);
    return FALSE;
}

/**
 * @brief 按 NLRI 从树中移除并销毁对应 rthead
 */
static gboolean remove_head_by_nlri(sbmp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    sbmp_rthead_t *head = g_tree_lookup(rib->head_tree, nlri);
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

sbmp_rib_t *sbmp_rib_create(void)
{
    sbmp_rib_t *rib = g_malloc0(sizeof(sbmp_rib_t));
    rib->head_tree = g_tree_new(nlri_compare);
    return rib;
}

void sbmp_rib_destroy(sbmp_rib_t *rib)
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

int sbmp_rib_reach_one(sbmp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source, const bgp_attr_t *attr,
                       const bgp_nexthop_t *nexthop)
{
    if (!rib || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    sbmp_rthead_t *head = g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        head = rthead_create(nlri);
        /* 键直接指向 head->nlri，无需额外堆分配 */
        g_tree_insert(rib->head_tree, &head->nlri, head);
        rib->head_count++;
    }

    sbmp_route_t *route = g_hash_table_lookup(head->route_hash, source);
    gboolean is_new = (route == NULL);
    if (is_new)
    {
        route = g_malloc0(sizeof(sbmp_route_t));
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

int sbmp_rib_unreach_one(sbmp_rib_t *rib, const bgp_nlri_entry_t *nlri, const net_addr_t *source)
{
    if (!rib || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    sbmp_rthead_t *head = g_tree_lookup(rib->head_tree, nlri);
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
        remove_head_by_nlri(rib, &head->nlri);
    }

    return 1;
}

/* ============================================================================
 * 批量应用与来源清理
 * ========================================================================== */

void sbmp_rib_apply_update(sbmp_rib_t *rib, const net_addr_t *source, const bgp_update_result_t *upd,
                           sbmp_rib_update_stats_t *stats)
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
        int rc = sbmp_rib_reach_one(rib, &upd->reach[i], source, &upd->attr, &upd->nexthop);
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
        int rc = sbmp_rib_unreach_one(rib, &upd->unreach[i], source);
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

typedef struct sbmp_source_purge_ctx
{
    const net_addr_t *source;
    uint32_t removed_routes;
    GPtrArray *empty_heads; /**< sbmp_rthead_t*，route_hash 已清空，待从树中移除 */
} sbmp_source_purge_ctx_t;

static gboolean purge_source_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    sbmp_source_purge_ctx_t *ctx = (sbmp_source_purge_ctx_t *)user_data;
    sbmp_rthead_t *head = (sbmp_rthead_t *)value;

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

void sbmp_rib_remove_source(sbmp_rib_t *rib, const net_addr_t *source, uint32_t *removed_routes,
                            uint32_t *removed_heads)
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

    sbmp_source_purge_ctx_t ctx;
    ctx.source = source;
    ctx.removed_routes = 0;
    /* 不需要 free_func：head 内存由 remove_head_by_nlri 释放 */
    ctx.empty_heads = g_ptr_array_new();

    g_tree_foreach(rib->head_tree, purge_source_cb, &ctx);

    for (guint i = 0; i < ctx.empty_heads->len; i++)
    {
        sbmp_rthead_t *head = (sbmp_rthead_t *)g_ptr_array_index(ctx.empty_heads, i);
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

const sbmp_rthead_t *sbmp_rib_lookup_head(const sbmp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    if (!rib || !rib->head_tree || !nlri)
    {
        return NULL;
    }
    return (const sbmp_rthead_t *)g_tree_lookup(rib->head_tree, nlri);
}

const sbmp_route_t *sbmp_rthead_lookup_route(const sbmp_rthead_t *head, const net_addr_t *source)
{
    if (!head || !head->route_hash || !source || source->family == 0)
    {
        return NULL;
    }
    return (const sbmp_route_t *)g_hash_table_lookup(head->route_hash, source);
}

uint32_t sbmp_rib_head_count(const sbmp_rib_t *rib)
{
    return rib ? rib->head_count : 0;
}

uint32_t sbmp_rib_route_count(const sbmp_rib_t *rib)
{
    return rib ? rib->route_count : 0;
}
