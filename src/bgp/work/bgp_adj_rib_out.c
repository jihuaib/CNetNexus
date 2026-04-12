/**
 * @file   bgp_adj_rib_out.c
 * @brief  BGP Adj-RIB-Out 实现
 * @author jhb
 * @date   2026/04/10
 */
#include "bgp_adj_rib_out.h"

#include <string.h>

/* ============================================================================
 * NLRI hash / equal（GHashTable 使用）
 * ========================================================================== */

/** FNV-1a 64 位散列（截断到 guint），bgp_nlri_entry_t 为值类型，按字节散列安全 */
static guint nlri_hash(gconstpointer p)
{
    const bgp_nlri_entry_t *nlri = p;
    const uint8_t *bytes = (const uint8_t *)nlri;
    size_t len = sizeof(*nlri);
    uint64_t h = 1469598103934665603ULL; /* FNV-1a offset basis */
    for (size_t i = 0; i < len; i++)
    {
        h ^= bytes[i];
        h *= 1099511628211ULL; /* FNV-1a prime */
    }
    return (guint)h;
}

static gboolean nlri_equal(gconstpointer a, gconstpointer b)
{
    return bgp_nlri_equal((const bgp_nlri_entry_t *)a, (const bgp_nlri_entry_t *)b);
}

static void nlri_key_free(gpointer p)
{
    g_free(p);
}

static void entry_free(gpointer p)
{
    if (!p)
    {
        return;
    }
    bgp_adj_rib_out_entry_t *e = p;
    if (e->attr_ref)
    {
        bgp_attr_release(e->attr_ref);
        e->attr_ref = NULL;
    }
    g_free(e);
}

/* ============================================================================
 * 生命周期
 * ========================================================================== */

bgp_adj_rib_out_t *bgp_adj_rib_out_create(void)
{
    bgp_adj_rib_out_t *aro = g_malloc0(sizeof(bgp_adj_rib_out_t));
    aro->table = g_hash_table_new_full(nlri_hash, nlri_equal, nlri_key_free, entry_free);
    aro->count = 0;
    return aro;
}

void bgp_adj_rib_out_destroy(bgp_adj_rib_out_t *aro)
{
    if (!aro)
    {
        return;
    }
    if (aro->table)
    {
        g_hash_table_destroy(aro->table);
        aro->table = NULL;
    }
    g_free(aro);
}

void bgp_adj_rib_out_clear(bgp_adj_rib_out_t *aro)
{
    if (!aro || !aro->table)
    {
        return;
    }
    g_hash_table_remove_all(aro->table);
    aro->count = 0;
}

/* ============================================================================
 * 查找 / 更新 / 移除
 * ========================================================================== */

const bgp_adj_rib_out_entry_t *bgp_adj_rib_out_lookup(const bgp_adj_rib_out_t *aro, const bgp_nlri_entry_t *nlri)
{
    if (!aro || !aro->table || !nlri)
    {
        return NULL;
    }
    return (const bgp_adj_rib_out_entry_t *)g_hash_table_lookup(aro->table, nlri);
}

bgp_aro_change_t bgp_adj_rib_out_update(bgp_adj_rib_out_t *aro, const bgp_nlri_entry_t *nlri, bgp_attr_ref_t *attr_ref,
                                        const bgp_nexthop_t *nh)
{
    if (!aro || !aro->table || !nlri || !attr_ref)
    {
        return BGP_ARO_UNCHANGED;
    }

    bgp_adj_rib_out_entry_t *existing = g_hash_table_lookup(aro->table, nlri);
    if (existing)
    {
        /* 判断是否真的变化 */
        gboolean attr_same = (existing->attr_ref == attr_ref) ||
                             (existing->attr_ref && attr_ref && existing->attr_ref->attr_id == attr_ref->attr_id);
        gboolean nh_same = (nh && memcmp(&existing->nexthop, nh, sizeof(*nh)) == 0);
        if (attr_same && nh_same)
        {
            return BGP_ARO_UNCHANGED;
        }

        /* 更新：释放旧属性引用，持有新属性引用 */
        bgp_attr_ref_t *old_ref = existing->attr_ref;
        bgp_attr_ref_get(attr_ref);
        existing->attr_ref = attr_ref;
        if (nh)
        {
            existing->nexthop = *nh;
        }
        existing->advertised = false;
        if (old_ref)
        {
            bgp_attr_release(old_ref);
        }
        return BGP_ARO_UPDATED;
    }

    /* 新增 */
    bgp_nlri_entry_t *key = g_malloc(sizeof(*key));
    memcpy(key, nlri, sizeof(*key));
    bgp_adj_rib_out_entry_t *entry = g_malloc0(sizeof(*entry));
    bgp_attr_ref_get(attr_ref);
    entry->attr_ref = attr_ref;
    if (nh)
    {
        entry->nexthop = *nh;
    }
    entry->advertised = false;
    g_hash_table_insert(aro->table, key, entry);
    aro->count++;
    return BGP_ARO_NEW;
}

bool bgp_adj_rib_out_remove(bgp_adj_rib_out_t *aro, const bgp_nlri_entry_t *nlri)
{
    if (!aro || !aro->table || !nlri)
    {
        return false;
    }
    if (g_hash_table_remove(aro->table, nlri))
    {
        if (aro->count > 0)
        {
            aro->count--;
        }
        return true;
    }
    return false;
}

void bgp_adj_rib_out_mark_advertised(bgp_adj_rib_out_t *aro, const bgp_nlri_entry_t *nlri)
{
    if (!aro || !aro->table || !nlri)
    {
        return;
    }
    bgp_adj_rib_out_entry_t *entry = g_hash_table_lookup(aro->table, nlri);
    if (entry)
    {
        entry->advertised = true;
    }
}

uint32_t bgp_adj_rib_out_count(const bgp_adj_rib_out_t *aro)
{
    return aro ? aro->count : 0u;
}

/* ============================================================================
 * 遍历
 * ========================================================================== */

typedef struct
{
    bgp_adj_rib_out_cb cb;
    gpointer user_data;
} foreach_ctx_t;

static void foreach_adapter(gpointer key, gpointer value, gpointer user_data)
{
    foreach_ctx_t *ctx = user_data;
    ctx->cb((const bgp_nlri_entry_t *)key, (const bgp_adj_rib_out_entry_t *)value, ctx->user_data);
}

void bgp_adj_rib_out_foreach(const bgp_adj_rib_out_t *aro, bgp_adj_rib_out_cb cb, gpointer user_data)
{
    if (!aro || !aro->table || !cb)
    {
        return;
    }
    foreach_ctx_t ctx = {.cb = cb, .user_data = user_data};
    g_hash_table_foreach(aro->table, foreach_adapter, &ctx);
}
