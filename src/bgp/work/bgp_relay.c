/**
 * @file   bgp_relay.c
 * @brief  BGP nexthop relay：维护 nexthop 与路由关系，并消费 ROUTE nexthop 回调
 */
#include "bgp_relay.h"

#include <string.h>
#include <sys/socket.h>

#include "bgp_calc.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_protocol.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"

typedef struct bgp_relay_route_key
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t safi;
    uint8_t prefix_len;
    net_addr_t prefix_addr;
    net_addr_t source_addr;
} bgp_relay_route_key_t;

typedef struct bgp_relay_nh_key
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t safi;
    uint8_t _pad0;
    net_addr_t nexthop_addr;
} bgp_relay_nh_key_t;

typedef struct bgp_relay_route
{
    bgp_relay_route_key_t key;
    bgp_relay_nh_key_t nh_key;
    bgp_attr_t attr;
    bgp_nexthop_t nexthop;
    uint8_t valid;
    uint8_t _pad[7];
    gint64 updated_at_usec;
} bgp_relay_route_t;

typedef struct bgp_relay_nh_watch
{
    bgp_relay_nh_key_t key;
    uint8_t resolved;
    uint8_t _pad[7];
    gint64 updated_at_usec;
    GHashTable *route_key_set; /* bgp_relay_route_key_t* -> bgp_relay_route_key_t* */
} bgp_relay_nh_watch_t;

/* key 指向 value 内嵌 key，key_destroy=NULL */
static GHashTable *g_bgp_relay_route_table = NULL; /* bgp_relay_route_key_t* -> bgp_relay_route_t* */
static GHashTable *g_bgp_relay_nh_table = NULL;    /* bgp_relay_nh_key_t* -> bgp_relay_nh_watch_t* */

static guint bgp_relay_route_key_hash(gconstpointer p)
{
    const bgp_relay_route_key_t *k = (const bgp_relay_route_key_t *)p;
    if (!k)
    {
        return 0;
    }
    guint h = (guint)k->vrf_id;
    h = h * 33u + (guint)k->afi;
    h = h * 33u + (guint)k->safi;
    h = h * 33u + (guint)k->prefix_len;
    h ^= net_addr_hash(&k->prefix_addr);
    h ^= (net_addr_hash(&k->source_addr) * 131u);
    return h;
}

static gboolean bgp_relay_route_key_equal(gconstpointer a, gconstpointer b)
{
    const bgp_relay_route_key_t *ka = (const bgp_relay_route_key_t *)a;
    const bgp_relay_route_key_t *kb = (const bgp_relay_route_key_t *)b;
    if (!ka || !kb)
    {
        return FALSE;
    }
    return ka->vrf_id == kb->vrf_id && ka->afi == kb->afi && ka->safi == kb->safi && ka->prefix_len == kb->prefix_len &&
           net_addr_equal(&ka->prefix_addr, &kb->prefix_addr) && net_addr_equal(&ka->source_addr, &kb->source_addr);
}

static guint bgp_relay_nh_key_hash(gconstpointer p)
{
    const bgp_relay_nh_key_t *k = (const bgp_relay_nh_key_t *)p;
    if (!k)
    {
        return 0;
    }
    guint h = (guint)k->vrf_id;
    h = h * 33u + (guint)k->afi;
    h = h * 33u + (guint)k->safi;
    h ^= net_addr_hash(&k->nexthop_addr);
    return h;
}

static gboolean bgp_relay_nh_key_equal(gconstpointer a, gconstpointer b)
{
    const bgp_relay_nh_key_t *ka = (const bgp_relay_nh_key_t *)a;
    const bgp_relay_nh_key_t *kb = (const bgp_relay_nh_key_t *)b;
    if (!ka || !kb)
    {
        return FALSE;
    }
    return ka->vrf_id == kb->vrf_id && ka->afi == kb->afi && ka->safi == kb->safi &&
           net_addr_equal(&ka->nexthop_addr, &kb->nexthop_addr);
}

static void bgp_relay_nh_watch_destroy(gpointer p)
{
    bgp_relay_nh_watch_t *watch = (bgp_relay_nh_watch_t *)p;
    if (!watch)
    {
        return;
    }
    if (watch->route_key_set)
    {
        g_hash_table_destroy(watch->route_key_set);
        watch->route_key_set = NULL;
    }
    g_free(watch);
}

static void bgp_relay_tables_ensure(void)
{
    if (!g_bgp_relay_route_table)
    {
        g_bgp_relay_route_table =
            g_hash_table_new_full(bgp_relay_route_key_hash, bgp_relay_route_key_equal, NULL, g_free);
    }
    if (!g_bgp_relay_nh_table)
    {
        g_bgp_relay_nh_table =
            g_hash_table_new_full(bgp_relay_nh_key_hash, bgp_relay_nh_key_equal, NULL, bgp_relay_nh_watch_destroy);
    }
}

static int bgp_nlri_to_route_prefix(const bgp_nlri_entry_t *nlri, uint16_t *afi_out, uint8_t *prefix_len_out,
                                    net_addr_t *prefix_addr_out)
{
    if (!nlri || !afi_out || !prefix_len_out || !prefix_addr_out)
    {
        return 0;
    }
    if (nlri->type != BGP_NLRI_PREFIX || nlri->safi != BGP_SAFI_UNICAST)
    {
        return 0;
    }

    if (nlri->afi == BGP_AFI_IPV4)
    {
        if (nlri->prefix.prefix.addr.family != AF_INET || nlri->prefix.prefix.prefix_len > 32)
        {
            return 0;
        }
        *afi_out = ROUTE_AFI_IPV4;
    }
    else if (nlri->afi == BGP_AFI_IPV6)
    {
        if (nlri->prefix.prefix.addr.family != AF_INET6 || nlri->prefix.prefix.prefix_len > 128)
        {
            return 0;
        }
        *afi_out = ROUTE_AFI_IPV6;
    }
    else
    {
        return 0;
    }

    *prefix_len_out = nlri->prefix.prefix.prefix_len;
    *prefix_addr_out = nlri->prefix.prefix.addr;
    return 1;
}

static void bgp_relay_make_route_key(bgp_relay_route_key_t *key, uint32_t vrf_id, uint16_t afi, uint8_t safi,
                                     uint8_t prefix_len, const net_addr_t *prefix_addr, const net_addr_t *source_addr)
{
    memset(key, 0, sizeof(*key));
    key->vrf_id = vrf_id;
    key->afi = afi;
    key->safi = safi;
    key->prefix_len = prefix_len;
    key->prefix_addr = *prefix_addr;
    key->source_addr = *source_addr;
}

static void bgp_relay_make_nh_key(bgp_relay_nh_key_t *key, uint32_t vrf_id, uint16_t afi, uint8_t safi,
                                  const net_addr_t *nexthop_addr)
{
    memset(key, 0, sizeof(*key));
    key->vrf_id = vrf_id;
    key->afi = afi;
    key->safi = safi;
    key->nexthop_addr = *nexthop_addr;
}

static bgp_instance_t *bgp_relay_lookup_instance(const bgp_relay_route_key_t *key)
{
    if (!key || !g_bgp_work_local->protocol)
    {
        return NULL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, key->vrf_id);
    if (!vrf)
    {
        return NULL;
    }

    bgp_afi_t afi = (bgp_afi_t)key->afi;
    bgp_safi_t safi = (key->safi == 0) ? BGP_SAFI_UNICAST : (bgp_safi_t)key->safi;
    return (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));
}

static void bgp_relay_build_nlri(const bgp_relay_route_key_t *key, bgp_nlri_entry_t *nlri)
{
    memset(nlri, 0, sizeof(*nlri));
    nlri->afi = key->afi;
    nlri->safi = key->safi;
    nlri->type = BGP_NLRI_PREFIX;
    nlri->prefix.prefix.prefix_len = key->prefix_len;
    nlri->prefix.prefix.addr = key->prefix_addr;
    nlri->prefix.has_rd = false;
    nlri->prefix.has_label = false;
}

static int bgp_relay_reach_route_to_rib(const bgp_relay_route_t *route)
{
    if (!route)
    {
        return -1;
    }

    bgp_instance_t *inst = bgp_relay_lookup_instance(&route->key);
    if (!inst)
    {
        return -1;
    }

    bgp_nlri_entry_t nlri;
    bgp_relay_build_nlri(&route->key, &nlri);
    net_addr_t src = route->key.source_addr;

    int rc_reach = bgp_rib_reach_one(inst->rib, &nlri, &src, 0, &route->attr, &route->nexthop);
    if (rc_reach < 0)
    {
        return rc_reach;
    }

    int rc_valid = bgp_rib_set_route_valid(inst->rib, &nlri, &src, route->valid ? TRUE : FALSE);
    if (rc_valid < 0)
    {
        return rc_valid;
    }

    if (inst->calc_queue && (rc_reach > 0 || rc_valid > 0))
    {
        bgp_calc_queue_push(inst->calc_queue, &nlri);
    }
    return (rc_reach > 0 || rc_valid > 0) ? 1 : 0;
}

static int bgp_relay_withdraw_route_from_rib(const bgp_relay_route_t *route)
{
    if (!route)
    {
        return -1;
    }

    bgp_instance_t *inst = bgp_relay_lookup_instance(&route->key);
    if (!inst)
    {
        return -1;
    }

    bgp_nlri_entry_t nlri;
    bgp_relay_build_nlri(&route->key, &nlri);
    net_addr_t src = route->key.source_addr;

    int rc = bgp_rib_unreach_one(inst->rib, &nlri, &src);
    if (rc == 1 && inst->calc_queue)
    {
        bgp_calc_queue_push(inst->calc_queue, &nlri);
    }
    return rc;
}

static int bgp_relay_set_route_valid(const bgp_relay_route_t *route, gboolean valid)
{
    if (!route)
    {
        return -1;
    }

    bgp_instance_t *inst = bgp_relay_lookup_instance(&route->key);
    if (!inst)
    {
        return -1;
    }

    bgp_nlri_entry_t nlri;
    bgp_relay_build_nlri(&route->key, &nlri);
    net_addr_t src = route->key.source_addr;

    int rc = bgp_rib_set_route_valid(inst->rib, &nlri, &src, valid);
    if (rc > 0 && inst->calc_queue)
    {
        bgp_calc_queue_push(inst->calc_queue, &nlri);
    }
    return rc;
}

static bgp_relay_nh_watch_t *bgp_relay_nh_watch_lookup(const bgp_relay_nh_key_t *key)
{
    if (!g_bgp_relay_nh_table || !key)
    {
        return NULL;
    }
    return (bgp_relay_nh_watch_t *)g_hash_table_lookup(g_bgp_relay_nh_table, key);
}

static int bgp_relay_nh_watch_add_route(bgp_relay_nh_watch_t *watch, const bgp_relay_route_key_t *route_key)
{
    if (!watch || !watch->route_key_set || !route_key)
    {
        return ERRCODE_FAIL;
    }

    if (g_hash_table_lookup(watch->route_key_set, route_key))
    {
        return ERRCODE_SUCCESS;
    }

    bgp_relay_route_key_t *copy = (bgp_relay_route_key_t *)g_malloc(sizeof(*copy));
    if (!copy)
    {
        return ERRCODE_FAIL;
    }
    *copy = *route_key;
    g_hash_table_insert(watch->route_key_set, copy, copy);
    return ERRCODE_SUCCESS;
}

static void bgp_relay_fill_nh_iter_req(route_nh_iter_req_t *req, const bgp_relay_nh_key_t *key)
{
    if (!req || !key)
    {
        return;
    }

    memset(req, 0, sizeof(*req));
    req->vrf_id = key->vrf_id;
    req->afi = key->afi;
    req->safi = key->safi;
    req->nexthop_addr = key->nexthop_addr;
}

static void bgp_relay_nh_watch_remove_if_empty(const bgp_relay_nh_watch_t *watch)
{
    if (!watch || !watch->route_key_set || g_hash_table_size(watch->route_key_set) > 0)
    {
        return;
    }

    route_nh_iter_req_t req;
    bgp_relay_fill_nh_iter_req(&req, &watch->key);
    (void)route_rpc_nh_unregister(g_bgp_local->dev_ipc_ctx, &req);

    g_hash_table_remove(g_bgp_relay_nh_table, &watch->key);
}

static void bgp_relay_detach_route_from_watch(const bgp_relay_route_t *route)
{
    if (!route || !g_bgp_relay_nh_table)
    {
        return;
    }

    bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(&route->nh_key);
    if (!watch || !watch->route_key_set)
    {
        return;
    }

    g_hash_table_remove(watch->route_key_set, &route->key);
    bgp_relay_nh_watch_remove_if_empty(watch);
}

static bgp_relay_nh_watch_t *bgp_relay_attach_route_to_watch(const bgp_relay_route_t *route)
{
    if (!route)
    {
        return NULL;
    }
    bgp_relay_tables_ensure();
    if (!g_bgp_relay_nh_table)
    {
        return NULL;
    }

    bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(&route->nh_key);
    if (!watch)
    {
        watch = (bgp_relay_nh_watch_t *)g_malloc0(sizeof(*watch));
        if (!watch)
        {
            return NULL;
        }
        watch->key = route->nh_key;
        watch->resolved = 0u;
        watch->updated_at_usec = g_get_real_time();
        watch->route_key_set = g_hash_table_new_full(bgp_relay_route_key_hash, bgp_relay_route_key_equal, g_free, NULL);
        if (!watch->route_key_set)
        {
            g_free(watch);
            return NULL;
        }

        route_nh_iter_req_t req;
        bgp_relay_fill_nh_iter_req(&req, &watch->key);

        if (route_rpc_nh_register(g_bgp_local->dev_ipc_ctx, &req) != ERRCODE_SUCCESS)
        {
            g_hash_table_destroy(watch->route_key_set);
            g_free(watch);
            return NULL;
        }

        g_hash_table_insert(g_bgp_relay_nh_table, &watch->key, watch);
    }

    if (bgp_relay_nh_watch_add_route(watch, &route->key) != ERRCODE_SUCCESS)
    {
        if (watch->route_key_set && g_hash_table_size(watch->route_key_set) == 0)
        {
            bgp_relay_nh_watch_remove_if_empty(watch);
        }
        return NULL;
    }

    watch->updated_at_usec = g_get_real_time();
    return watch;
}

static int bgp_relay_route_remove_by_key(const bgp_relay_route_key_t *key)
{
    if (!g_bgp_relay_route_table || !key)
    {
        return 0;
    }

    bgp_relay_route_t *route = (bgp_relay_route_t *)g_hash_table_lookup(g_bgp_relay_route_table, key);
    if (!route)
    {
        return 0;
    }

    route->valid = 0u;
    (void)bgp_relay_withdraw_route_from_rib(route);

    bgp_relay_detach_route_from_watch(route);
    g_hash_table_remove(g_bgp_relay_route_table, key);
    return 1;
}

static int bgp_relay_route_upsert(const bgp_relay_route_key_t *key, const bgp_attr_t *attr,
                                  const bgp_nexthop_t *nexthop)
{
    if (!key || !attr || !nexthop)
    {
        return ERRCODE_FAIL;
    }
    if (nexthop->global.family == 0)
    {
        return ERRCODE_FAIL;
    }

    bgp_relay_tables_ensure();
    if (!g_bgp_relay_route_table || !g_bgp_relay_nh_table)
    {
        return ERRCODE_FAIL;
    }

    bgp_relay_route_t *route = (bgp_relay_route_t *)g_hash_table_lookup(g_bgp_relay_route_table, key);
    gboolean is_new = FALSE;
    if (!route)
    {
        route = (bgp_relay_route_t *)g_malloc0(sizeof(*route));
        if (!route)
        {
            return ERRCODE_FAIL;
        }
        route->key = *key;
        g_hash_table_insert(g_bgp_relay_route_table, &route->key, route);
        is_new = TRUE;
    }

    bgp_relay_nh_key_t new_nh_key;
    bgp_relay_make_nh_key(&new_nh_key, key->vrf_id, key->afi, key->safi, &nexthop->global);

    if (!is_new && !bgp_relay_nh_key_equal(&route->nh_key, &new_nh_key))
    {
        bgp_relay_detach_route_from_watch(route);
    }

    route->attr = *attr;
    route->nexthop = *nexthop;
    route->nh_key = new_nh_key;
    route->updated_at_usec = g_get_real_time();

    bgp_relay_nh_watch_t *watch = bgp_relay_attach_route_to_watch(route);
    if (!watch)
    {
        if (is_new)
        {
            g_hash_table_remove(g_bgp_relay_route_table, &route->key);
        }
        return ERRCODE_FAIL;
    }

    route->valid = watch->resolved ? 1u : 0u;
    if (bgp_relay_reach_route_to_rib(route) < 0)
    {
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

void bgp_relay_init(void)
{
    bgp_relay_tables_ensure();
}

void bgp_relay_cleanup(void)
{
    if (g_bgp_relay_route_table)
    {
        g_hash_table_destroy(g_bgp_relay_route_table);
        g_bgp_relay_route_table = NULL;
    }
    if (g_bgp_relay_nh_table)
    {
        g_hash_table_destroy(g_bgp_relay_nh_table);
        g_bgp_relay_nh_table = NULL;
    }
}

void bgp_relay_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd,
                                  bgp_peer_update_ingest_stats_t *stats)
{
    if (stats)
    {
        memset(stats, 0, sizeof(*stats));
    }
    if (!session || !session->vrf || !upd)
    {
        return;
    }

    uint32_t vrf_id = session->vrf->vrf_id;

    for (uint32_t i = 0; i < upd->reach_len; ++i)
    {
        const bgp_nlri_entry_t *nlri = &upd->reach[i];
        uint16_t afi = 0;
        uint8_t prefix_len = 0;
        net_addr_t prefix_addr;
        memset(&prefix_addr, 0, sizeof(prefix_addr));
        if (!bgp_nlri_to_route_prefix(nlri, &afi, &prefix_len, &prefix_addr))
        {
            continue;
        }

        if (upd->nexthop.global.family != prefix_addr.family)
        {
            if (stats)
            {
                stats->reach_failed++;
            }
            continue;
        }

        bgp_relay_route_key_t key;
        bgp_relay_make_route_key(&key, vrf_id, (uint16_t)nlri->afi, (uint8_t)nlri->safi, prefix_len, &prefix_addr,
                                 &session->neighbor_addr);

        if (bgp_relay_route_upsert(&key, &upd->attr, &upd->nexthop) == ERRCODE_SUCCESS)
        {
            if (stats)
            {
                stats->reach_injected++;
            }
        }
        else if (stats)
        {
            stats->reach_failed++;
        }
    }

    for (uint32_t i = 0; i < upd->unreach_len; ++i)
    {
        const bgp_nlri_entry_t *nlri = &upd->unreach[i];
        uint16_t afi = 0;
        uint8_t prefix_len = 0;
        net_addr_t prefix_addr;
        memset(&prefix_addr, 0, sizeof(prefix_addr));
        if (!bgp_nlri_to_route_prefix(nlri, &afi, &prefix_len, &prefix_addr))
        {
            continue;
        }

        bgp_relay_route_key_t key;
        bgp_relay_make_route_key(&key, vrf_id, (uint16_t)nlri->afi, (uint8_t)nlri->safi, prefix_len, &prefix_addr,
                                 &session->neighbor_addr);
        (void)bgp_relay_route_remove_by_key(&key);

        if (stats)
        {
            stats->unreach_injected++;
        }
    }
}

void bgp_relay_flush_peer_routes(uint32_t vrf_id, const net_addr_t *source)
{
    if (!source || !g_bgp_relay_route_table)
    {
        return;
    }

    GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
    if (!keys)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key_ptr = NULL;
    gpointer val_ptr = NULL;
    g_hash_table_iter_init(&iter, g_bgp_relay_route_table);
    while (g_hash_table_iter_next(&iter, &key_ptr, &val_ptr))
    {
        (void)val_ptr;
        const bgp_relay_route_key_t *key = (const bgp_relay_route_key_t *)key_ptr;
        if (!key)
        {
            continue;
        }
        if (key->vrf_id != vrf_id || !net_addr_equal(&key->source_addr, source))
        {
            continue;
        }

        bgp_relay_route_key_t *copy = (bgp_relay_route_key_t *)g_malloc(sizeof(*copy));
        if (!copy)
        {
            continue;
        }
        *copy = *key;
        g_ptr_array_add(keys, copy);
    }

    for (guint i = 0; i < keys->len; ++i)
    {
        bgp_relay_route_key_t *key = (bgp_relay_route_key_t *)g_ptr_array_index(keys, i);
        if (!key)
        {
            continue;
        }
        (void)bgp_relay_route_remove_by_key(key);
    }

    g_ptr_array_free(keys, TRUE);
}

uint32_t bgp_relay_handle_nh_notify(const route_nh_iter_notify_t *notify)
{
    if (!notify || !g_bgp_relay_nh_table || !g_bgp_relay_route_table)
    {
        return 0;
    }
    if (notify->safi != 0 && notify->safi != BGP_SAFI_UNICAST)
    {
        return 0;
    }

    bgp_relay_nh_key_t key;
    bgp_relay_make_nh_key(&key, notify->vrf_id, notify->afi, (notify->safi == 0) ? BGP_SAFI_UNICAST : notify->safi,
                          &notify->relay_addr);

    bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(&key);
    if (!watch)
    {
        return 0;
    }

    uint8_t new_state = notify->resolved ? 1u : 0u;
    if (watch->resolved == new_state)
    {
        return 0;
    }

    watch->resolved = new_state;
    watch->updated_at_usec = g_get_real_time();

    uint32_t touched = 0;
    GHashTableIter iter;
    gpointer route_key_ptr = NULL;
    gpointer route_val_ptr = NULL;
    g_hash_table_iter_init(&iter, watch->route_key_set);
    while (g_hash_table_iter_next(&iter, &route_key_ptr, &route_val_ptr))
    {
        (void)route_val_ptr;
        const bgp_relay_route_key_t *route_key = (const bgp_relay_route_key_t *)route_key_ptr;
        bgp_relay_route_t *route = (bgp_relay_route_t *)g_hash_table_lookup(g_bgp_relay_route_table, route_key);
        if (!route)
        {
            continue;
        }

        if (watch->resolved)
        {
            route->valid = 1u;
            if (bgp_relay_set_route_valid(route, TRUE) > 0)
            {
                touched++;
            }
            continue;
        }

        route->valid = 0u;
        if (bgp_relay_set_route_valid(route, FALSE) > 0)
        {
            touched++;
        }
    }

    return touched;
}
