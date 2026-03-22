/**
 * @file   route_rib.c
 * @brief  Route 模块内存 RIB 实现（二进制键，无字符串拼接）
 * @author jhb
 * @date   2026/02/01
 */
#include "route_rib.h"

#include <string.h>

#include "log.h"
#include "route.h"

// ============================================================================
// 内部辅助：键构造
// ============================================================================

/**
 * @brief 填充前缀头键（调用前必须 memset 为 0 以清除填充字节）
 */
static void make_head_key(route_head_key_t *k, uint32_t vrf_id, uint16_t afi, const net_addr_t *addr,
                          uint8_t prefix_len)
{
    memset(k, 0, sizeof(*k));
    k->vrf_id = vrf_id;
    k->afi = afi;
    k->prefix_len = prefix_len;
    k->addr = *addr;
}

/**
 * @brief 填充路径键（调用前必须 memset 为 0 以清除填充字节）
 */
static void make_path_key(route_path_key_t *k, uint32_t protocol, const net_addr_t *source)
{
    memset(k, 0, sizeof(*k));
    k->protocol = protocol;
    k->source = *source;
}

// ============================================================================
// GTree 比较函数（二进制 memcmp）
// ============================================================================

/**
 * @brief 前缀头键二进制比较（GTree GCompareDataFunc）
 */
static gint head_key_cmp(gconstpointer a, gconstpointer b, gpointer userdata)
{
    (void)userdata;
    return memcmp(a, b, sizeof(route_head_key_t));
}

// ============================================================================
// GHashTable 哈希与相等函数
// ============================================================================

/**
 * @brief 路径键哈希（protocol XOR source 地址哈希）
 */
static guint path_key_hash(gconstpointer p)
{
    const route_path_key_t *k = (const route_path_key_t *)p;
    return (guint)k->protocol ^ net_addr_hash(&k->source);
}

/**
 * @brief 路径键相等（protocol 相等且 source 地址相等）
 */
static gboolean path_key_equal(gconstpointer a, gconstpointer b)
{
    const route_path_key_t *ka = (const route_path_key_t *)a;
    const route_path_key_t *kb = (const route_path_key_t *)b;
    return ka->protocol == kb->protocol && net_addr_equal(&ka->source, &kb->source);
}

// ============================================================================
// 内存释放
// ============================================================================

/**
 * @brief 释放 route_path_t（供 GHashTable value_destroy_func 使用）
 */
static void path_free(gpointer data)
{
    g_free(data);
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

// ============================================================================
// 内部：查找或创建前缀头
// ============================================================================

static route_head_t *get_or_create_head(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr,
                                        uint8_t prefix_len)
{
    route_head_key_t tmp_key;
    make_head_key(&tmp_key, vrf_id, afi, prefix_addr, prefix_len);

    route_head_t *head = (route_head_t *)g_tree_lookup(rib->head_tree, &tmp_key);
    if (head)
    {
        return head;
    }

    head = (route_head_t *)g_malloc0(sizeof(route_head_t));
    if (!head)
    {
        return NULL;
    }

    head->key = tmp_key;
    head->path_hash = g_hash_table_new_full(path_key_hash, path_key_equal, NULL, path_free);

    /* key 内嵌于 value，GTree 直接保存 &head->key 指针，无需单独分配 */
    g_tree_insert(rib->head_tree, &head->key, head);
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

    /* key_destroy=NULL（键内嵌于 value），value_destroy=head_free */
    rib->head_tree = g_tree_new_full(head_key_cmp, NULL, NULL, head_free);
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

int route_rib_add(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                  uint32_t protocol, const net_addr_t *source, const net_addr_t *nexthop, int32_t metric,
                  int32_t preference, uint32_t out_ifindex)
{
    if (!rib || !prefix_addr || !source || !nexthop)
    {
        return -1;
    }

    route_head_t *head = get_or_create_head(rib, vrf_id, afi, prefix_addr, prefix_len);
    if (!head)
    {
        return -1;
    }

    route_path_key_t tmp_key;
    make_path_key(&tmp_key, protocol, source);

    route_path_t *path = (route_path_t *)g_hash_table_lookup(head->path_hash, &tmp_key);
    int is_new = (path == NULL);

    if (is_new)
    {
        path = (route_path_t *)g_malloc0(sizeof(route_path_t));
        if (!path)
        {
            return -1;
        }
        path->key = tmp_key;
        /* key 内嵌于 value，GHashTable 直接保存 &path->key 指针 */
        g_hash_table_insert(head->path_hash, &path->key, path);
        rib->path_count++;
    }

    path->nexthop = *nexthop;
    path->metric = metric;
    path->preference = preference;
    path->out_ifindex = out_ifindex;
    path->updated_at_usec = g_get_real_time();

    return is_new ? 1 : 0;
}

int route_rib_del(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                  uint32_t protocol, const net_addr_t *source, route_path_cb cb, void *userdata)
{
    if (!rib || !prefix_addr || !source)
    {
        return -1;
    }

    route_head_key_t head_key;
    make_head_key(&head_key, vrf_id, afi, prefix_addr, prefix_len);

    route_head_t *head = (route_head_t *)g_tree_lookup(rib->head_tree, &head_key);
    if (!head)
    {
        return 0;
    }

    route_path_key_t path_key;
    make_path_key(&path_key, protocol, source);

    route_path_t *path = (route_path_t *)g_hash_table_lookup(head->path_hash, &path_key);
    if (!path)
    {
        return 0;
    }

    /* 删除前触发回调 */
    if (cb)
    {
        cb(head, path, userdata);
    }

    g_hash_table_remove(head->path_hash, &path_key);
    rib->path_count--;

    /* 前缀头下没有路径时，从树中移除（head 随即被 head_free 释放） */
    if (g_hash_table_size(head->path_hash) == 0)
    {
        g_tree_remove(rib->head_tree, &head->key);
        rib->head_count--;
    }

    return 1;
}

int route_rib_del_proto_for_prefix(route_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr,
                                   uint8_t prefix_len, uint32_t protocol, route_path_cb cb, void *userdata)
{
    if (!rib || !prefix_addr)
    {
        return -1;
    }

    route_head_key_t head_key;
    make_head_key(&head_key, vrf_id, afi, prefix_addr, prefix_len);

    route_head_t *head = (route_head_t *)g_tree_lookup(rib->head_tree, &head_key);
    if (!head)
    {
        return 0;
    }

    /* 先收集匹配路径键的副本（避免边遍历边删除） */
    GArray *keys_to_del = g_array_new(FALSE, FALSE, sizeof(route_path_key_t));
    GHashTableIter iter;
    gpointer key_ptr, val_ptr;
    g_hash_table_iter_init(&iter, head->path_hash);
    while (g_hash_table_iter_next(&iter, &key_ptr, &val_ptr))
    {
        route_path_t *p = (route_path_t *)val_ptr;
        if (p->key.protocol == protocol)
        {
            g_array_append_val(keys_to_del, p->key);
        }
    }

    int count = 0;
    for (guint i = 0; i < keys_to_del->len; i++)
    {
        route_path_key_t *pk = &g_array_index(keys_to_del, route_path_key_t, i);
        route_path_t *p = (route_path_t *)g_hash_table_lookup(head->path_hash, pk);
        if (p)
        {
            if (cb)
            {
                cb(head, p, userdata);
            }
            g_hash_table_remove(head->path_hash, pk);
            rib->path_count--;
            count++;
        }
    }
    g_array_free(keys_to_del, TRUE);

    if (g_hash_table_size(head->path_hash) == 0)
    {
        g_tree_remove(rib->head_tree, &head->key);
        rib->head_count--;
    }

    return count;
}

// ============================================================================
// 查询
// ============================================================================

const route_head_t *route_rib_lookup_head(const route_rib_t *rib, uint32_t vrf_id, uint16_t afi,
                                          const net_addr_t *prefix_addr, uint8_t prefix_len)
{
    if (!rib || !prefix_addr)
    {
        return NULL;
    }

    route_head_key_t key;
    make_head_key(&key, vrf_id, afi, prefix_addr, prefix_len);
    return (const route_head_t *)g_tree_lookup(rib->head_tree, &key);
}

const route_path_t *route_rib_lookup_path(const route_head_t *head, uint32_t protocol, const net_addr_t *source)
{
    if (!head || !source)
    {
        return NULL;
    }

    route_path_key_t key;
    make_path_key(&key, protocol, source);
    return (const route_path_t *)g_hash_table_lookup(head->path_hash, &key);
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
    (void)key;
    route_head_t *head = (route_head_t *)value;
    walk_ctx_t *ctx = (walk_ctx_t *)data;

    /* VRF 过滤 */
    if (ctx->vrf_filter != ROUTE_VRF_ALL && head->key.vrf_id != ctx->vrf_filter)
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
        if (ctx->proto_filter != ROUTE_PROTOCOL_MAX && path->key.protocol != ctx->proto_filter)
        {
            continue;
        }
        ctx->cb(head, path, ctx->userdata);
    }

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
