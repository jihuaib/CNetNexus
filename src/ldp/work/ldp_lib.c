/**
 * @file   ldp_lib.c
 * @brief  LDP LIB 实现
 *
 * M4 内部使用简单 label 计数器（16 起步），M5 起改为通过 TUNNEL RPC 申请。
 *
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_lib.h"

#include <string.h>

#include "log.h"

#define LDP_LOCAL_LABEL_MIN 16u
#define LDP_LOCAL_LABEL_MAX 1048575u

static GHashTable *g_local = NULL;  /* key=ldp_fec_t* (g_malloc) → ldp_local_label_t* */
static GHashTable *g_remote = NULL; /* key=remote-key blob → ldp_remote_label_t* */
static uint32_t g_next_label = LDP_LOCAL_LABEL_MIN;

typedef struct remote_key
{
    uint32_t peer_lsr_id;
    uint16_t peer_label_space;
    uint16_t _pad;
    uint32_t prefix;
    uint8_t prefix_len;
    uint8_t _pad2[3];
} remote_key_t;

static guint fec_hash(gconstpointer key)
{
    const ldp_fec_t *f = (const ldp_fec_t *)key;
    return (guint)(f->prefix * 31u + (uint32_t)f->prefix_len);
}

static gboolean fec_equal(gconstpointer a, gconstpointer b)
{
    const ldp_fec_t *fa = (const ldp_fec_t *)a;
    const ldp_fec_t *fb = (const ldp_fec_t *)b;
    return fa->prefix == fb->prefix && fa->prefix_len == fb->prefix_len;
}

static guint remote_hash(gconstpointer key)
{
    const remote_key_t *k = (const remote_key_t *)key;
    guint h = (guint)k->peer_lsr_id;
    h = h * 31u + (guint)k->peer_label_space;
    h = h * 31u + (guint)k->prefix;
    h = h * 31u + (guint)k->prefix_len;
    return h;
}

static gboolean remote_equal(gconstpointer a, gconstpointer b)
{
    const remote_key_t *ka = (const remote_key_t *)a;
    const remote_key_t *kb = (const remote_key_t *)b;
    return ka->peer_lsr_id == kb->peer_lsr_id && ka->peer_label_space == kb->peer_label_space &&
           ka->prefix == kb->prefix && ka->prefix_len == kb->prefix_len;
}

static remote_key_t make_remote_key(uint32_t peer, uint16_t space, const ldp_fec_t *fec)
{
    remote_key_t k;
    memset(&k, 0, sizeof(k));
    k.peer_lsr_id = peer;
    k.peer_label_space = space;
    k.prefix = fec->prefix;
    k.prefix_len = fec->prefix_len;
    return k;
}

void ldp_lib_init(void)
{
    if (!g_local)
    {
        g_local = g_hash_table_new_full(fec_hash, fec_equal, g_free, g_free);
    }
    if (!g_remote)
    {
        g_remote = g_hash_table_new_full(remote_hash, remote_equal, g_free, g_free);
    }
}

void ldp_lib_cleanup(void)
{
    if (g_local)
    {
        g_hash_table_destroy(g_local);
        g_local = NULL;
    }
    if (g_remote)
    {
        g_hash_table_destroy(g_remote);
        g_remote = NULL;
    }
    g_next_label = LDP_LOCAL_LABEL_MIN;
}

void ldp_lib_set_local_label(const ldp_fec_t *fec, uint32_t label)
{
    if (!fec || !g_local)
    {
        return;
    }
    ldp_local_label_t *e = (ldp_local_label_t *)g_hash_table_lookup(g_local, fec);
    if (e)
    {
        e->label = label;
        return;
    }
    ldp_fec_t *kheap = g_malloc(sizeof(*kheap));
    *kheap = *fec;
    e = g_malloc0(sizeof(*e));
    e->fec = *fec;
    e->label = label;
    g_hash_table_insert(g_local, kheap, e);
}

uint32_t ldp_lib_alloc_local_label(const ldp_fec_t *fec)
{
    if (!fec || !g_local)
    {
        return 0u;
    }
    ldp_local_label_t *e = (ldp_local_label_t *)g_hash_table_lookup(g_local, fec);
    if (e)
    {
        return e->label;
    }
    if (g_next_label > LDP_LOCAL_LABEL_MAX)
    {
        LOG_ERROR("LDP: local label space exhausted");
        return 0u;
    }
    ldp_fec_t *kheap = g_malloc(sizeof(*kheap));
    *kheap = *fec;
    e = g_malloc0(sizeof(*e));
    e->fec = *fec;
    e->label = g_next_label++;
    g_hash_table_insert(g_local, kheap, e);
    return e->label;
}

void ldp_lib_free_local_label(const ldp_fec_t *fec)
{
    if (!fec || !g_local)
    {
        return;
    }
    g_hash_table_remove(g_local, fec);
}

const ldp_local_label_t *ldp_lib_lookup_local(const ldp_fec_t *fec)
{
    if (!fec || !g_local)
    {
        return NULL;
    }
    return (const ldp_local_label_t *)g_hash_table_lookup(g_local, fec);
}

GHashTable *ldp_lib_local_table(void)
{
    return g_local;
}

void ldp_lib_set_remote(uint32_t peer, uint16_t space, const ldp_fec_t *fec, uint32_t label)
{
    if (!fec || !g_remote)
    {
        return;
    }
    remote_key_t key = make_remote_key(peer, space, fec);
    ldp_remote_label_t *e = (ldp_remote_label_t *)g_hash_table_lookup(g_remote, &key);
    if (e)
    {
        e->label = label;
        return;
    }
    remote_key_t *kheap = g_malloc(sizeof(*kheap));
    *kheap = key;
    e = g_malloc0(sizeof(*e));
    e->peer_lsr_id = peer;
    e->peer_label_space = space;
    e->fec = *fec;
    e->label = label;
    g_hash_table_insert(g_remote, kheap, e);
}

void ldp_lib_del_remote(uint32_t peer, uint16_t space, const ldp_fec_t *fec)
{
    if (!fec || !g_remote)
    {
        return;
    }
    remote_key_t key = make_remote_key(peer, space, fec);
    g_hash_table_remove(g_remote, &key);
}

void ldp_lib_purge_peer(uint32_t peer, uint16_t space)
{
    if (!g_remote)
    {
        return;
    }
    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    GList *to_drop = NULL;
    g_hash_table_iter_init(&it, g_remote);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        ldp_remote_label_t *e = (ldp_remote_label_t *)val;
        if (e && e->peer_lsr_id == peer && e->peer_label_space == space)
        {
            to_drop = g_list_prepend(to_drop, key);
        }
    }
    for (GList *l = to_drop; l; l = l->next)
    {
        g_hash_table_remove(g_remote, l->data);
    }
    g_list_free(to_drop);
}

GHashTable *ldp_lib_remote_table(void)
{
    return g_remote;
}
