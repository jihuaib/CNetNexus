/**
 * @file   isis_route.c
 * @brief  ISIS SPF 路由多路径存储（head + path）与最优路径同步
 * @author jhb
 * @date   2026/04/14
 */
#include "isis_route.h"

#include <string.h>

#include "errcode.h"
#include "isis_nexthop.h"
#include "isis_route_sync.h"

static void isis_route_state_free(gpointer data)
{
    isis_route_state_t *state = (isis_route_state_t *)data;
    if (state)
    {
        isis_route_state_reset(state);
    }
    g_free(state);
}

static void isis_route_path_free(isis_route_path_t *path)
{
    if (!path)
    {
        return;
    }
    g_free(path->path_key);
    path->path_key = NULL;
    isis_route_state_reset(&path->state);
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

void isis_route_state_reset(isis_route_state_t *state)
{
    if (!state)
    {
        return;
    }
    if (state->nexthop_id != 0u)
    {
        isis_nexthop_release(state->nexthop_table, state->nexthop_id);
    }
    state->nexthop_id = 0u;
    state->nexthop_table = NULL;
}

int isis_route_state_copy(isis_route_state_t *dst, const isis_route_state_t *src)
{
    if (!dst || !src)
    {
        return ERRCODE_FAIL;
    }

    isis_route_state_t copy = *src;
    if (copy.nexthop_id != 0u && isis_nexthop_retain(copy.nexthop_table, copy.nexthop_id) != ERRCODE_SUCCESS)
    {
        copy.nexthop_id = 0u;
        copy.nexthop_table = NULL;
        return ERRCODE_FAIL;
    }

    *dst = copy;
    return ERRCODE_SUCCESS;
}

static int isis_nexthop_value_same(const isis_nexthop_value_t *a, const isis_nexthop_value_t *b)
{
    if (!a || !b)
    {
        return 0;
    }
    return a->out_ifindex == b->out_ifindex && net_addr_equal(&a->relay_addr, &b->relay_addr);
}

int isis_route_state_set_nexthop(isis_route_state_t *state, isis_nexthop_table_t *table, uint32_t key_ifindex,
                                 uint32_t out_ifindex, const net_addr_t *nexthop)
{
    if (!state || !table)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_key_t key;
    isis_nexthop_make_route_key(state->afi, key_ifindex, nexthop, &key);

    isis_nexthop_value_t value;
    memset(&value, 0, sizeof(value));
    value.relay_addr = key.nexthop;
    value.out_ifindex = out_ifindex;

    route_nhobj_key_t cur_key;
    isis_nexthop_value_t cur_value;
    if (state->nexthop_id != 0u && state->nexthop_table == table &&
        isis_nexthop_lookup(table, state->nexthop_id, &cur_key) == ERRCODE_SUCCESS &&
        isis_nexthop_get_value(table, state->nexthop_id, &cur_value) == ERRCODE_SUCCESS &&
        isis_nexthop_key_equal(&cur_key, &key) && isis_nexthop_value_same(&cur_value, &value))
    {
        return ERRCODE_SUCCESS;
    }

    uint32_t new_id = 0u;
    if (isis_nexthop_acquire(table, &key, &value, &new_id) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    isis_route_state_reset(state);
    state->nexthop_id = new_id;
    state->nexthop_table = table;
    return ERRCODE_SUCCESS;
}

int isis_route_state_get_nexthop(const isis_route_state_t *state, net_addr_t *nexthop_out)
{
    if (!state || !nexthop_out)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_key_t key;
    if (isis_nexthop_lookup(state->nexthop_table, state->nexthop_id, &key) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    *nexthop_out = key.nexthop;
    return ERRCODE_SUCCESS;
}

int isis_route_state_get_out_ifindex(const isis_route_state_t *state, uint32_t *out_ifindex)
{
    if (!state || !out_ifindex)
    {
        return ERRCODE_FAIL;
    }

    isis_nexthop_value_t value;
    if (isis_nexthop_get_value(state->nexthop_table, state->nexthop_id, &value) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    *out_ifindex = value.out_ifindex;
    return ERRCODE_SUCCESS;
}

int isis_route_state_same(const isis_route_state_t *a, const isis_route_state_t *b)
{
    if (!a || !b)
    {
        return 0;
    }

    if (a->afi != b->afi || a->prefix_len != b->prefix_len || a->metric != b->metric ||
        !net_addr_equal(&a->prefix_addr, &b->prefix_addr) || !net_addr_equal(&a->source_addr, &b->source_addr))
    {
        return 0;
    }

    route_nhobj_key_t a_key;
    route_nhobj_key_t b_key;
    isis_nexthop_value_t a_value;
    isis_nexthop_value_t b_value;
    if (a->nexthop_table != b->nexthop_table ||
        isis_nexthop_lookup(a->nexthop_table, a->nexthop_id, &a_key) != ERRCODE_SUCCESS ||
        isis_nexthop_lookup(b->nexthop_table, b->nexthop_id, &b_key) != ERRCODE_SUCCESS ||
        isis_nexthop_get_value(a->nexthop_table, a->nexthop_id, &a_value) != ERRCODE_SUCCESS ||
        isis_nexthop_get_value(b->nexthop_table, b->nexthop_id, &b_value) != ERRCODE_SUCCESS)
    {
        return 0;
    }
    return isis_nexthop_key_equal(&a_key, &b_key) && isis_nexthop_value_same(&a_value, &b_value);
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
    uint32_t pa_oif = 0u;
    uint32_t pb_oif = 0u;
    (void)isis_route_state_get_out_ifindex(&pa->state, &pa_oif);
    (void)isis_route_state_get_out_ifindex(&pb->state, &pb_oif);
    if (pa_oif != pb_oif)
    {
        return (pa_oif < pb_oif) ? -1 : 1;
    }

    char pa_nh[64] = {0};
    char pb_nh[64] = {0};
    net_addr_t pa_nh_addr;
    net_addr_t pb_nh_addr;
    isis_route_addr_key_str((isis_route_state_get_nexthop(&pa->state, &pa_nh_addr) == ERRCODE_SUCCESS) ? &pa_nh_addr
                                                                                                       : NULL,
                            pa_nh, sizeof(pa_nh));
    isis_route_addr_key_str((isis_route_state_get_nexthop(&pb->state, &pb_nh_addr) == ERRCODE_SUCCESS) ? &pb_nh_addr
                                                                                                       : NULL,
                            pb_nh, sizeof(pb_nh));
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
    net_addr_t nexthop;
    uint32_t out_ifindex = 0u;
    isis_route_addr_key_str(
        (state && isis_route_state_get_nexthop(state, &nexthop) == ERRCODE_SUCCESS) ? &nexthop : NULL, nh, sizeof(nh));
    if (state)
    {
        (void)isis_route_state_get_out_ifindex(state, &out_ifindex);
    }
    isis_route_addr_key_str(state ? &state->source_addr : NULL, src, sizeof(src));
    g_snprintf(buf, sz, "%s|oif=%u|nh=%s|src=%s", route_key ? route_key : "", out_ifindex, nh, src);
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
        isis_route_state_t next_state;
        if (isis_route_state_copy(&next_state, state) != ERRCODE_SUCCESS)
        {
            return;
        }
        isis_route_state_reset(&path->state);
        path->state = next_state;
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
    if (isis_route_state_copy(&path->state, state) != ERRCODE_SUCCESS)
    {
        isis_route_path_free(path);
        return;
    }

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
        if (isis_route_state_copy(&dst_path->state, &src_path->state) != ERRCODE_SUCCESS)
        {
            isis_route_path_free(dst_path);
            continue;
        }

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
        if (isis_route_state_copy(copy, &best_path->state) != ERRCODE_SUCCESS)
        {
            g_free(copy);
            continue;
        }
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
