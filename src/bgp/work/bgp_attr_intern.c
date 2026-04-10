/**
 * @file   bgp_attr_intern.c
 * @brief  BGP 路径属性去重（intern）实现
 * @author jhb
 * @date   2026/04/09
 */
#include "bgp_attr_intern.h"

#include <glib.h>
#include <string.h>

#include "log.h"

/* ============================================================================
 * 全局状态
 * ========================================================================== */

/** 属性 intern 表：key = bgp_attr_ref_t*，value = 同一指针 */
static GHashTable *g_attr_table = NULL;

/** attr_id → bgp_attr_ref_t* 反查表（用于按 ID 查找） */
static GHashTable *g_attr_id_table = NULL;

/** 下一个可分配的 attr_id（从 1 开始，0 表示无效） */
static uint32_t g_next_attr_id = 1;

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
    if (g_attr_table)
    {
        return; /* 幂等 */
    }
    g_attr_table = g_hash_table_new(g_hash_attr_ref, g_equal_attr_ref);
    g_attr_id_table = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_next_attr_id = 1;
    LOG_INFO("BGP: attr intern 表已初始化");
}

void bgp_attr_intern_fini(void)
{
    if (g_attr_id_table)
    {
        g_hash_table_destroy(g_attr_id_table);
        g_attr_id_table = NULL;
    }
    if (g_attr_table)
    {
        /* 释放所有残留 ref（正常情况应为空） */
        GHashTableIter iter;
        gpointer key;
        g_hash_table_iter_init(&iter, g_attr_table);
        while (g_hash_table_iter_next(&iter, &key, NULL))
        {
            g_free(key);
        }
        g_hash_table_destroy(g_attr_table);
        g_attr_table = NULL;
    }
    g_next_attr_id = 1;
}

bgp_attr_ref_t *bgp_attr_intern(const bgp_attr_t *attr)
{
    if (!attr || !g_attr_table)
    {
        return NULL;
    }

    /* 构造临时 probe（栈上），用于查表 */
    bgp_attr_ref_t probe;
    memcpy(&probe.attr, attr, sizeof(bgp_attr_t));
    probe.hash = attr_hash_fn(attr);

    bgp_attr_ref_t *existing = (bgp_attr_ref_t *)g_hash_table_lookup(g_attr_table, &probe);
    if (existing)
    {
        existing->refcnt++;
        return existing;
    }

    /* 不存在，分配新节点 */
    bgp_attr_ref_t *ref = g_malloc(sizeof(bgp_attr_ref_t));
    memcpy(&ref->attr, attr, sizeof(bgp_attr_t));
    ref->refcnt = 1;
    ref->hash = probe.hash;
    ref->attr_id = g_next_attr_id++;

    g_hash_table_insert(g_attr_table, ref, ref);
    g_hash_table_insert(g_attr_id_table, GUINT_TO_POINTER(ref->attr_id), ref);

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
        if (g_attr_id_table)
        {
            g_hash_table_remove(g_attr_id_table, GUINT_TO_POINTER(ref->attr_id));
        }
        if (g_attr_table)
        {
            g_hash_table_remove(g_attr_table, ref);
        }
        g_free(ref);
    }
}

const bgp_attr_ref_t *bgp_attr_find_by_id(uint32_t attr_id)
{
    if (!g_attr_id_table || attr_id == 0)
    {
        return NULL;
    }
    return (const bgp_attr_ref_t *)g_hash_table_lookup(g_attr_id_table, GUINT_TO_POINTER(attr_id));
}

uint32_t bgp_attr_intern_count(void)
{
    return g_attr_table ? g_hash_table_size(g_attr_table) : 0;
}
