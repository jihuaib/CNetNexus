/**
 * @file   route_nhobj.c
 * @brief  Route nexthop 对象分配器实现（registry 引用 + FIB 引用双计数，对标 tunnel-id）
 * @author jhb
 * @date   2026/06/03
 */
#include "route_nhobj.h"

#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "fib.h"
#include "log.h"
#include "route_main.h"

/** nexthop 对象表项：键内嵌（作为 by_key 表的键指针），双引用计数 */
typedef struct route_nhobj_entry
{
    route_nhobj_key_t key;  /**< 去重键（原始下一跳身份） */
    uint32_t nexthop_id;    /**< 对象 ID（非 0） */
    uint32_t refcount;      /**< registry 引用计数（route_path 持有） */
    uint32_t fib_refcount;  /**< FIB 引用计数（已安装路由持有，>0 表示已下刷 FIB） */
    net_addr_t relay_addr;  /**< relay 解析后的网关（下 FIB 用） */
    uint32_t relay_ifindex; /**< relay 解析后的出接口（下 FIB 用） */
} route_nhobj_entry_t;

#define ROUTE_NHOBJ_PROTO_SLOTS 8u /**< 协议分区数（覆盖 ROUTE_PROTOCOL_* 0..5） */

static GHashTable *g_nhobj_by_key = NULL;                    /**< route_nhobj_key_t* -> entry*（拥有 value） */
static GHashTable *g_nhobj_by_id = NULL;                     /**< nexthop_id -> entry*（借用，不拥有） */
static uint32_t g_nhobj_next[ROUTE_NHOBJ_PROTO_SLOTS] = {0}; /**< 每协议分区的下一个候选 id */

// ============================================================================
// 键 hash / equal
// ============================================================================

static guint nhobj_key_hash(gconstpointer p)
{
    const route_nhobj_key_t *k = (const route_nhobj_key_t *)p;
    guint h = k->vrf_id;
    h = h * 33u + k->protocol;
    h = h * 33u + k->afi;
    h = h * 33u + k->nh_type;
    h = h * 33u + k->key_ifindex;
    h ^= net_addr_hash(&k->nexthop);
    return h;
}

void route_nhobj_foreach(route_nhobj_iter_fn fn, void *user)
{
    if (!fn || !g_nhobj_by_id)
    {
        return;
    }
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_nhobj_by_id);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const route_nhobj_entry_t *e = (const route_nhobj_entry_t *)value;
        route_nhobj_info_t info;
        info.key = e->key;
        info.relay_addr = e->relay_addr;
        info.relay_ifindex = e->relay_ifindex;
        fn(e->nexthop_id, &info, e->refcount, e->fib_refcount, user);
    }
}

int route_nhobj_key_equal(const route_nhobj_key_t *a, const route_nhobj_key_t *b)
{
    if (!a || !b)
    {
        return 0;
    }
    return a->vrf_id == b->vrf_id && a->protocol == b->protocol && a->afi == b->afi && a->nh_type == b->nh_type &&
           a->key_ifindex == b->key_ifindex && net_addr_equal(&a->nexthop, &b->nexthop);
}

static gboolean nhobj_key_equal(gconstpointer a, gconstpointer b)
{
    return route_nhobj_key_equal((const route_nhobj_key_t *)a, (const route_nhobj_key_t *)b) ? TRUE : FALSE;
}

// ============================================================================
// id 分配（单调自增 + 回绕跳过在用）
// ============================================================================

/** 在协议分区 [base, base+SPAN) 内分配一个未占用 id（单调自增，回绕跳过在用） */
static uint32_t nhobj_id_alloc(uint32_t protocol)
{
    uint32_t slot = (protocol < ROUTE_NHOBJ_PROTO_SLOTS) ? protocol : ROUTE_PROTOCOL_STATIC;
    uint32_t base = ROUTE_NHOBJ_ID_BASE(slot);
    uint32_t end = base + ROUTE_NHOBJ_ID_SPAN;
    if (g_nhobj_next[slot] < base || g_nhobj_next[slot] >= end)
    {
        g_nhobj_next[slot] = base;
    }

    uint32_t start = g_nhobj_next[slot];
    do
    {
        uint32_t id = g_nhobj_next[slot]++;
        if (g_nhobj_next[slot] >= end)
        {
            g_nhobj_next[slot] = base;
        }
        if (!g_hash_table_contains(g_nhobj_by_id, GUINT_TO_POINTER(id)))
        {
            return id;
        }
    } while (g_nhobj_next[slot] != start);

    return 0u;
}

// ============================================================================
// FIB 下刷
// ============================================================================

static void nhobj_fib_upsert(const route_nhobj_entry_t *entry)
{
    fib_nexthop_entry_t fib;
    memset(&fib, 0, sizeof(fib));
    fib.nexthop_id = entry->nexthop_id;
    fib.vrf_id = entry->key.vrf_id;
    fib.afi = entry->key.afi;
    fib.nh_type = entry->key.nh_type;
    fib.state = 1u;
    fib.out_ifindex = entry->relay_ifindex;
    fib.gateway_addr = entry->relay_addr;
    if (fib_rpc_nexthop_upsert(route_local_ipc_ctx(), &fib) != ERRCODE_SUCCESS)
    {
        LOG_WARN("[route_nhobj] FIB nexthop upsert 失败 id=%u", entry->nexthop_id);
    }
}

static void nhobj_fib_delete(uint32_t nexthop_id)
{
    fib_nexthop_entry_t fib;
    memset(&fib, 0, sizeof(fib));
    fib.nexthop_id = nexthop_id;
    if (fib_rpc_nexthop_delete(route_local_ipc_ctx(), &fib) != ERRCODE_SUCCESS)
    {
        LOG_WARN("[route_nhobj] FIB nexthop delete 失败 id=%u", nexthop_id);
    }
}

// ============================================================================
// 生命周期
// ============================================================================

void route_nhobj_init(void)
{
    if (!g_nhobj_by_key)
    {
        g_nhobj_by_key = g_hash_table_new_full(nhobj_key_hash, nhobj_key_equal, NULL, g_free);
    }
    if (!g_nhobj_by_id)
    {
        g_nhobj_by_id = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
    }
    memset(g_nhobj_next, 0, sizeof(g_nhobj_next));
}

void route_nhobj_cleanup(void)
{
    if (g_nhobj_by_id)
    {
        g_hash_table_destroy(g_nhobj_by_id);
        g_nhobj_by_id = NULL;
    }
    if (g_nhobj_by_key)
    {
        g_hash_table_destroy(g_nhobj_by_key);
        g_nhobj_by_key = NULL;
    }
}

// ============================================================================
// registry 引用
// ============================================================================

int route_nhobj_acquire(const route_nhobj_key_t *key, uint32_t want_id, uint32_t *id_out)
{
    if (!key || !id_out || !g_nhobj_by_key || !g_nhobj_by_id)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_entry_t *entry = (route_nhobj_entry_t *)g_hash_table_lookup(g_nhobj_by_key, key);
    if (entry)
    {
        entry->refcount++;
        *id_out = entry->nexthop_id;
        return ERRCODE_SUCCESS;
    }

    uint32_t id;
    if (want_id != 0u)
    {
        /* 业务进程重启后按原 id 恢复对象：id 落在该协议分区，正常不会被占用 */
        if (g_hash_table_contains(g_nhobj_by_id, GUINT_TO_POINTER(want_id)))
        {
            LOG_ERROR("[route_nhobj] 恢复 id=%u 冲突（已被其它 nexthop 占用）", want_id);
            return ERRCODE_FAIL;
        }
        id = want_id;
    }
    else
    {
        id = nhobj_id_alloc(key->protocol);
        if (id == 0u)
        {
            LOG_ERROR("[route_nhobj] nexthop id 空间耗尽");
            return ERRCODE_FAIL;
        }
    }

    entry = g_malloc0(sizeof(*entry));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }
    entry->key = *key;
    entry->nexthop_id = id;
    entry->refcount = 1u;
    entry->fib_refcount = 0u;
    memset(&entry->relay_addr, 0, sizeof(entry->relay_addr));
    entry->relay_ifindex = 0u;
    g_hash_table_insert(g_nhobj_by_key, &entry->key, entry);
    g_hash_table_insert(g_nhobj_by_id, GUINT_TO_POINTER(id), entry);

    *id_out = id;
    return ERRCODE_SUCCESS;
}

int route_nhobj_retain(uint32_t id)
{
    if (id == 0u || !g_nhobj_by_id)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_entry_t *entry = (route_nhobj_entry_t *)g_hash_table_lookup(g_nhobj_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    entry->refcount++;
    return ERRCODE_SUCCESS;
}

void route_nhobj_release(uint32_t id)
{
    if (id == 0u || !g_nhobj_by_id || !g_nhobj_by_key)
    {
        return;
    }

    route_nhobj_entry_t *entry = (route_nhobj_entry_t *)g_hash_table_lookup(g_nhobj_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return;
    }
    if (entry->refcount > 1u)
    {
        entry->refcount--;
        return;
    }

    /* registry 引用归零：若仍处于 FIB attach 态先撤 FIB，再从两表删除 */
    if (entry->fib_refcount > 0u)
    {
        nhobj_fib_delete(entry->nexthop_id);
        entry->fib_refcount = 0u;
    }
    g_hash_table_remove(g_nhobj_by_id, GUINT_TO_POINTER(id));
    g_hash_table_remove(g_nhobj_by_key, &entry->key); /* GDestroyNotify 释放 entry */
}

void route_nhobj_set_relay(uint32_t id, const net_addr_t *relay_addr, uint32_t relay_ifindex)
{
    if (id == 0u || !g_nhobj_by_id)
    {
        return;
    }

    route_nhobj_entry_t *entry = (route_nhobj_entry_t *)g_hash_table_lookup(g_nhobj_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return;
    }

    net_addr_t relay;
    memset(&relay, 0, sizeof(relay));
    if (relay_addr)
    {
        relay = *relay_addr;
    }

    if (entry->relay_ifindex == relay_ifindex && net_addr_equal(&entry->relay_addr, &relay))
    {
        return; /* 无变化 */
    }
    entry->relay_addr = relay;
    entry->relay_ifindex = relay_ifindex;
    if (entry->fib_refcount > 0u)
    {
        nhobj_fib_upsert(entry); /* 已下刷过则更新 FIB（id 不变） */
    }
}

int route_nhobj_lookup(uint32_t id, route_nhobj_info_t *out)
{
    if (id == 0u || !out || !g_nhobj_by_id)
    {
        return ERRCODE_FAIL;
    }

    route_nhobj_entry_t *entry = (route_nhobj_entry_t *)g_hash_table_lookup(g_nhobj_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }
    out->key = entry->key;
    out->relay_addr = entry->relay_addr;
    out->relay_ifindex = entry->relay_ifindex;
    return ERRCODE_SUCCESS;
}

// ============================================================================
// FIB 引用
// ============================================================================

void route_nhobj_fib_attach(uint32_t id)
{
    if (id == 0u || !g_nhobj_by_id)
    {
        return;
    }

    route_nhobj_entry_t *entry = (route_nhobj_entry_t *)g_hash_table_lookup(g_nhobj_by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return;
    }
    entry->fib_refcount++;
    if (entry->fib_refcount == 1u)
    {
        nhobj_fib_upsert(entry); /* 首个 FIB 引用：下刷对象到 FIB */
    }
}

void route_nhobj_fib_detach(uint32_t id)
{
    if (id == 0u || !g_nhobj_by_id)
    {
        return;
    }

    route_nhobj_entry_t *entry = (route_nhobj_entry_t *)g_hash_table_lookup(g_nhobj_by_id, GUINT_TO_POINTER(id));
    if (!entry || entry->fib_refcount == 0u)
    {
        return;
    }
    entry->fib_refcount--;
    if (entry->fib_refcount == 0u)
    {
        nhobj_fib_delete(entry->nexthop_id); /* 末个 FIB 引用：撤销 FIB 对象 */
    }
}
