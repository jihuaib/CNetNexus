/**
 * @file   route_rib.c
 * @brief  Route 模块内存 RIB 实现
 * @author jhb
 * @date   2026/02/01
 */
#include "route_rib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "route.h"

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 构造前缀头的 GTree 键："vrf_id/afi/prefix/prefix_len"
 */
static void make_head_key(char *out, size_t out_size, uint32_t vrf_id, uint16_t afi, const char *prefix,
                          uint8_t prefix_len)
{
    snprintf(out, out_size, "%u/%u/%s/%u", vrf_id, afi, prefix, (unsigned)prefix_len);
}

/**
 * @brief 构造路径哈希键："protocol:source"
 */
static void make_path_key(char *out, size_t out_size, uint32_t protocol, const char *source)
{
    snprintf(out, out_size, "%u:%s", protocol, source);
}

/**
 * @brief 释放 route_path_t（供 GHashTable destroy_func 使用）
 */
static void path_free(gpointer data)
{
    g_free(data);
}

/**
 * @brief GTree 键比较函数（包装 g_strcmp0 以匹配 GCompareDataFunc 签名）
 */
static gint head_key_cmp(gconstpointer a, gconstpointer b, gpointer userdata)
{
    (void)userdata;
    return g_strcmp0((const char *)a, (const char *)b);
}

/**
 * @brief 释放 route_head_t（供 GTree value_destroy_func 使用）
 */
static void head_free(gpointer data)
{
    route_head_t *head = (route_head_t *)data;
    if (head->path_hash)
    {
        g_hash_table_destroy(head->path_hash);
    }
    g_free(head);
}

/**
 * @brief 查找或创建 route_head_t
 */
static route_head_t *get_or_create_head(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const char *prefix,
                                        uint8_t prefix_len)
{
    char key[ROUTE_RIB_HEAD_KEY_MAX];
    make_head_key(key, sizeof(key), vrf_id, afi, prefix, prefix_len);

    route_head_t *head = (route_head_t *)g_tree_lookup(rib->head_tree, key);
    if (head)
    {
        return head;
    }

    head = (route_head_t *)g_malloc0(sizeof(route_head_t));
    if (!head)
    {
        return NULL;
    }

    g_strlcpy(head->key, key, sizeof(head->key));
    head->vrf_id = vrf_id;
    head->afi = afi;
    head->prefix_len = prefix_len;
    g_strlcpy(head->prefix, prefix, sizeof(head->prefix));
    head->path_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, path_free);

    g_tree_insert(rib->head_tree, g_strdup(head->key), head);
    rib->head_count++;

    return head;
}

// ============================================================================
// RIB 生命周期
// ============================================================================

route_rib_t *route_rib_create(void)
{
    route_rib_t *rib = (route_rib_t *)g_malloc0(sizeof(route_rib_t));
    if (!rib)
    {
        return NULL;
    }

    rib->head_tree = g_tree_new_full(head_key_cmp, NULL, g_free, head_free);
    return rib;
}

void route_rib_destroy(route_rib_t *rib)
{
    if (!rib)
    {
        return;
    }
    g_tree_destroy(rib->head_tree);
    g_free(rib);
}

// ============================================================================
// 写操作
// ============================================================================

int route_rib_add(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const char *prefix, uint8_t prefix_len,
                  uint32_t protocol, const char *source, const char *nexthop, int32_t metric, int32_t preference)
{
    if (!rib || !prefix || !source || !nexthop)
    {
        return -1;
    }

    route_head_t *head = get_or_create_head(rib, vrf_id, afi, prefix, prefix_len);
    if (!head)
    {
        return -1;
    }

    char path_key[ROUTE_RIB_HEAD_KEY_MAX];
    make_path_key(path_key, sizeof(path_key), protocol, source);

    route_path_t *path = (route_path_t *)g_hash_table_lookup(head->path_hash, path_key);
    int is_new = (path == NULL);

    if (is_new)
    {
        path = (route_path_t *)g_malloc0(sizeof(route_path_t));
        if (!path)
        {
            return -1;
        }
        g_hash_table_insert(head->path_hash, g_strdup(path_key), path);
        rib->path_count++;
    }

    path->protocol = protocol;
    g_strlcpy(path->source, source, sizeof(path->source));
    g_strlcpy(path->nexthop, nexthop, sizeof(path->nexthop));
    path->metric = metric;
    path->preference = preference;
    path->updated_at_usec = g_get_real_time();

    return is_new ? 1 : 0;
}

int route_rib_del(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const char *prefix, uint8_t prefix_len,
                  uint32_t protocol, const char *source, route_path_cb cb, void *userdata)
{
    if (!rib || !prefix || !source)
    {
        return -1;
    }

    char head_key[ROUTE_RIB_HEAD_KEY_MAX];
    make_head_key(head_key, sizeof(head_key), vrf_id, afi, prefix, prefix_len);

    route_head_t *head = (route_head_t *)g_tree_lookup(rib->head_tree, head_key);
    if (!head)
    {
        return 0;
    }

    char path_key[ROUTE_RIB_HEAD_KEY_MAX];
    make_path_key(path_key, sizeof(path_key), protocol, source);

    route_path_t *path = (route_path_t *)g_hash_table_lookup(head->path_hash, path_key);
    if (!path)
    {
        return 0;
    }

    /* 删除前触发回调 */
    if (cb)
    {
        cb(head, path, userdata);
    }

    g_hash_table_remove(head->path_hash, path_key);
    rib->path_count--;

    /* 前缀头下没有路径时，从树中移除 */
    if (g_hash_table_size(head->path_hash) == 0)
    {
        g_tree_remove(rib->head_tree, head_key);
        rib->head_count--;
    }

    return 1;
}

int route_rib_del_proto_for_prefix(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const char *prefix,
                                   uint8_t prefix_len, uint32_t protocol, route_path_cb cb, void *userdata)
{
    if (!rib || !prefix)
    {
        return -1;
    }

    char head_key[ROUTE_RIB_HEAD_KEY_MAX];
    make_head_key(head_key, sizeof(head_key), vrf_id, afi, prefix, prefix_len);

    route_head_t *head = (route_head_t *)g_tree_lookup(rib->head_tree, head_key);
    if (!head)
    {
        return 0;
    }

    /* 先收集要删除的 path_key 列表（避免边遍历边删除） */
    GList *keys_to_del = NULL;
    GHashTableIter iter;
    gpointer key_ptr, val_ptr;
    g_hash_table_iter_init(&iter, head->path_hash);
    while (g_hash_table_iter_next(&iter, &key_ptr, &val_ptr))
    {
        route_path_t *path = (route_path_t *)val_ptr;
        if (path->protocol == protocol)
        {
            keys_to_del = g_list_prepend(keys_to_del, g_strdup((const char *)key_ptr));
        }
    }

    int count = 0;
    for (GList *l = keys_to_del; l; l = l->next)
    {
        const char *pk = (const char *)l->data;
        route_path_t *path = (route_path_t *)g_hash_table_lookup(head->path_hash, pk);
        if (path)
        {
            if (cb)
            {
                cb(head, path, userdata);
            }
            g_hash_table_remove(head->path_hash, pk);
            rib->path_count--;
            count++;
        }
    }
    g_list_free_full(keys_to_del, g_free);

    if (g_hash_table_size(head->path_hash) == 0)
    {
        g_tree_remove(rib->head_tree, head_key);
        rib->head_count--;
    }

    return count;
}

// ============================================================================
// 查询
// ============================================================================

const route_head_t *route_rib_lookup_head(const route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const char *prefix,
                                          uint8_t prefix_len)
{
    if (!rib || !prefix)
    {
        return NULL;
    }

    char key[ROUTE_RIB_HEAD_KEY_MAX];
    make_head_key(key, sizeof(key), vrf_id, afi, prefix, prefix_len);
    return (const route_head_t *)g_tree_lookup(rib->head_tree, key);
}

const route_path_t *route_rib_lookup_path(const route_head_t *head, uint32_t protocol, const char *source)
{
    if (!head || !source)
    {
        return NULL;
    }

    char path_key[ROUTE_RIB_HEAD_KEY_MAX];
    make_path_key(path_key, sizeof(path_key), protocol, source);
    return (const route_path_t *)g_hash_table_lookup(head->path_hash, path_key);
}

// ============================================================================
// 遍历辅助
// ============================================================================

typedef struct
{
    uint32_t proto_filter;
    uint32_t vrf_filter;
    route_path_cb cb;
    void *userdata;
} walk_ctx_t;

static gboolean walk_head(gpointer key, gpointer value, gpointer data)
{
    route_head_t *head = (route_head_t *)value;
    walk_ctx_t *ctx = (walk_ctx_t *)data;

    /* VRF 过滤 */
    if (ctx->vrf_filter != ROUTE_VRF_ALL && head->vrf_id != ctx->vrf_filter)
    {
        return FALSE;
    }

    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, head->path_hash);
    while (g_hash_table_iter_next(&iter, &k, &v))
    {
        route_path_t *path = (route_path_t *)v;
        /* 协议过滤 */
        if (ctx->proto_filter != ROUTE_PROTOCOL_MAX && path->protocol != ctx->proto_filter)
        {
            continue;
        }
        ctx->cb(head, path, ctx->userdata);
    }

    (void)key;
    return FALSE; /* 继续遍历 */
}

void route_rib_walk(route_rib_t *rib, uint32_t proto_filter, uint32_t vrf_filter, route_path_cb cb, void *userdata)
{
    if (!rib || !cb)
    {
        return;
    }

    walk_ctx_t ctx = {
        .proto_filter = proto_filter,
        .vrf_filter = vrf_filter,
        .cb = cb,
        .userdata = userdata,
    };
    g_tree_foreach(rib->head_tree, walk_head, &ctx);
}

uint32_t route_rib_head_count(const route_rib_t *rib)
{
    return rib ? rib->head_count : 0;
}

uint32_t route_rib_path_count(const route_rib_t *rib)
{
    return rib ? rib->path_count : 0;
}
