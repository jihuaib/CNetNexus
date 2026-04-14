/**
 * @file   isis_route.c
 * @brief  ISIS SPF 路由多路径存储（head + path）与最优路径同步
 * @author jhb
 * @date   2026/04/14
 */
#include "isis_route.h"

#include <string.h>

#include "errcode.h"
#include "isis_route_sync.h"

static void isis_route_state_free(gpointer data)
{
    g_free(data);
}

static void isis_route_path_free(isis_route_path_t *path)
{
    if (!path)
    {
        return;
    }
    g_free(path->path_key);
    path->path_key = NULL;
    path->head = NULL;
    g_free(path);
}

static void isis_route_addr_key_str(const net_addr_t *addr, char *buf, size_t sz)
{
    if (!buf || sz == 0u)
    {
        return;
    }

    g_strlcpy(buf, "-", sz);
    if (!addr)
    {
        return;
    }

    if ((addr->family == AF_INET || addr->family == AF_INET6) && !net_addr_is_zero(addr))
    {
        net_addr_to_str(addr, buf, sz);
    }
}

int isis_route_state_same(const isis_route_state_t *a, const isis_route_state_t *b)
{
    if (!a || !b)
    {
        return 0;
    }

    return (a->afi == b->afi && a->prefix_len == b->prefix_len && a->out_ifindex == b->out_ifindex &&
            a->metric == b->metric && net_addr_equal(&a->prefix_addr, &b->prefix_addr) &&
            net_addr_equal(&a->source_addr, &b->source_addr) && net_addr_equal(&a->nexthop_addr, &b->nexthop_addr))
               ? 1
               : 0;
}

static gint isis_route_path_cmp(gconstpointer a, gconstpointer b)
{
    const isis_route_path_t *pa = (const isis_route_path_t *)a;
    const isis_route_path_t *pb = (const isis_route_path_t *)b;
    if (!pa || !pb)
    {
        return 0;
    }

    if (pa->state.metric != pb->state.metric)
    {
        return (pa->state.metric < pb->state.metric) ? -1 : 1;
    }
    if (pa->state.out_ifindex != pb->state.out_ifindex)
    {
        return (pa->state.out_ifindex < pb->state.out_ifindex) ? -1 : 1;
    }

    char pa_nh[64] = {0};
    char pb_nh[64] = {0};
    isis_route_addr_key_str(&pa->state.nexthop_addr, pa_nh, sizeof(pa_nh));
    isis_route_addr_key_str(&pb->state.nexthop_addr, pb_nh, sizeof(pb_nh));
    int nh_cmp = strcmp(pa_nh, pb_nh);
    if (nh_cmp != 0)
    {
        return nh_cmp;
    }

    char pa_src[64] = {0};
    char pb_src[64] = {0};
    isis_route_addr_key_str(&pa->state.source_addr, pa_src, sizeof(pa_src));
    isis_route_addr_key_str(&pb->state.source_addr, pb_src, sizeof(pb_src));
    int src_cmp = strcmp(pa_src, pb_src);
    if (src_cmp != 0)
    {
        return src_cmp;
    }

    return strcmp(pa->path_key ? pa->path_key : "", pb->path_key ? pb->path_key : "");
}

static void isis_route_head_resort(isis_route_head_t *head)
{
    if (!head)
    {
        return;
    }
    head->path_list = g_list_sort(head->path_list, isis_route_path_cmp);
}

void isis_route_head_free(gpointer data)
{
    isis_route_head_t *head = (isis_route_head_t *)data;
    if (!head)
    {
        return;
    }

    GList *cur = head->path_list;
    while (cur)
    {
        GList *next = cur->next;
        isis_route_path_free((isis_route_path_t *)cur->data);
        cur->data = NULL;
        g_list_free_1(cur);
        cur = next;
    }
    head->path_list = NULL;
    head->path_count = 0u;

    g_free(head->route_key);
    head->route_key = NULL;
    g_free(head);
}

GHashTable *isis_route_head_table_new(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_route_head_free);
}

void isis_route_path_key_format(char *buf, size_t sz, const char *route_key, const isis_route_state_t *state)
{
    if (!buf || sz == 0u)
    {
        return;
    }

    char nh[64] = {0};
    char src[64] = {0};
    isis_route_addr_key_str(state ? &state->nexthop_addr : NULL, nh, sizeof(nh));
    isis_route_addr_key_str(state ? &state->source_addr : NULL, src, sizeof(src));
    g_snprintf(buf, sz, "%s|oif=%u|nh=%s|src=%s", route_key ? route_key : "", state ? state->out_ifindex : 0u, nh, src);
}

static isis_route_head_t *isis_route_head_create(const char *route_key)
{
    if (!route_key || route_key[0] == '\0')
    {
        return NULL;
    }

    isis_route_head_t *head = g_malloc0(sizeof(*head));
    if (!head)
    {
        return NULL;
    }

    head->route_key = g_strdup(route_key);
    if (!head->route_key)
    {
        g_free(head);
        return NULL;
    }
    return head;
}

static isis_route_path_t *isis_route_head_lookup_path_mut(isis_route_head_t *head, const char *path_key)
{
    if (!head || !path_key || path_key[0] == '\0')
    {
        return NULL;
    }

    for (GList *cur = head->path_list; cur; cur = cur->next)
    {
        isis_route_path_t *path = (isis_route_path_t *)cur->data;
        if (path && path->path_key && strcmp(path->path_key, path_key) == 0)
        {
            return path;
        }
    }
    return NULL;
}

void isis_route_head_table_add_path(GHashTable *head_table, const char *route_key, const char *path_key,
                                    const isis_route_state_t *state)
{
    if (!head_table || !route_key || route_key[0] == '\0' || !path_key || path_key[0] == '\0' || !state)
    {
        return;
    }

    isis_route_head_t *head = (isis_route_head_t *)g_hash_table_lookup(head_table, route_key);
    if (!head)
    {
        head = isis_route_head_create(route_key);
        if (!head)
        {
            return;
        }
        g_hash_table_replace(head_table, g_strdup(route_key), head);
    }

    isis_route_path_t *path = isis_route_head_lookup_path_mut(head, path_key);
    if (path)
    {
        path->state = *state;
        isis_route_head_resort(head);
        return;
    }

    path = g_malloc0(sizeof(*path));
    if (!path)
    {
        return;
    }

    path->head = head;
    path->path_key = g_strdup(path_key);
    if (!path->path_key)
    {
        g_free(path);
        return;
    }
    path->state = *state;

    head->path_list = g_list_append(head->path_list, path);
    head->path_count++;
    isis_route_head_resort(head);
}

const isis_route_path_t *isis_route_head_best_path(const isis_route_head_t *head)
{
    if (!head || !head->path_list)
    {
        return NULL;
    }
    return (const isis_route_path_t *)head->path_list->data;
}

static isis_route_head_t *isis_route_head_clone(const isis_route_head_t *src)
{
    if (!src || !src->route_key)
    {
        return NULL;
    }

    isis_route_head_t *dst = isis_route_head_create(src->route_key);
    if (!dst)
    {
        return NULL;
    }

    for (GList *cur = src->path_list; cur; cur = cur->next)
    {
        const isis_route_path_t *src_path = (const isis_route_path_t *)cur->data;
        if (!src_path || !src_path->path_key)
        {
            continue;
        }

        isis_route_path_t *dst_path = g_malloc0(sizeof(*dst_path));
        if (!dst_path)
        {
            continue;
        }

        dst_path->head = dst;
        dst_path->path_key = g_strdup(src_path->path_key);
        if (!dst_path->path_key)
        {
            g_free(dst_path);
            continue;
        }
        dst_path->state = src_path->state;

        dst->path_list = g_list_append(dst->path_list, dst_path);
        dst->path_count++;
    }

    isis_route_head_resort(dst);
    return dst;
}

static GHashTable *isis_route_build_best_table(const GHashTable *head_table)
{
    GHashTable *best = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, isis_route_state_free);
    if (!best)
    {
        return NULL;
    }

    if (!head_table)
    {
        return best;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, (GHashTable *)head_table);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        const char *route_key = (const char *)key;
        const isis_route_head_t *head = (const isis_route_head_t *)value;
        const isis_route_path_t *best_path = isis_route_head_best_path(head);
        if (!route_key || !best_path)
        {
            continue;
        }

        isis_route_state_t *copy = g_malloc0(sizeof(*copy));
        if (!copy)
        {
            continue;
        }
        *copy = best_path->state;
        g_hash_table_replace(best, g_strdup(route_key), copy);
    }

    return best;
}

static const isis_route_state_t *isis_route_head_best_state(const isis_route_head_t *head)
{
    const isis_route_path_t *best_path = isis_route_head_best_path(head);
    if (!best_path)
    {
        return NULL;
    }
    return &best_path->state;
}

int isis_route_reconcile_spf(isis_instance_cfg_t *inst, const GHashTable *desired_heads)
{
    if (!inst || !inst->learned_route_heads)
    {
        return ERRCODE_FAIL;
    }

    GHashTable *desired_best = isis_route_build_best_table(desired_heads);
    if (!desired_best)
    {
        return ERRCODE_FAIL;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;

    g_hash_table_iter_init(&iter, inst->learned_route_heads);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        const char *route_key = (const char *)key;
        const isis_route_head_t *cur_head = (const isis_route_head_t *)value;
        const isis_route_state_t *cur = isis_route_head_best_state(cur_head);
        const isis_route_state_t *want =
            desired_best ? (const isis_route_state_t *)g_hash_table_lookup(desired_best, route_key) : NULL;
        if (!route_key || !cur || !g_str_has_prefix(route_key, "lsp|"))
        {
            continue;
        }
        if (!want || !isis_route_state_same(cur, want))
        {
            (void)isis_route_sync_publish_del(cur);
            g_hash_table_iter_remove(&iter);
        }
    }

    g_hash_table_iter_init(&iter, desired_best);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        const char *route_key = (const char *)key;
        const isis_route_state_t *want = (const isis_route_state_t *)value;
        if (!route_key || !want || !g_str_has_prefix(route_key, "lsp|"))
        {
            continue;
        }

        const isis_route_head_t *cur_head =
            (const isis_route_head_t *)g_hash_table_lookup(inst->learned_route_heads, route_key);
        const isis_route_state_t *cur = isis_route_head_best_state(cur_head);
        if (cur && isis_route_state_same(cur, want))
        {
            continue;
        }

        if (isis_route_sync_publish_add(want) != ERRCODE_SUCCESS)
        {
            continue;
        }
    }

    g_hash_table_iter_init(&iter, inst->learned_route_heads);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        const char *route_key = (const char *)key;
        if (!route_key || !g_str_has_prefix(route_key, "lsp|"))
        {
            continue;
        }
        g_hash_table_iter_remove(&iter);
    }

    if (desired_heads)
    {
        g_hash_table_iter_init(&iter, (GHashTable *)desired_heads);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            const char *route_key = (const char *)key;
            const isis_route_head_t *want_head = (const isis_route_head_t *)value;
            if (!route_key || !want_head || !g_str_has_prefix(route_key, "lsp|"))
            {
                continue;
            }

            isis_route_head_t *copy_head = isis_route_head_clone(want_head);
            if (!copy_head)
            {
                continue;
            }
            g_hash_table_replace(inst->learned_route_heads, g_strdup(route_key), copy_head);
        }
    }

    g_hash_table_destroy(desired_best);
    return ERRCODE_SUCCESS;
}
