/**
 * @file   bgp_rib.c
 * @brief  BGP 内存 RIB 通用结构实现（rthead-tree + per-head routes）
 * @author jhb
 * @date   2026/03/13
 */
#include "bgp_rib.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * 内部辅助
 * ========================================================================== */

static gint head_key_compare(gconstpointer a, gconstpointer b)
{
    return g_strcmp0((const char *)a, (const char *)b);
}

static void build_head_key(const bgp_nlri_entry_t *nlri, char *buf, size_t sz)
{
    const char *nk = (nlri && nlri->key[0] != '\0') ? nlri->key : "unknown";
    snprintf(buf, sz, "%u/%u/%s", nlri ? nlri->afi : 0, nlri ? nlri->safi : 0, nk);
}

static bgp_rthead_t *rthead_create(const bgp_nlri_entry_t *nlri, const char *head_key)
{
    bgp_rthead_t *head = g_malloc0(sizeof(bgp_rthead_t));
    snprintf(head->key, sizeof(head->key), "%s", head_key ? head_key : "");
    if (nlri)
    {
        memcpy(&head->nlri, nlri, sizeof(*nlri));
    }
    head->route_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
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

static gboolean destroy_tree_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)user_data;
    g_free(key);
    rthead_destroy((bgp_rthead_t *)value);
    return FALSE;
}

static gboolean lookup_head_extended(GTree *tree, const char *head_key, gpointer *orig_key, gpointer *orig_val)
{
    if (!tree || !head_key)
    {
        return FALSE;
    }
    return g_tree_lookup_extended(tree, head_key, orig_key, orig_val);
}

static gboolean remove_head_by_key(bgp_rib_t *rib, const char *head_key)
{
    gpointer orig_key = NULL;
    gpointer orig_val = NULL;
    if (!lookup_head_extended(rib->head_tree, head_key, &orig_key, &orig_val))
    {
        return FALSE;
    }

    g_tree_remove(rib->head_tree, head_key);
    g_free(orig_key);
    rthead_destroy((bgp_rthead_t *)orig_val);

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
    rib->head_tree = g_tree_new(head_key_compare);
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

int bgp_rib_reach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const char *source, const bgp_attr_t *attr,
                      const bgp_nexthop_t *nexthop)
{
    if (!rib || !nlri || !source || source[0] == '\0')
    {
        return -1;
    }

    char head_key[BGP_RIB_HEAD_KEY_MAX];
    build_head_key(nlri, head_key, sizeof(head_key));

    bgp_rthead_t *head = g_tree_lookup(rib->head_tree, head_key);
    if (!head)
    {
        head = rthead_create(nlri, head_key);
        g_tree_insert(rib->head_tree, g_strdup(head_key), head);
        rib->head_count++;
    }

    bgp_route_node_t *route = g_hash_table_lookup(head->route_hash, source);
    gboolean is_new = (route == NULL);
    if (is_new)
    {
        route = g_malloc0(sizeof(bgp_route_node_t));
        g_hash_table_insert(head->route_hash, g_strdup(source), route);
        rib->route_count++;
    }

    snprintf(route->source, sizeof(route->source), "%s", source);
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

int bgp_rib_unreach_one(bgp_rib_t *rib, const bgp_nlri_entry_t *nlri, const char *source)
{
    if (!rib || !nlri || !source || source[0] == '\0')
    {
        return -1;
    }

    char head_key[BGP_RIB_HEAD_KEY_MAX];
    build_head_key(nlri, head_key, sizeof(head_key));

    bgp_rthead_t *head = g_tree_lookup(rib->head_tree, head_key);
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
        remove_head_by_key(rib, head_key);
    }

    return 1;
}

/* ============================================================================
 * 批量应用与来源清理
 * ========================================================================== */

void bgp_rib_apply_update(bgp_rib_t *rib, const char *source, const bgp_update_result_t *upd,
                          bgp_rib_update_stats_t *stats)
{
    if (stats)
    {
        memset(stats, 0, sizeof(*stats));
    }
    if (!rib || !source || source[0] == '\0' || !upd)
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
    const char *source;
    uint32_t removed_routes;
    GPtrArray *empty_keys; /* gchar* */
} source_purge_ctx_t;

static gboolean purge_source_cb(gpointer key, gpointer value, gpointer user_data)
{
    source_purge_ctx_t *ctx = (source_purge_ctx_t *)user_data;
    bgp_rthead_t *head = (bgp_rthead_t *)value;

    if (g_hash_table_remove(head->route_hash, ctx->source))
    {
        ctx->removed_routes++;
        if (g_hash_table_size(head->route_hash) == 0)
        {
            g_ptr_array_add(ctx->empty_keys, g_strdup((const char *)key));
        }
    }
    return FALSE;
}

void bgp_rib_remove_source(bgp_rib_t *rib, const char *source, uint32_t *removed_routes, uint32_t *removed_heads)
{
    if (removed_routes)
    {
        *removed_routes = 0;
    }
    if (removed_heads)
    {
        *removed_heads = 0;
    }
    if (!rib || !source || source[0] == '\0')
    {
        return;
    }

    source_purge_ctx_t ctx;
    ctx.source = source;
    ctx.removed_routes = 0;
    ctx.empty_keys = g_ptr_array_new_with_free_func(g_free);

    g_tree_foreach(rib->head_tree, purge_source_cb, &ctx);

    for (guint i = 0; i < ctx.empty_keys->len; i++)
    {
        const char *head_key = g_ptr_array_index(ctx.empty_keys, i);
        if (remove_head_by_key(rib, head_key) && removed_heads)
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

    g_ptr_array_free(ctx.empty_keys, TRUE);
}

/* ============================================================================
 * 查询
 * ========================================================================== */

const bgp_rthead_t *bgp_rib_lookup_head(const bgp_rib_t *rib, const bgp_nlri_entry_t *nlri)
{
    if (!rib || !rib->head_tree || !nlri)
    {
        return NULL;
    }
    char head_key[BGP_RIB_HEAD_KEY_MAX];
    build_head_key(nlri, head_key, sizeof(head_key));
    return (const bgp_rthead_t *)g_tree_lookup(rib->head_tree, head_key);
}

const bgp_route_node_t *bgp_rthead_lookup_route(const bgp_rthead_t *head, const char *source)
{
    if (!head || !head->route_hash || !source || source[0] == '\0')
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
