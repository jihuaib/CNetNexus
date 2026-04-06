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

typedef struct bgp_relay_nh_key
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t safi;
    uint8_t _pad0;
    net_addr_t nexthop_addr;
} bgp_relay_nh_key_t;

typedef struct bgp_relay_nh_watch
{
    bgp_relay_nh_key_t key;
    uint8_t resolved;
    uint8_t _pad[7];
    uint32_t out_ifindex;
    net_addr_t relay_addr;
    gint64 updated_at_usec;
    GList *route_list; /* bgp_route_node_t* 双挂（同时在 rthead->route_list） */
} bgp_relay_nh_watch_t;

static GHashTable *g_bgp_relay_nh_table = NULL; /* bgp_relay_nh_key_t* -> bgp_relay_nh_watch_t* */

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
    if (watch->route_list)
    {
        g_list_free(watch->route_list);
        watch->route_list = NULL;
    }
    g_free(watch);
}

static void bgp_relay_tables_ensure(void)
{
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

static void bgp_relay_make_nh_key(bgp_relay_nh_key_t *key, uint32_t vrf_id, uint16_t afi, uint8_t safi,
                                  const net_addr_t *nexthop_addr)
{
    memset(key, 0, sizeof(*key));
    key->vrf_id = vrf_id;
    key->afi = afi;
    key->safi = safi;
    key->nexthop_addr = *nexthop_addr;
}

static bgp_instance_t *bgp_relay_lookup_instance(uint32_t vrf_id, uint16_t afi, uint8_t safi)
{
    if (!g_bgp_work_local->protocol)
    {
        return NULL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, vrf_id);
    if (!vrf)
    {
        return NULL;
    }

    bgp_afi_t afi_key = (bgp_afi_t)afi;
    bgp_safi_t safi_key = (safi == 0) ? BGP_SAFI_UNICAST : (bgp_safi_t)safi;
    return (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi_key, safi_key));
}

static int bgp_relay_build_nh_key_from_route(const bgp_route_node_t *route, bgp_relay_nh_key_t *nh_key_out)
{
    if (!route || !route->head || !route->head->inst || !route->head->inst->vrf || !nh_key_out ||
        route->nexthop.global.family == 0)
    {
        return 0;
    }

    bgp_relay_make_nh_key(nh_key_out, route->head->inst->vrf->vrf_id, route->head->nlri.afi, route->head->nlri.safi,
                          &route->nexthop.global);
    return 1;
}

static void bgp_relay_trigger_calc(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !inst->calc_queue || !nlri)
    {
        return;
    }

    bgp_calc_queue_push(inst->calc_queue, inst, nlri);
}

static int bgp_relay_reach_route_to_rib(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri, const net_addr_t *source,
                                        const bgp_attr_t *attr, const bgp_nexthop_t *nexthop,
                                        bgp_route_node_t **route_out, gboolean *is_new_out)
{
    if (is_new_out)
    {
        *is_new_out = FALSE;
    }
    if (route_out)
    {
        *route_out = NULL;
    }
    if (!inst || !inst->rib || !nlri || !source || source->family == 0 || !attr || !nexthop)
    {
        return -1;
    }

    bgp_rthead_t *head = bgp_rib_ensure_head(inst->rib, nlri);
    if (!head)
    {
        return -1;
    }

    bgp_route_node_t *route = bgp_rthead_lookup_route_mut(head, source);
    gboolean is_new = FALSE;
    if (!route)
    {
        route = bgp_rthead_create_route(inst->rib, head, source);
        if (!route)
        {
            return -1;
        }
        is_new = TRUE;
    }

    if (bgp_rib_route_apply_reach(route, 0, attr, nexthop) != 0)
    {
        return -1;
    }

    if (bgp_rib_set_route_valid(inst->rib, nlri, source, FALSE) < 0)
    {
        return -1;
    }

    if (route_out)
    {
        *route_out = route;
    }
    if (is_new_out)
    {
        *is_new_out = is_new;
    }
    return is_new ? 1 : 0;
}

static int bgp_relay_withdraw_route_from_rib(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri,
                                             const net_addr_t *source)
{
    if (!inst || !inst->rib || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    int rc = bgp_rib_unreach_one(inst->rib, nlri, source);
    if (rc == 1 && inst->calc_queue)
    {
        bgp_calc_queue_push(inst->calc_queue, inst, nlri);
    }
    return rc;
}

static int bgp_relay_set_route_valid(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri, const net_addr_t *source,
                                     gboolean valid)
{
    if (!inst || !inst->rib || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    int rc = bgp_rib_set_route_valid(inst->rib, nlri, source, valid);
    if (rc > 0 && inst->calc_queue)
    {
        bgp_calc_queue_push(inst->calc_queue, inst, nlri);
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

static int bgp_relay_nh_watch_add_route(bgp_relay_nh_watch_t *watch, bgp_route_node_t *route)
{
    if (!watch || !route)
    {
        return ERRCODE_FAIL;
    }

    if (g_list_find(watch->route_list, route))
    {
        return ERRCODE_SUCCESS;
    }

    GList *new_list = g_list_append(watch->route_list, route);
    if (!new_list)
    {
        return ERRCODE_FAIL;
    }
    watch->route_list = new_list;
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
    if (!watch || watch->route_list)
    {
        return;
    }

    route_nh_iter_req_t req;
    bgp_relay_fill_nh_iter_req(&req, &watch->key);
    (void)route_rpc_nh_unregister(g_bgp_local->dev_ipc_ctx, &req);

    g_hash_table_remove(g_bgp_relay_nh_table, &watch->key);
}

static void bgp_relay_route_iter_clear(bgp_route_node_t *route)
{
    if (!route)
    {
        return;
    }

    route->iter_watched = 0u;
    route->iter_resolved = 0u;
    route->iter_out_ifindex = 0u;
    route->iter_relay_addr.family = 0;
}

static void bgp_relay_route_iter_update_from_watch(bgp_route_node_t *route, const bgp_relay_nh_watch_t *watch)
{
    if (!route)
    {
        return;
    }
    if (!watch)
    {
        bgp_relay_route_iter_clear(route);
        return;
    }

    route->iter_watched = 1u;
    route->iter_resolved = watch->resolved ? 1u : 0u;
    route->iter_out_ifindex = watch->out_ifindex;
    route->iter_relay_addr = watch->relay_addr;
}

static void bgp_relay_detach_route_from_watch(bgp_route_node_t *route, const bgp_relay_nh_key_t *nh_key,
                                              gboolean clear_state)
{
    if (!route || !nh_key || !g_bgp_relay_nh_table)
    {
        if (clear_state)
        {
            bgp_relay_route_iter_clear(route);
        }
        return;
    }

    bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(nh_key);
    if (!watch)
    {
        if (clear_state)
        {
            bgp_relay_route_iter_clear(route);
        }
        return;
    }

    watch->route_list = g_list_remove(watch->route_list, route);
    bgp_relay_nh_watch_remove_if_empty(watch);
    if (clear_state)
    {
        bgp_relay_route_iter_clear(route);
    }
}

static bgp_relay_nh_watch_t *bgp_relay_attach_route_to_watch(bgp_route_node_t *route, const bgp_relay_nh_key_t *nh_key)
{
    if (!route || !nh_key)
    {
        return NULL;
    }

    bgp_relay_tables_ensure();
    if (!g_bgp_relay_nh_table)
    {
        return NULL;
    }

    bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(nh_key);
    if (!watch)
    {
        watch = (bgp_relay_nh_watch_t *)g_malloc0(sizeof(*watch));
        if (!watch)
        {
            return NULL;
        }

        watch->key = *nh_key;
        watch->resolved = 0u;
        watch->out_ifindex = 0u;
        watch->relay_addr.family = 0;
        watch->updated_at_usec = g_get_real_time();
        watch->route_list = NULL;

        route_nh_iter_req_t req;
        bgp_relay_fill_nh_iter_req(&req, &watch->key);
        if (route_rpc_nh_register(g_bgp_local->dev_ipc_ctx, &req) != ERRCODE_SUCCESS)
        {
            g_free(watch);
            return NULL;
        }

        g_hash_table_insert(g_bgp_relay_nh_table, &watch->key, watch);
    }

    if (bgp_relay_nh_watch_add_route(watch, route) != ERRCODE_SUCCESS)
    {
        if (!watch->route_list)
        {
            bgp_relay_nh_watch_remove_if_empty(watch);
        }
        return NULL;
    }

    watch->updated_at_usec = g_get_real_time();
    bgp_relay_route_iter_update_from_watch(route, watch);
    return watch;
}

static int bgp_relay_route_remove_from_inst(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri,
                                            const net_addr_t *source)
{
    if (!inst || !inst->rib || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    const bgp_rthead_t *head_ro = bgp_rib_lookup_head(inst->rib, nlri);
    if (!head_ro)
    {
        return 0;
    }

    bgp_route_node_t *route = bgp_rthead_lookup_route_mut((bgp_rthead_t *)head_ro, source);
    if (!route)
    {
        return 0;
    }

    bgp_relay_nh_key_t nh_key;
    gboolean has_nh_key = bgp_relay_build_nh_key_from_route(route, &nh_key) ? TRUE : FALSE;
    if (has_nh_key)
    {
        bgp_relay_detach_route_from_watch(route, &nh_key, TRUE);
    }

    return bgp_relay_withdraw_route_from_rib(inst, nlri, source);
}

static int bgp_relay_route_remove(uint32_t vrf_id, const bgp_nlri_entry_t *nlri, const net_addr_t *source)
{
    if (!nlri || !source || source->family == 0)
    {
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = bgp_relay_lookup_instance(vrf_id, (uint16_t)nlri->afi, (uint8_t)nlri->safi);
    if (!inst)
    {
        return 0;
    }

    int rc = bgp_relay_route_remove_from_inst(inst, nlri, source);
    if (rc < 0)
    {
        return ERRCODE_FAIL;
    }
    return (rc > 0) ? ERRCODE_SUCCESS : 0;
}

static int bgp_relay_route_upsert(uint32_t vrf_id, const bgp_nlri_entry_t *nlri, const net_addr_t *source,
                                  const bgp_attr_t *attr, const bgp_nexthop_t *nexthop)
{
    if (!nlri || !source || source->family == 0 || !attr || !nexthop)
    {
        return ERRCODE_FAIL;
    }
    if (nexthop->global.family == 0)
    {
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = bgp_relay_lookup_instance(vrf_id, (uint16_t)nlri->afi, (uint8_t)nlri->safi);
    if (!inst || !inst->rib)
    {
        return ERRCODE_FAIL;
    }

    const bgp_rthead_t *head_ro = bgp_rib_lookup_head(inst->rib, nlri);
    bgp_route_node_t *old_route = NULL;
    if (head_ro)
    {
        old_route = bgp_rthead_lookup_route_mut((bgp_rthead_t *)head_ro, source);
    }

    uint8_t prev_valid = 0u;
    bgp_relay_nh_key_t old_nh_key;
    memset(&old_nh_key, 0, sizeof(old_nh_key));
    gboolean had_old_nh = FALSE;
    if (old_route)
    {
        prev_valid = BIT_TEST(old_route->flags, BGP_ROUTE_FLAG_VALID) ? 1u : 0u;
        had_old_nh = bgp_relay_build_nh_key_from_route(old_route, &old_nh_key) ? TRUE : FALSE;
    }

    /* 先写 RIB（置 invalid，等待 nexthop 迭代结果）。 */
    bgp_route_node_t *route = NULL;
    if (bgp_relay_reach_route_to_rib(inst, nlri, source, attr, nexthop, &route, NULL) < 0 || !route)
    {
        return ERRCODE_FAIL;
    }

    bgp_relay_nh_key_t new_nh_key;
    if (!bgp_relay_build_nh_key_from_route(route, &new_nh_key))
    {
        if (old_route && had_old_nh)
        {
            bgp_relay_detach_route_from_watch(route, &old_nh_key, TRUE);
        }
        (void)bgp_relay_withdraw_route_from_rib(inst, nlri, source);
        return ERRCODE_FAIL;
    }

    gboolean nh_changed = (old_route && (!had_old_nh || !bgp_relay_nh_key_equal(&old_nh_key, &new_nh_key)));

    bgp_relay_nh_watch_t *watch = bgp_relay_attach_route_to_watch(route, &new_nh_key);
    if (!watch)
    {
        if (old_route && had_old_nh)
        {
            bgp_relay_detach_route_from_watch(route, &old_nh_key, TRUE);
        }
        (void)bgp_relay_withdraw_route_from_rib(inst, nlri, source);
        return ERRCODE_FAIL;
    }

    if (nh_changed && had_old_nh)
    {
        bgp_relay_detach_route_from_watch(route, &old_nh_key, FALSE);
    }

    /* watch 的 resolved 决定 valid，再触发优选。 */
    uint8_t new_valid = watch->resolved ? 1u : 0u;
    int rc_valid = bgp_relay_set_route_valid(inst, nlri, source, new_valid ? TRUE : FALSE);
    if (rc_valid < 0)
    {
        return ERRCODE_FAIL;
    }
    if (rc_valid == 0 && ((prev_valid != new_valid) || watch->resolved))
    {
        bgp_relay_trigger_calc(inst, nlri);
    }

    return ERRCODE_SUCCESS;
}

static gboolean bgp_relay_nexthop_family_compatible(const bgp_session_t *session, const net_addr_t *prefix_addr,
                                                    const bgp_nexthop_t *nexthop)
{
    if (!session || !prefix_addr || !nexthop)
    {
        return FALSE;
    }

    /* 常规场景：前缀与 nexthop 同族。 */
    if (nexthop->global.family == prefix_addr->family)
    {
        return TRUE;
    }

    /* RFC 8950：仅在协商了 Extended Nexthop 时，允许 IPv4 前缀使用 IPv6 nexthop。 */
    if (prefix_addr->family == AF_INET && nexthop->global.family == AF_INET6 &&
        BIT_TEST(session->negotiated_caps, BGP_SESS_CAP_EXT_NEXTHOP))
    {
        return TRUE;
    }

    /* 双栈场景：允许 IPv6 前缀使用 IPv4 nexthop。 */
    if (prefix_addr->family == AF_INET6 && nexthop->global.family == AF_INET)
    {
        return TRUE;
    }

    return FALSE;
}

static void bgp_relay_collect_nlri_cb(const bgp_nlri_entry_t *nlri, gpointer user_data)
{
    if (!nlri || !user_data)
    {
        return;
    }

    GPtrArray *arr = (GPtrArray *)user_data;
    bgp_nlri_entry_t *copy = (bgp_nlri_entry_t *)g_malloc(sizeof(*copy));
    if (!copy)
    {
        return;
    }
    *copy = *nlri;
    g_ptr_array_add(arr, copy);
}

void bgp_relay_init(void)
{
    bgp_relay_tables_ensure();
}

void bgp_relay_cleanup(void)
{
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

        if (!bgp_relay_nexthop_family_compatible(session, &prefix_addr, &upd->nexthop))
        {
            if (stats)
            {
                stats->reach_failed++;
            }
            continue;
        }

        if (bgp_relay_route_upsert(vrf_id, nlri, &session->neighbor_addr, &upd->attr, &upd->nexthop) == ERRCODE_SUCCESS)
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

        (void)bgp_relay_route_remove(vrf_id, nlri, &session->neighbor_addr);

        if (stats)
        {
            stats->unreach_injected++;
        }
    }
}

void bgp_relay_flush_peer_routes(uint32_t vrf_id, const net_addr_t *source)
{
    if (!source || source->family == 0 || !g_bgp_work_local->protocol)
    {
        return;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, vrf_id);
    if (!vrf || !vrf->inst_hash)
    {
        return;
    }

    GHashTableIter inst_iter;
    gpointer inst_key_ptr = NULL;
    gpointer inst_val_ptr = NULL;
    g_hash_table_iter_init(&inst_iter, vrf->inst_hash);
    while (g_hash_table_iter_next(&inst_iter, &inst_key_ptr, &inst_val_ptr))
    {
        (void)inst_key_ptr;
        bgp_instance_t *inst = (bgp_instance_t *)inst_val_ptr;
        if (!inst || !inst->rib)
        {
            continue;
        }

        GPtrArray *nlri_list = g_ptr_array_new_with_free_func(g_free);
        if (!nlri_list)
        {
            continue;
        }

        bgp_rib_foreach_source(inst->rib, source, bgp_relay_collect_nlri_cb, nlri_list);
        for (guint i = 0; i < nlri_list->len; ++i)
        {
            bgp_nlri_entry_t *nlri = (bgp_nlri_entry_t *)g_ptr_array_index(nlri_list, i);
            if (!nlri)
            {
                continue;
            }
            (void)bgp_relay_route_remove_from_inst(inst, nlri, source);
        }

        g_ptr_array_free(nlri_list, TRUE);
    }
}

uint32_t bgp_relay_handle_nh_notify(const route_nh_iter_notify_t *notify)
{
    if (!notify || !g_bgp_relay_nh_table)
    {
        return 0;
    }
    if (notify->safi != 0 && notify->safi != BGP_SAFI_UNICAST)
    {
        return 0;
    }

    const net_addr_t *key_nh = (notify->nexthop_addr.family == AF_INET || notify->nexthop_addr.family == AF_INET6)
                                   ? &notify->nexthop_addr
                                   : &notify->relay_addr;
    if (key_nh->family != AF_INET && key_nh->family != AF_INET6)
    {
        return 0;
    }

    bgp_relay_nh_key_t key;
    bgp_relay_make_nh_key(&key, notify->vrf_id, notify->afi, (notify->safi == 0) ? BGP_SAFI_UNICAST : notify->safi,
                          key_nh);

    bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(&key);
    if (!watch)
    {
        return 0;
    }

    uint8_t new_state = notify->resolved ? 1u : 0u;
    gboolean state_changed = (watch->resolved != new_state);

    watch->resolved = new_state;
    watch->out_ifindex = notify->out_ifindex;
    watch->relay_addr = notify->relay_addr;
    watch->updated_at_usec = g_get_real_time();

    uint32_t touched = 0;
    for (GList *l = watch->route_list; l; l = l->next)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)l->data;
        if (!route || !route->head || !route->head->inst)
        {
            continue;
        }

        bgp_relay_route_iter_update_from_watch(route, watch);
        if (!state_changed)
        {
            continue;
        }

        bgp_instance_t *inst = route->head->inst;
        if (bgp_relay_set_route_valid(inst, &route->head->nlri, &route->source, watch->resolved ? TRUE : FALSE) > 0)
        {
            touched++;
        }
    }

    return touched;
}
