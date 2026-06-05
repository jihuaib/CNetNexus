/**
 * @file   isis_nexthop.c
 * @brief  ISIS protocol nexthop registry.
 */
#include "isis_nexthop.h"

#include <glib.h>
#include <string.h>

#include "errcode.h"
#include "isis_main.h"
#include "log.h"
#include "route.h"

#define ISIS_NHOBJ_RPC_TIMEOUT_MS 3000u

typedef struct isis_nexthop_entry
{
    route_nhobj_key_t key;
    isis_nexthop_value_t value;
    uint32_t id;
    uint32_t refcount;
} isis_nexthop_entry_t;

struct isis_nexthop_table
{
    GHashTable *by_key;
    GHashTable *by_id;
};

static guint isis_nexthop_key_hash(gconstpointer p)
{
    const route_nhobj_key_t *k = (const route_nhobj_key_t *)p;
    if (!k)
    {
        return 0u;
    }

    guint h = (guint)k->vrf_id;
    h = h * 33u + (guint)k->protocol;
    h = h * 33u + (guint)k->afi;
    h = h * 33u + (guint)k->nh_type;
    h = h * 33u + (guint)k->key_ifindex;
    h ^= net_addr_hash(&k->nexthop);
    return h;
}

int isis_nexthop_key_equal(const route_nhobj_key_t *a, const route_nhobj_key_t *b)
{
    if (!a || !b)
    {
        return 0;
    }

    return a->vrf_id == b->vrf_id && a->protocol == b->protocol && a->afi == b->afi && a->nh_type == b->nh_type &&
           a->key_ifindex == b->key_ifindex && net_addr_equal(&a->nexthop, &b->nexthop);
}

static gboolean isis_nexthop_key_hash_equal(gconstpointer a, gconstpointer b)
{
    return isis_nexthop_key_equal((const route_nhobj_key_t *)a, (const route_nhobj_key_t *)b) ? TRUE : FALSE;
}

static void isis_zero_addr_for_afi(uint16_t afi, net_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->family = (afi == ROUTE_AFI_IPV6) ? AF_INET6 : AF_INET;
}

void isis_nexthop_make_route_key(uint16_t afi, uint32_t key_ifindex, const net_addr_t *nexthop, route_nhobj_key_t *key)
{
    if (!key)
    {
        return;
    }

    memset(key, 0, sizeof(*key));
    key->vrf_id = ROUTE_VRF_DEFAULT;
    key->protocol = ROUTE_PROTOCOL_ISIS;
    key->afi = afi;
    key->nh_type = ROUTE_NH_TYPE_IP;
    if (nexthop)
    {
        key->nexthop = *nexthop;
    }
    if (key->nexthop.family != AF_INET && key->nexthop.family != AF_INET6)
    {
        isis_zero_addr_for_afi(afi, &key->nexthop);
    }
    key->key_ifindex = key_ifindex;
}

/**
 * @brief 向 ROUTE 发送 acquire 并取回 id
 * @param key     nexthop 身份键
 * @param value   relay 解析值（relay_addr / out_ifindex）
 * @param want_id 期望 id：0=首次申请由 ROUTE 分配；非 0=重启反刷，要求 ROUTE 按该 id 恢复
 * @param id_out  ROUTE 返回的 id
 */
static int isis_nh_route_acquire(const route_nhobj_key_t *key, const isis_nexthop_value_t *value, uint32_t want_id,
                                 uint32_t *id_out)
{
    route_nhobj_msg_t req;
    memset(&req, 0, sizeof(req));
    req.key = *key;
    req.nexthop_id = want_id; /* 0=ROUTE 分配；非 0=要求 ROUTE 恢复该 id */
    req.relay_addr = value->relay_addr;
    req.relay_ifindex = value->out_ifindex;

    uint32_t got = 0u;
    if (route_rpc_nhobj_acquire_wait(isis_local_ipc_ctx(), &req, ISIS_NHOBJ_RPC_TIMEOUT_MS, &got) != ERRCODE_SUCCESS ||
        got == 0u || (want_id != 0u && got != want_id))
    {
        return ERRCODE_FAIL;
    }
    if (id_out)
    {
        *id_out = got;
    }
    return ERRCODE_SUCCESS;
}

isis_nexthop_table_t *isis_nexthop_table_create(void)
{
    isis_nexthop_table_t *table = g_malloc0(sizeof(*table));
    if (!table)
    {
        return NULL;
    }
    table->by_key = g_hash_table_new_full(isis_nexthop_key_hash, isis_nexthop_key_hash_equal, NULL, g_free);
    table->by_id = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
    if (!table->by_key || !table->by_id)
    {
        isis_nexthop_table_destroy(table);
        return NULL;
    }
    return table;
}

void isis_nexthop_table_destroy(isis_nexthop_table_t *table)
{
    if (!table)
    {
        return;
    }
    if (table->by_key)
    {
        GHashTableIter iter;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, table->by_key);
        while (g_hash_table_iter_next(&iter, NULL, &value))
        {
            const isis_nexthop_entry_t *entry = (const isis_nexthop_entry_t *)value;
            if (entry && entry->id != 0u)
            {
                (void)route_rpc_nhobj_release(isis_local_ipc_ctx(), entry->id);
            }
        }
    }

    if (table->by_id)
    {
        g_hash_table_destroy(table->by_id);
        table->by_id = NULL;
    }
    if (table->by_key)
    {
        g_hash_table_destroy(table->by_key);
        table->by_key = NULL;
    }
    g_free(table);
}

int isis_nexthop_acquire(isis_nexthop_table_t *table, const route_nhobj_key_t *key, const isis_nexthop_value_t *value,
                         uint32_t *id_out)
{
    if (!table || !key || !value || !id_out || !table->by_key || !table->by_id)
    {
        return ERRCODE_FAIL;
    }

    isis_nexthop_entry_t *entry = (isis_nexthop_entry_t *)g_hash_table_lookup(table->by_key, key);
    if (entry)
    {
        entry->refcount++;
        entry->value = *value;
        *id_out = entry->id;
        return ERRCODE_SUCCESS;
    }

    /* 首次申请：want_id=0，由 ROUTE 分配 id 并返回 */
    uint32_t id = 0u;
    if (isis_nh_route_acquire(key, value, 0u, &id) != ERRCODE_SUCCESS || id == 0u)
    {
        return ERRCODE_FAIL;
    }

    entry = g_malloc0(sizeof(*entry));
    if (!entry)
    {
        (void)route_rpc_nhobj_release(isis_local_ipc_ctx(), id);
        return ERRCODE_FAIL;
    }

    entry->key = *key;
    entry->value = *value;
    entry->id = id;
    entry->refcount = 1u;
    g_hash_table_insert(table->by_key, &entry->key, entry);
    g_hash_table_insert(table->by_id, GUINT_TO_POINTER(id), entry);

    *id_out = id;
    return ERRCODE_SUCCESS;
}

int isis_nexthop_retain(isis_nexthop_table_t *table, uint32_t id)
{
    if (!table || id == 0u || !table->by_id)
    {
        return ERRCODE_FAIL;
    }

    isis_nexthop_entry_t *entry = (isis_nexthop_entry_t *)g_hash_table_lookup(table->by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    entry->refcount++;
    return ERRCODE_SUCCESS;
}

void isis_nexthop_release(isis_nexthop_table_t *table, uint32_t id)
{
    if (!table || id == 0u || !table->by_id || !table->by_key)
    {
        return;
    }

    isis_nexthop_entry_t *entry = (isis_nexthop_entry_t *)g_hash_table_lookup(table->by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return;
    }

    if (entry->refcount > 1u)
    {
        entry->refcount--;
        return;
    }

    (void)route_rpc_nhobj_release(isis_local_ipc_ctx(), id);
    g_hash_table_remove(table->by_id, GUINT_TO_POINTER(id));
    g_hash_table_remove(table->by_key, &entry->key);
}

int isis_nexthop_lookup(isis_nexthop_table_t *table, uint32_t id, route_nhobj_key_t *key_out)
{
    if (!table || id == 0u || !key_out || !table->by_id)
    {
        return ERRCODE_FAIL;
    }

    isis_nexthop_entry_t *entry = (isis_nexthop_entry_t *)g_hash_table_lookup(table->by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    *key_out = entry->key;
    return ERRCODE_SUCCESS;
}

int isis_nexthop_get_value(isis_nexthop_table_t *table, uint32_t id, isis_nexthop_value_t *value_out)
{
    if (!table || id == 0u || !value_out || !table->by_id)
    {
        return ERRCODE_FAIL;
    }

    isis_nexthop_entry_t *entry = (isis_nexthop_entry_t *)g_hash_table_lookup(table->by_id, GUINT_TO_POINTER(id));
    if (!entry)
    {
        return ERRCODE_FAIL;
    }

    *value_out = entry->value;
    return ERRCODE_SUCCESS;
}

uint32_t isis_nexthop_table_resync(isis_nexthop_table_t *table)
{
    if (!table || !table->by_key)
    {
        return 0u;
    }

    uint32_t pushed = 0u;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, table->by_key);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        isis_nexthop_entry_t *entry = (isis_nexthop_entry_t *)value;
        if (!entry || entry->id == 0u)
        {
            continue;
        }
        /* ROUTE 重启后按原 id 反刷恢复对象 */
        if (isis_nh_route_acquire(&entry->key, &entry->value, entry->id, NULL) == ERRCODE_SUCCESS)
        {
            pushed++;
        }
        else
        {
            LOG_WARN("ISIS: nexthop resync push failed id=%u", entry->id);
        }
    }
    return pushed;
}
