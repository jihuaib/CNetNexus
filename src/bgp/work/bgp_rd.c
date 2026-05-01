/**
 * @file   bgp_rd.c
 * @brief  BGP RD entry 管理（protocol 级 hash + per-RD RIB 持有）
 * @author jhb
 * @date   2026/05/01
 */
#include "bgp_rd.h"

#include <string.h>

#include "bgp_instance.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"

const bgp_rd_t BGP_RD_PUBLIC = {{0, 0, 0, 0, 0, 0, 0, 0}};

/* ============================================================================
 * 复合键 hash / equal
 * ========================================================================== */

guint bgp_rd_key_hash(gconstpointer key)
{
    const uint8_t *p = (const uint8_t *)key;
    /* FNV-1a 32 位，覆盖 16 字节复合键 */
    guint h = 2166136261u;
    for (size_t i = 0; i < sizeof(bgp_rd_key_t); i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

gboolean bgp_rd_key_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(bgp_rd_key_t)) == 0;
}

guint bgp_rd_hash(gconstpointer key)
{
    const uint8_t *p = (const uint8_t *)key;
    guint h = 2166136261U;
    for (size_t i = 0; i < sizeof(bgp_rd_t); i++)
    {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

gboolean bgp_rd_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(bgp_rd_t)) == 0;
}

/* ============================================================================
 * NLRI → RD 提取
 * ========================================================================== */

void bgp_nlri_extract_rd(const bgp_nlri_entry_t *nlri, bgp_rd_t *out)
{
    if (!out)
    {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!nlri)
    {
        return;
    }
    switch (nlri->type)
    {
        case BGP_NLRI_PREFIX:
            if (nlri->prefix.has_rd)
            {
                *out = nlri->prefix.rd;
            }
            break;
        case BGP_NLRI_EVPN:
            *out = nlri->evpn.rd;
            break;
        case BGP_NLRI_FLOWSPEC:
            if (nlri->flowspec.has_rd)
            {
                *out = nlri->flowspec.rd;
            }
            break;
        default:
            break;
    }
}

/* ============================================================================
 * 内部辅助：键构造
 * ========================================================================== */

static void rd_key_init(bgp_rd_key_t *k, uint16_t afi, uint8_t safi, uint32_t vrf_id, const bgp_rd_t *rd)
{
    memset(k, 0, sizeof(*k));
    k->afi = afi;
    k->safi = safi;
    k->vrf_id = vrf_id;
    if (rd)
    {
        k->rd = *rd;
    }
}

static bgp_rd_entry_t *rd_entry_create(const bgp_rd_key_t *key, bgp_instance_t *inst)
{
    bgp_rd_entry_t *e = g_malloc0(sizeof(bgp_rd_entry_t));
    e->key = *key;
    e->inst = inst;
    e->rib = bgp_rib_create();
    e->rib->rd_entry = e;
    return e;
}

static void rd_entry_destroy(gpointer data)
{
    bgp_rd_entry_t *e = (bgp_rd_entry_t *)data;
    if (!e)
    {
        return;
    }
    if (e->rib)
    {
        bgp_rib_destroy(e->rib);
        e->rib = NULL;
    }
    g_free(e);
}

/* ============================================================================
 * protocol 级 ensure / find / remove
 * ========================================================================== */

bgp_rd_entry_t *bgp_protocol_find_rd_entry(bgp_protocol_t *proto, uint16_t afi, uint8_t safi, uint32_t vrf_id,
                                           const bgp_rd_t *rd)
{
    if (!proto || !proto->rd_hash)
    {
        return NULL;
    }
    bgp_rd_key_t key;
    rd_key_init(&key, afi, safi, vrf_id, rd);
    return g_hash_table_lookup(proto->rd_hash, &key);
}

bgp_rd_entry_t *bgp_protocol_ensure_rd_entry(bgp_protocol_t *proto, bgp_instance_t *inst, const bgp_rd_t *rd)
{
    if (!proto || !proto->rd_hash || !inst || !inst->vrf)
    {
        return NULL;
    }

    bgp_rd_key_t key;
    rd_key_init(&key, (uint16_t)inst->afi, (uint8_t)inst->safi, inst->vrf->vrf_id, rd);

    bgp_rd_entry_t *e = g_hash_table_lookup(proto->rd_hash, &key);
    if (e)
    {
        return e;
    }

    e = rd_entry_create(&key, inst);
    g_hash_table_insert(proto->rd_hash, &e->key, e);

    if (inst->rd_entries)
    {
        g_hash_table_insert(inst->rd_entries, &e->key.rd, e);
    }
    return e;
}

void bgp_protocol_remove_rd_entry(bgp_protocol_t *proto, bgp_instance_t *inst, const bgp_rd_t *rd)
{
    if (!proto || !proto->rd_hash || !inst)
    {
        return;
    }
    bgp_rd_key_t key;
    rd_key_init(&key, (uint16_t)inst->afi, (uint8_t)inst->safi, inst->vrf ? inst->vrf->vrf_id : 0, rd);

    bgp_rd_entry_t *e = g_hash_table_lookup(proto->rd_hash, &key);
    if (!e)
    {
        return;
    }
    if (inst->rd_entries)
    {
        g_hash_table_remove(inst->rd_entries, &e->key.rd);
    }
    g_hash_table_remove(proto->rd_hash, &key); /* 触发 rd_entry_destroy */
}

void bgp_protocol_remove_all_inst_rd_entries(bgp_protocol_t *proto, bgp_instance_t *inst)
{
    if (!proto || !proto->rd_hash || !inst || !inst->rd_entries)
    {
        return;
    }
    /* 收集 RD 列表后再删，避免遍历时改 hash */
    GPtrArray *rds = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, inst->rd_entries);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        g_ptr_array_add(rds, key); /* key 即指向 entry->key.rd */
    }
    for (guint i = 0; i < rds->len; i++)
    {
        bgp_rd_t *rd = (bgp_rd_t *)g_ptr_array_index(rds, i);
        bgp_protocol_remove_rd_entry(proto, inst, rd);
    }
    g_ptr_array_free(rds, TRUE);
}

/* hash 表销毁回调注册：value 用 rd_entry_destroy；提供给 bgp_protocol_create 调用 */
GDestroyNotify bgp_rd_entry_destroy_notify(void)
{
    return rd_entry_destroy;
}
