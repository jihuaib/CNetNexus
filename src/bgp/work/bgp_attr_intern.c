/**
 * @file   bgp_attr_intern.c
 * @brief  BGP 路径属性：intern 存储 + 协议语义 helper 实现
 * @author jhb
 * @date   2026/04/09
 */
#include "bgp_attr_intern.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_instance.h"
#include "log.h"

/* ============================================================================
 * Hash / Equal
 * ========================================================================== */

/**
 * @brief FNV-1a 哈希（对 bgp_attr_t 全部字节）
 */
static uint32_t attr_hash_fn(const bgp_attr_t *attr)
{
    const uint8_t *data = (const uint8_t *)attr;
    uint32_t h = 2166136261u; /* FNV offset basis */
    for (size_t i = 0; i < sizeof(bgp_attr_t); i++)
    {
        h ^= data[i];
        h *= 16777619u; /* FNV prime */
    }
    return h;
}

static guint g_hash_attr_ref(gconstpointer key)
{
    const bgp_attr_ref_t *ref = (const bgp_attr_ref_t *)key;
    return (guint)ref->hash;
}

static gboolean g_equal_attr_ref(gconstpointer a, gconstpointer b)
{
    const bgp_attr_ref_t *ra = (const bgp_attr_ref_t *)a;
    const bgp_attr_ref_t *rb = (const bgp_attr_ref_t *)b;
    return memcmp(&ra->attr, &rb->attr, sizeof(bgp_attr_t)) == 0;
}

/* ============================================================================
 * API
 * ========================================================================== */

void bgp_attr_intern_init(void)
{
    LOG_INFO("BGP: attr intern 模块已初始化");
}

void bgp_attr_intern_fini(void) {}

void bgp_attr_store_init(bgp_instance_t *inst)
{
    if (!inst || inst->attr_table)
    {
        return;
    }
    inst->attr_table = g_hash_table_new(g_hash_attr_ref, g_equal_attr_ref);
    inst->attr_id_table = g_hash_table_new(g_direct_hash, g_direct_equal);
    inst->next_attr_id = 1;
}

void bgp_attr_store_destroy(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }
    if (inst->attr_id_table)
    {
        g_hash_table_destroy(inst->attr_id_table);
        inst->attr_id_table = NULL;
    }
    if (inst->attr_table)
    {
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, inst->attr_table);
        while (g_hash_table_iter_next(&iter, &key, NULL))
        {
            g_free(key);
        }
        g_hash_table_destroy(inst->attr_table);
        inst->attr_table = NULL;
    }
    inst->next_attr_id = 1;
}

bgp_attr_ref_t *bgp_attr_intern(bgp_instance_t *inst, const bgp_attr_t *attr)
{
    if (!inst || !attr)
    {
        return NULL;
    }
    if (!inst->attr_table || !inst->attr_id_table)
    {
        bgp_attr_store_init(inst);
    }
    if (!inst->attr_table || !inst->attr_id_table)
    {
        return NULL;
    }

    /* 构造临时 probe（栈上），用于查表 */
    bgp_attr_ref_t probe;
    memcpy(&probe.attr, attr, sizeof(bgp_attr_t));
    probe.hash = attr_hash_fn(attr);

    bgp_attr_ref_t *existing = (bgp_attr_ref_t *)g_hash_table_lookup(inst->attr_table, &probe);
    if (existing)
    {
        existing->refcnt++;
        return existing;
    }

    /* 不存在，分配新节点 */
    bgp_attr_ref_t *ref = g_malloc(sizeof(bgp_attr_ref_t));
    ref->inst = inst;
    memcpy(&ref->attr, attr, sizeof(bgp_attr_t));
    ref->refcnt = 1;
    ref->hash = probe.hash;
    ref->attr_id = inst->next_attr_id++;

    g_hash_table_insert(inst->attr_table, ref, ref);
    g_hash_table_insert(inst->attr_id_table, GUINT_TO_POINTER(ref->attr_id), ref);

    return ref;
}

void bgp_attr_ref_get(bgp_attr_ref_t *ref)
{
    if (ref)
    {
        ref->refcnt++;
    }
}

void bgp_attr_release(bgp_attr_ref_t *ref)
{
    if (!ref)
    {
        return;
    }
    if (ref->refcnt == 0)
    {
        return;
    }

    ref->refcnt--;
    if (ref->refcnt == 0)
    {
        bgp_instance_t *inst = ref->inst;
        if (inst && inst->attr_id_table)
        {
            g_hash_table_remove(inst->attr_id_table, GUINT_TO_POINTER(ref->attr_id));
        }
        if (inst && inst->attr_table)
        {
            g_hash_table_remove(inst->attr_table, ref);
        }
        g_free(ref);
    }
}

const bgp_attr_ref_t *bgp_attr_find_by_id(const bgp_instance_t *inst, uint32_t attr_id)
{
    if (!inst || !inst->attr_id_table || attr_id == 0)
    {
        return NULL;
    }
    return (const bgp_attr_ref_t *)g_hash_table_lookup(inst->attr_id_table, GUINT_TO_POINTER(attr_id));
}

uint32_t bgp_attr_intern_count(const bgp_instance_t *inst)
{
    return (inst && inst->attr_table) ? g_hash_table_size(inst->attr_table) : 0;
}

void bgp_attr_intern_foreach(const bgp_instance_t *inst, bgp_attr_intern_cb cb, gpointer user_data)
{
    if (!inst || !cb || !inst->attr_table)
    {
        return;
    }
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, inst->attr_table);
    while (g_hash_table_iter_next(&iter, &key, NULL))
    {
        cb((const bgp_attr_ref_t *)key, user_data);
    }
}

/* ============================================================================
 * 属性语义 helper
 * ========================================================================== */

gboolean bgp_attr_as_path_contains_as(const char *as_path, uint32_t asn)
{
    if (!as_path || as_path[0] == '\0' || asn == 0u)
    {
        return FALSE;
    }

    const char *p = as_path;
    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t' || *p == '{' || *p == '}' || *p == ',')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }

        char *end = NULL;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p)
        {
            p++;
            continue;
        }
        if ((uint32_t)v == asn)
        {
            return TRUE;
        }
        p = end;
    }

    return FALSE;
}

gboolean bgp_attr_is_as_loop(const bgp_attr_t *attr, uint32_t local_as)
{
    if (!attr || local_as == 0u)
    {
        return FALSE;
    }
    return bgp_attr_as_path_contains_as(attr->as_path, local_as);
}

void bgp_attr_build_imported(bgp_attr_t *attr)
{
    if (!attr)
    {
        return;
    }
    memset(attr, 0, sizeof(*attr));
    attr->origin = BGP_ORIGIN_INCOMPLETE;
    attr->local_pref = 100;
    attr->has_local_pref = true;
}

void bgp_nexthop_from_addr(bgp_nexthop_t *nh, const net_addr_t *addr)
{
    if (!nh)
    {
        return;
    }
    memset(nh, 0, sizeof(*nh));
    nh->has_link_local = false;
    if (addr)
    {
        nh->global = *addr;
    }
}
