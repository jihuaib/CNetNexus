/**
 * @file   bgp_rib.c
 * @brief  BGP 内存 RIB 通用结构实现（rthead-tree + per-head routes 双向链表）
 * @author jhb
 * @date   2026/03/13
 */
#include "bgp_rib.h"

#include <string.h>

#include "bgp_attr_intern.h"
#include "bgp_nexthop.h"
#include "bgp_rd.h"
#include "bgp_vrf_import.h"
#include "net_addr.h"
#include "route.h"

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
    /* inst 经 rd_entry 反向解引用得到（rd_entry 在 bgp_rib_create 之后由调用方装填） */
    head->inst = (rib && rib->rd_entry) ? rib->rd_entry->inst : NULL;
    head->rib = rib;         /* 反指针：reap 时无需逐层回查即可拿到所属 RIB */
    head->route_list = NULL; /* 路径列表，首元素为当前最优路径 */
    head->queue_refcnt = 0;
    return head;
}

static void route_node_release_attrs(bgp_route_node_t *route)
{
    if (!route)
    {
        return;
    }
    bgp_attr_release(route->attr);
    route->attr = NULL;
    bgp_attr_release(route->base_attr);
    route->base_attr = NULL;
    bgp_nexthop_reset_route(route);
    /* inter-AS Option B 中转换标：节点回收时释放其本地入标签 + 撤销 SWAP ILM */
    bgp_vrf_import_transit_release_label(route);
}

/**
 * @brief 逻辑删除：只打"待删"标记，不摘链、不释放
 *
 * 清除有效/最优/导入态并置 STALE，真正的物理回收交给 rib_reap_head。
 * 这样删除入口（unreach/purge/gc）全部收敛为"打标记"，物理删除只剩 reap 一处。
 */
static void rib_mark_deleted(bgp_route_node_t *route)
{
    if (!route)
    {
        return;
    }
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_BEST);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
    route->import_proto = 0;
    BIT_SET(route->flags, BGP_ROUTE_FLAG_STALE);
    route->updated_at_usec = g_get_real_time();
}

/**
 * @brief 物理回收唯一出口：reap 一个 head 下已逻辑删除的路径节点，必要时销毁 head
 *
 * 回收条件：节点 STALE（已逻辑删除）+ !FLUSHED（已完成对 ROUTE 的撤销下刷）+
 * borrow_refcnt==0（无外部借用）。三者满足才摘链 + 释放。
 *  - 被借用（borrow_refcnt>0）的 STALE 节点留在链表上（同时撑住 head 不被释放），
 *    待 bgp_route_node_borrow_unref 归零后再次 reap 时回收——这是修复跨 AF
 *    import-rib 借用 UAF 的关键：head 在仍有外部借用指向其子节点时绝不提前释放。
 *  - queue_refcnt>0（head 仍在 calc 队列中）时整体延后，不做任何物理删除。
 * 扫描后若 route_list 空且无队列引用，则从树中摘除并销毁 head 本身。
 *
 * @param out_head_destroyed 非空时回填 head 是否在本次被销毁（销毁后不得再访问 head）
 * @return 本次物理删除的节点数
 */
static uint32_t rib_reap_head(bgp_rib_t *rib, bgp_rthead_t *head, gboolean *out_head_destroyed)
{
    if (out_head_destroyed)
    {
        *out_head_destroyed = FALSE;
    }
    /* 仍有队列引用时不做物理删除，避免过早释放正在被 calc 处理的 head/node。 */
    if (!rib || !head || head->queue_refcnt > 0)
    {
        return 0;
    }

    uint32_t reaped = 0;
    GList *node = head->route_list;
    while (node)
    {
        GList *next = node->next;
        bgp_route_node_t *route = (bgp_route_node_t *)node->data;
        /* 已逻辑删除(STALE) + 已完成对 ROUTE 的撤销下刷(!FLUSHED) + 无外部借用，
         * 三者满足才物理回收；仍被借用的 STALE 节点留链保活（连带保活 head），
         * 待 bgp_route_node_borrow_unref 归零后再次 reap 时回收。 */
        if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_STALE) && !BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED) &&
            route->borrow_refcnt == 0)
        {
            head->route_list = g_list_delete_link(head->route_list, node);
            route_node_release_attrs(route);
            g_free(route);
            if (rib->route_count > 0)
            {
                rib->route_count--;
            }
            reaped++;
        }
        node = next;
    }

    if (!head->route_list && head->queue_refcnt == 0)
    {
        /* 键指向 head->nlri，移除后再销毁 head（含 nlri 内存） */
        g_tree_remove(rib->head_tree, &head->nlri);
        g_free(head);
        if (rib->head_count > 0)
        {
            rib->head_count--;
        }
        if (out_head_destroyed)
        {
            *out_head_destroyed = TRUE;
        }
    }

    return reaped;
}

/** 释放路径节点前先 release 共享属性 */
static void route_node_free(gpointer data)
{
    bgp_route_node_t *route = (bgp_route_node_t *)data;
    if (!route)
    {
        return;
    }
    /* 外部借用引用门控：若有 import_rib mirror 或 relay watch 仍持有借用指针，
     * 跳过实际释放（节点留给最后一次 bgp_route_node_borrow_unref 回收）。 */
    if (route->borrow_refcnt > 0)
    {
        return;
    }
    route_node_release_attrs(route);
    g_free(route);
}

void bgp_route_node_borrow_ref(bgp_route_node_t *route)
{
    if (!route)
    {
        return;
    }
    route->borrow_refcnt++;
}

void bgp_route_node_borrow_unref(bgp_route_node_t *route)
{
    if (!route || route->borrow_refcnt == 0)
    {
        return;
    }
    route->borrow_refcnt--;
    if (route->borrow_refcnt != 0)
    {
        return;
    }
    /* 借用归零：若节点之前被逻辑删除（标 STALE）而因借用留在链表上，此刻触发 reap
     * 完成物理回收（摘链 + 释放，并在 head 变空时连带销毁 head）。
     * 若节点未被标删（仍是有效借用路径刚释放），则什么都不做，节点继续存活。 */
    if (route->head && BIT_TEST(route->flags, BGP_ROUTE_FLAG_STALE))
    {
        (void)rib_reap_head(route->head->rib, route->head, NULL);
    }
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

int bgp_rib_route_apply_reach(bgp_route_node_t *route, uint32_t import_proto, const bgp_attr_t *attr)
{
    if (!route)
    {
        return -1;
    }

    /* 按 import_proto 置/清 IMPORT 标记；reach 默认置为 valid，路径变更后清除 BEST（等待 calc 重新评选）。
     * ROUTE_PROTOCOL_MAX 作为"非 import"的哨兵值（直接邻居路径调用方传入），其余值（含 CONNECTED=0）
     * 一律视为 import 路径来源协议。 */
    if (import_proto != ROUTE_PROTOCOL_MAX)
    {
        BIT_SET(route->flags, BGP_ROUTE_FLAG_IMPORT);
        route->import_proto = import_proto;
        bgp_attr_release(route->base_attr);
        route->base_attr = NULL;
    }
    else
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
        route->import_proto = 0;
    }
    BIT_SET(route->flags, BGP_ROUTE_FLAG_VALID);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_BEST);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_FLUSHED);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_STALE);
    route->has_label = 0u;
    route->label_source = BGP_ROUTE_LABEL_SOURCE_NONE;
    route->label = 0u;

    if (attr)
    {
        bgp_attr_ref_t *new_ref = bgp_attr_intern(route->head ? route->head->inst : NULL, attr);
        if (new_ref)
        {
            bgp_attr_release(route->attr);
            route->attr = new_ref;
        }
    }
    route->updated_at_usec = g_get_real_time();
    if (route->added_at_usec == 0)
    {
        route->added_at_usec = route->updated_at_usec;
    }

    return 0;
}

int bgp_rib_route_set_base_attr(bgp_route_node_t *route, const bgp_attr_t *base_attr)
{
    if (!route)
    {
        return -1;
    }

    bgp_attr_ref_t *new_ref = NULL;
    if (base_attr)
    {
        new_ref = bgp_attr_intern(route->head ? route->head->inst : NULL, base_attr);
        if (!new_ref)
        {
            return -1;
        }
    }

    bgp_attr_release(route->base_attr);
    route->base_attr = new_ref;
    return 0;
}

void bgp_route_set_label_from_nlri(bgp_route_node_t *route, const bgp_nlri_entry_t *nlri, uint8_t label_source)
{
    /* labeled-unicast 与 VPN（vpnv4/vpn-flowspec）的 NLRI 都携带 MPLS 标签。 */
    if (!route || !nlri || nlri->type != BGP_NLRI_PREFIX || !nlri->prefix.has_label ||
        (nlri->safi != BGP_SAFI_LABELED && nlri->safi != BGP_SAFI_VPN_UNICAST && nlri->safi != BGP_SAFI_VPN_FLOWSPEC))
    {
        return;
    }

    route->has_label = 1u;
    route->label = nlri->prefix.label;
    route->label_source = label_source;
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

    /* 逻辑删除（打标记）+ 尝试物理回收。FLUSHED 节点会被 reap 跳过、留待下刷撤销后清理；
     * 未下刷且无借用的节点当场被 reap 摘链释放。head 可能在 reap 中销毁，之后不得再访问。 */
    rib_mark_deleted(route);
    (void)rib_reap_head(rib, head, NULL);
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
    uint32_t marked_routes;   /* 逻辑删除（打标记）的节点数 */
    GPtrArray *touched_heads; /* bgp_rthead_t*，命中并打标记的 head，待遍历结束后统一 reap */
} source_purge_ctx_t;

static gboolean purge_source_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    source_purge_ctx_t *ctx = (source_purge_ctx_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;

    bgp_route_node_t *route = route_list_find(head->route_list, ctx->source);
    if (!route)
    {
        return FALSE;
    }

    /* 会话来源清理只处理 peer 路由；合成路由（重分发/跨表 import/export）source 相同也不能被清掉。 */
    if (bgp_route_is_synthetic(route))
    {
        return FALSE;
    }
    if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_STALE))
    {
        return FALSE;
    }

    /* 仅打标记，不在 g_tree_foreach 期间改动树结构；物理回收（含 head 销毁）
     * 留到遍历结束后对 touched_heads 统一 reap。 */
    rib_mark_deleted(route);
    ctx->marked_routes++;
    g_ptr_array_add(ctx->touched_heads, head);
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
    ctx.marked_routes = 0;
    ctx.touched_heads = g_ptr_array_new();

    g_tree_foreach(rib->head_tree, purge_source_cb, &ctx);

    uint32_t heads_destroyed = 0;
    for (guint i = 0; i < ctx.touched_heads->len; i++)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)g_ptr_array_index(ctx.touched_heads, i);
        gboolean destroyed = FALSE;
        (void)rib_reap_head(rib, head, &destroyed);
        if (destroyed)
        {
            heads_destroyed++;
        }
    }

    if (removed_routes)
    {
        *removed_routes = ctx.marked_routes;
    }
    if (removed_heads)
    {
        *removed_heads = heads_destroyed;
    }

    g_ptr_array_free(ctx.touched_heads, TRUE);
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
    /* GC 即物理回收：收敛到唯一出口 rib_reap_head（含 head 空时销毁）。 */
    return rib_reap_head(rib, head, NULL);
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
    if (route && !bgp_route_is_synthetic(route))
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

typedef struct rib_walk_heads_ctx
{
    const bgp_nlri_entry_t *last_nlri;
    gboolean has_last;
    uint32_t budget;
    uint32_t processed;
    bgp_rib_head_walk_cb cb;
    gpointer user_data;
    bgp_nlri_entry_t *out_last;
    gboolean stopped;
} rib_walk_heads_ctx_t;

static gboolean walk_heads_from_tree_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    rib_walk_heads_ctx_t *ctx = (rib_walk_heads_ctx_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;
    if (!ctx || !head)
    {
        return FALSE;
    }

    if (ctx->has_last && bgp_nlri_cmp(&head->nlri, ctx->last_nlri) <= 0)
    {
        return FALSE;
    }
    if (ctx->processed >= ctx->budget)
    {
        ctx->stopped = TRUE;
        return TRUE;
    }
    if (ctx->cb && !ctx->cb(head, ctx->user_data))
    {
        ctx->stopped = TRUE;
        return TRUE;
    }

    if (ctx->out_last)
    {
        memcpy(ctx->out_last, &head->nlri, sizeof(*ctx->out_last));
    }
    ctx->processed++;
    return FALSE;
}

gboolean bgp_rib_walk_heads_from(bgp_rib_t *rib, const bgp_nlri_entry_t *last_nlri, gboolean has_last, uint32_t budget,
                                 bgp_rib_head_walk_cb cb, gpointer user_data, bgp_nlri_entry_t *out_last,
                                 gboolean *out_has_last, uint32_t *out_processed)
{
    if (out_has_last)
    {
        *out_has_last = FALSE;
    }
    if (out_processed)
    {
        *out_processed = 0;
    }
    if (!rib || !rib->head_tree || !cb || budget == 0)
    {
        return TRUE;
    }

    rib_walk_heads_ctx_t ctx = {
        .last_nlri = last_nlri,
        .has_last = has_last,
        .budget = budget,
        .processed = 0,
        .cb = cb,
        .user_data = user_data,
        .out_last = out_last,
        .stopped = FALSE,
    };

    g_tree_foreach(rib->head_tree, walk_heads_from_tree_cb, &ctx);
    if (out_has_last)
    {
        *out_has_last = (ctx.processed > 0);
    }
    if (out_processed)
    {
        *out_processed = ctx.processed;
    }
    return !ctx.stopped;
}
