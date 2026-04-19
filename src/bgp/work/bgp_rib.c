/**
 * @file   bgp_rib.c
 * @brief  BGP 内存 RIB 通用结构实现（rthead-tree + per-head routes 双向链表）
 * @author jhb
 * @date   2026/03/13
 */
#include "bgp_rib.h"

#include <string.h>

#include "bgp_attr_intern.h"
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
    head->queue_refcnt = 0;
    return head;
}

/** 释放路径节点前先 release 共享属性 */
static void route_node_free(gpointer data)
{
    bgp_route_node_t *route = (bgp_route_node_t *)data;
    if (route)
    {
        bgp_attr_release(route->attr);
        route->attr = NULL;
    }
    g_free(route);
}

static void rthead_destroy(bgp_rthead_t *head)
{
    if (!head)
    {
        return;
    }
    /* 释放所有路径节点（含属性引用） */
    g_list_free_full(head->route_list, route_node_free);
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
    if (head->queue_refcnt > 0)
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

void bgp_rib_head_ref(bgp_rthead_t *head)
{
    if (!head)
    {
        return;
    }
    head->queue_refcnt++;
}

void bgp_rib_head_unref(bgp_rthead_t *head)
{
    if (!head || head->queue_refcnt == 0)
    {
        return;
    }

    head->queue_refcnt--;
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

bgp_route_node_t *bgp_rthead_lookup_route_mut(bgp_rthead_t *head, const net_addr_t *source)
{
    if (!head || !source || source->family == 0)
    {
        return NULL;
    }
    return route_list_find(head->route_list, source);
}

bgp_route_node_t *bgp_rthead_create_route(bgp_rib_t *rib, bgp_rthead_t *head, const net_addr_t *source)
{
    if (!rib || !head || !source || source->family == 0)
    {
        return NULL;
    }

    bgp_route_node_t *route = g_malloc0(sizeof(*route));
    if (!route)
    {
        return NULL;
    }

    route->head = head;
    route->source = *source;
    /* 新路径追加到链表尾部（不影响当前 best-first 排序） */
    head->route_list = g_list_append(head->route_list, route);
    rib->route_count++;
    return route;
}

int bgp_rib_route_apply_reach(bgp_route_node_t *route, uint32_t import_proto, const bgp_attr_t *attr,
                              const bgp_nexthop_t *nexthop)
{
    if (!route)
    {
        return -1;
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
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_FLUSHED);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_STALE);

    if (attr)
    {
        bgp_attr_ref_t *new_ref = bgp_attr_intern(attr, BGP_ATTR_SRC_LOC_RIB);
        bgp_attr_release(route->attr);
        route->attr = new_ref;
    }
    if (nexthop)
    {
        memcpy(&route->nexthop, nexthop, sizeof(*nexthop));
    }
    /* reach 后迭代状态由 relay 更新，这里先清空，避免显示旧值。 */
    route->iter_watched = 0u;
    route->iter_resolved = 0u;
    route->iter_out_ifindex = 0u;
    route->iter_relay_addr.family = 0;
    route->updated_at_usec = g_get_real_time();
    if (route->added_at_usec == 0)
    {
        route->added_at_usec = route->updated_at_usec;
    }

    return 0;
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

    if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_STALE))
    {
        return 0;
    }

    if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED))
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_BEST);
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
        BIT_SET(route->flags, BGP_ROUTE_FLAG_STALE);
        route->updated_at_usec = g_get_real_time();
    }
    else
    {
        head->route_list = g_list_remove(head->route_list, route);
        route_node_free(route);

        if (rib->route_count > 0)
        {
            rib->route_count--;
        }

        if (!head->route_list)
        {
            remove_head_by_nlri(rib, &head->nlri);
        }
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

typedef struct source_purge_ctx
{
    const net_addr_t *source;
    uint32_t removed_routes;      /* 逻辑删除数量（包含转 stale） */
    uint32_t removed_phys_routes; /* 物理删除数量（用于 route_count 递减） */
    GPtrArray *empty_heads;       /* bgp_rthead_t*，route_list 已清空，待从树中移除 */
} source_purge_ctx_t;

static gboolean purge_source_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    source_purge_ctx_t *ctx = (source_purge_ctx_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;

    bgp_route_node_t *route = route_list_find(head->route_list, ctx->source);
    if (route)
    {
        /* 会话来源清理只处理 peer 路由；本地 import-route 即使 source 相同也不能被清掉。 */
        if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_IMPORT))
        {
            return FALSE;
        }

        if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_STALE))
        {
            return FALSE;
        }
        ctx->removed_routes++;

        if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED))
        {
            BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
            BIT_CLR(route->flags, BGP_ROUTE_FLAG_BEST);
            BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
            BIT_SET(route->flags, BGP_ROUTE_FLAG_STALE);
            route->updated_at_usec = g_get_real_time();
        }
        else
        {
            head->route_list = g_list_remove(head->route_list, route);
            route_node_free(route);
            ctx->removed_phys_routes++;
            if (!head->route_list)
            {
                g_ptr_array_add(ctx->empty_heads, head);
            }
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
    ctx.removed_phys_routes = 0;
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

    if (rib->route_count >= ctx.removed_phys_routes)
    {
        rib->route_count -= ctx.removed_phys_routes;
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

uint32_t bgp_rib_cleanup_stale(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    if (!rib || !nlri)
    {
        return 0;
    }

    bgp_rthead_t *head = (bgp_rthead_t *)g_tree_lookup(rib->head_tree, nlri);
    if (!head)
    {
        return 0;
    }

    return bgp_rib_gc_head(rib, head);
}

uint32_t bgp_rib_gc_head(bgp_rib_t *rib, bgp_rthead_t *head)
{
    if (!rib || !head)
    {
        return 0;
    }

    /* 仍有队列引用时不做物理删除，避免过早释放。 */
    if (head->queue_refcnt > 0)
    {
        return 0;
    }

    uint32_t cleaned = 0;
    GList *node = head->route_list;
    while (node)
    {
        GList *next = node->next;
        bgp_route_node_t *route = (bgp_route_node_t *)node->data;
        if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_STALE) && !BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED))
        {
            head->route_list = g_list_delete_link(head->route_list, node);
            route_node_free(route);
            cleaned++;
        }
        node = next;
    }

    if (cleaned > 0)
    {
        if (rib->route_count >= cleaned)
        {
            rib->route_count -= cleaned;
        }
        else
        {
            rib->route_count = 0;
        }
    }

    if (!head->route_list)
    {
        (void)remove_head_by_nlri(rib, &head->nlri);
    }

    return cleaned;
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
    const bgp_route_node_t *route = route_list_find(head->route_list, ctx->source);
    if (route && !BIT_TEST(route->flags, BGP_ROUTE_FLAG_IMPORT))
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

bgp_rthead_t *bgp_rib_ensure_head(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    if (!rib || !rib->head_tree || !nlri)
    {
        return NULL;
    }

    bgp_rthead_t *head = g_tree_lookup(rib->head_tree, nlri);
    if (head)
    {
        return head;
    }

    head = rthead_create(nlri, rib);
    if (!head)
    {
        return NULL;
    }

    g_tree_insert(rib->head_tree, &head->nlri, head);
    rib->head_count++;
    return head;
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
