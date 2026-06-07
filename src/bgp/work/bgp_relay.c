/**
 * @file   bgp_relay.c
 * @brief  BGP nexthop relay：维护 nexthop 与路由关系，并消费 ROUTE nexthop 回调
 */
#include "bgp_relay.h"

#include <string.h>
#include <sys/socket.h>

#include "bgp_calc.h"
#include "bgp_if_cache.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_nexthop.h"
#include "bgp_peer.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_vrf_import.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "route.h"

typedef struct bgp_relay_nh_key
{
    uint32_t nexthop_id;
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
    uint32_t tunnel_id;
    net_addr_t relay_addr;
    gint64 updated_at_usec;
    GList *route_list; /* bgp_route_node_t* 双挂（同时在 rthead->route_list） */
} bgp_relay_nh_watch_t;

static GHashTable *g_bgp_relay_nh_table = NULL; /* bgp_relay_nh_key_t* -> bgp_relay_nh_watch_t* */

static gboolean bgp_relay_route_is_lu(const bgp_route_node_t *route);

static guint bgp_relay_nh_key_hash(gconstpointer p)
{
    const bgp_relay_nh_key_t *k = (const bgp_relay_nh_key_t *)p;
    if (!k)
    {
        return 0;
    }
    if (k->nexthop_id != 0u)
    {
        return (guint)(k->nexthop_id * 33u + k->safi);
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
    if (ka->nexthop_id != kb->nexthop_id || ka->safi != kb->safi)
    {
        return FALSE;
    }
    if (ka->nexthop_id != 0u)
    {
        return TRUE;
    }
    return ka->vrf_id == kb->vrf_id && ka->afi == kb->afi && net_addr_equal(&ka->nexthop_addr, &kb->nexthop_addr);
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
        for (GList *l = watch->route_list; l; l = l->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)l->data;
            bgp_route_node_borrow_unref(route);
        }
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

static uint8_t bgp_relay_normalize_safi(uint8_t safi)
{
    return (safi == 0) ? BGP_SAFI_UNICAST : safi;
}

static uint8_t bgp_relay_route_nh_safi(uint8_t safi)
{
    (void)safi;
    return BGP_SAFI_UNICAST;
}

static int bgp_relay_make_route_nh_key(bgp_relay_nh_key_t *key, uint32_t nexthop_id, uint8_t safi)
{
    if (!key || nexthop_id == 0u)
    {
        return 0;
    }

    memset(key, 0, sizeof(*key));
    key->nexthop_id = nexthop_id;
    key->safi = bgp_relay_route_nh_safi(safi);
    return 1;
}

static void bgp_relay_make_tunnel_key(bgp_relay_nh_key_t *key, uint32_t vrf_id, uint16_t afi, uint8_t safi,
                                      const net_addr_t *endpoint)
{
    memset(key, 0, sizeof(*key));
    key->vrf_id = vrf_id;
    key->afi = afi;
    key->safi = bgp_relay_normalize_safi(safi);
    if (endpoint)
    {
        key->nexthop_addr = *endpoint;
    }
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

static gboolean bgp_relay_route_is_local_cross_uc(const bgp_route_node_t *route)
{
    return route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) && route->head && route->head->inst &&
           route->head->inst->vrf && route->head->inst->vrf->vrf_id != BGP_VRF_PUBLIC_ID &&
           route->head->inst->safi == BGP_SAFI_UNICAST;
}

static int bgp_relay_build_nh_key_from_route(const bgp_route_node_t *route, bgp_relay_nh_key_t *nh_key_out)
{
    if (!route || !route->head || !route->head->inst || !route->head->inst->vrf || !nh_key_out)
    {
        return 0;
    }

    net_addr_t nexthop_addr;
    if (bgp_nexthop_get_route_addr(route, &nexthop_addr) != ERRCODE_SUCCESS ||
        (nexthop_addr.family != AF_INET && nexthop_addr.family != AF_INET6))
    {
        return 0;
    }

    /* vrf-import 远端跨表路由（REMOTE_CROSS）：下一跳是远端 PE（eBGP vpnv4 邻居），
     * 在公网表对该 PE 地址做隧道迭代 → 命中 eBGP-vpnv4 假隧道（BGP_ADJ），用于 VRF 转发。
     * 注意 endpoint 是「下一跳 PE 地址」而非前缀（与 labeled 的 endpoint=前缀不同）。
     * safi 必须用隧道 watch 的规范值 BGP_SAFI_LABELED：TUNNEL 只按 (vrf,afi,endpoint) 标识 watch，
     * 不携带 safi，而 tunnel notify 回来时 bgp_relay_handle_tunnel_notify 固定用 BGP_SAFI_LABELED
     * 构造查找键。若这里用导入路由派生的 unicast safi（=1），notify 将查不到本 watch、
     * REMOTE_CROSS 路由永远解析不出隧道、无法下刷 VRF FIB。 */
    if (BIT_TEST(route->flags, BGP_ROUTE_FLAG_REMOTE_CROSS))
    {
        bgp_relay_make_tunnel_key(nh_key_out, BGP_VRF_PUBLIC_ID, route->head->nlri.afi, BGP_SAFI_LABELED,
                                  &nexthop_addr);
        return 1;
    }

    if (bgp_relay_route_is_lu(route))
    {
        const net_addr_t *endpoint = &route->head->nlri.prefix.prefix.addr;
        if (endpoint->family != AF_INET && endpoint->family != AF_INET6)
        {
            return 0;
        }
        bgp_relay_make_tunnel_key(nh_key_out, route->head->inst->vrf->vrf_id, route->head->nlri.afi,
                                  route->head->nlri.safi, endpoint);
        return 1;
    }

    if (route->nexthop_id == 0u)
    {
        return 0;
    }

    return bgp_relay_make_route_nh_key(nh_key_out, route->nexthop_id, route->head->nlri.safi);
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
                                        const bgp_attr_t *attr, const bgp_attr_t *base_attr,
                                        const bgp_nexthop_t *nexthop, bgp_route_node_t **route_out,
                                        gboolean *is_new_out)
{
    if (is_new_out)
    {
        *is_new_out = FALSE;
    }
    if (route_out)
    {
        *route_out = NULL;
    }
    if (!inst || !nlri || !source || source->family == 0 || !attr || !nexthop)
    {
        return -1;
    }

    bgp_rib_t *rib = bgp_inst_rib_ensure_for_nlri(inst, nlri);
    if (!rib)
    {
        return -1;
    }

    bgp_rthead_t *head = bgp_rib_ensure_head(rib, nlri);
    if (!head)
    {
        return -1;
    }

    bgp_route_node_t *route = bgp_rthead_lookup_route_mut(head, source);
    gboolean is_new = FALSE;
    if (!route)
    {
        route = bgp_rthead_create_route(rib, head, source);
        if (!route)
        {
            return -1;
        }
        is_new = TRUE;
    }

    /* peer 学习的路径不是 import-route，传 ROUTE_PROTOCOL_MAX 哨兵以避免被 IMPORT 标记位识别 */
    if (bgp_rib_route_apply_reach(route, ROUTE_PROTOCOL_MAX, attr) != 0)
    {
        return -1;
    }
    if (bgp_rib_route_set_base_attr(route, base_attr) != 0)
    {
        return -1;
    }
    bgp_route_set_label_from_nlri(route, nlri, BGP_ROUTE_LABEL_SOURCE_RECEIVED);

    if (bgp_rib_set_route_valid(rib, nlri, source, FALSE) < 0)
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
    if (!inst || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, nlri);
    if (!rib)
    {
        return 0;
    }
    int rc = bgp_rib_unreach_one(rib, nlri, source);
    if (rc == 1 && inst->calc_queue)
    {
        bgp_calc_queue_push(inst->calc_queue, inst, nlri);
    }
    return rc;
}

static int bgp_relay_set_route_valid(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri, const net_addr_t *source,
                                     gboolean valid)
{
    if (!inst || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, nlri);
    if (!rib)
    {
        return 0;
    }
    int rc = bgp_rib_set_route_valid(rib, nlri, source, valid);
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
    /* 登记借用引用：阻止 route_node_free 在 watch 仍持有指针时真正释放节点 */
    bgp_route_node_borrow_ref(route);
    return ERRCODE_SUCCESS;
}

static void bgp_relay_fill_nh_iter_req(route_nh_iter_req_t *req, const bgp_relay_nh_key_t *key)
{
    if (!req || !key)
    {
        return;
    }

    memset(req, 0, sizeof(*req));
    req->nexthop_id = key->nexthop_id;
    req->safi = key->safi;
}

static void bgp_relay_fill_tunnel_resolve_req(tunnel_resolve_req_t *req, const bgp_relay_nh_key_t *key)
{
    if (!req || !key)
    {
        return;
    }

    memset(req, 0, sizeof(*req));
    req->vrf_id = key->vrf_id;
    req->afi = key->afi;
    req->endpoint = key->nexthop_addr;
}

static guint32 bgp_relay_owner_id_from_source(const net_addr_t *source)
{
    return source ? (guint32)net_addr_hash(source) : 0u;
}

static gboolean bgp_relay_route_is_lu(const bgp_route_node_t *route)
{
    return route && route->head && route->head->nlri.type == BGP_NLRI_PREFIX &&
           route->head->nlri.safi == BGP_SAFI_LABELED && route->has_label;
}

static void bgp_relay_fill_lu_candidate(tunnel_candidate_t *candidate, const bgp_route_node_t *route)
{
    if (!candidate || !bgp_relay_route_is_lu(route))
    {
        return;
    }

    memset(candidate, 0, sizeof(*candidate));
    candidate->vrf_id =
        route->head->inst && route->head->inst->vrf ? route->head->inst->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
    candidate->afi = route->head->nlri.afi;
    candidate->source_type = TUNNEL_SOURCE_BGP_LU;
    candidate->preference = 100u;
    candidate->owner_module_id = DEV_MODULE_ID_BGP;
    candidate->owner_id = bgp_relay_owner_id_from_source(&route->source);
    candidate->fec.vrf_id = candidate->vrf_id;
    candidate->fec.afi = candidate->afi;
    candidate->fec.prefix_len = route->head->nlri.prefix.prefix.prefix_len;
    candidate->fec.addr = route->head->nlri.prefix.prefix.addr;
    candidate->endpoint = route->head->nlri.prefix.prefix.addr;
    (void)bgp_nexthop_get_route_addr(route, &candidate->nexthop);
    candidate->label_count = 1u;
    candidate->labels[0] = route->label;
}

static void bgp_relay_publish_lu_candidate(const bgp_route_node_t *route, gboolean add)
{
    net_addr_t nexthop_addr;
    if (!bgp_relay_route_is_lu(route) || bgp_nexthop_get_route_addr(route, &nexthop_addr) != ERRCODE_SUCCESS ||
        (nexthop_addr.family != AF_INET && nexthop_addr.family != AF_INET6))
    {
        return;
    }

    tunnel_candidate_t candidate;
    bgp_relay_fill_lu_candidate(&candidate, route);
    if (add)
    {
        (void)tunnel_rpc_candidate_add(g_bgp_local->dev_ipc_ctx, &candidate);
    }
    else
    {
        (void)tunnel_rpc_candidate_del(g_bgp_local->dev_ipc_ctx, &candidate);
    }
}

static void bgp_relay_nh_watch_remove_if_empty(const bgp_relay_nh_watch_t *watch)
{
    if (!watch || watch->route_list)
    {
        return;
    }

    if (watch->key.nexthop_id == 0u)
    {
        tunnel_resolve_req_t req;
        bgp_relay_fill_tunnel_resolve_req(&req, &watch->key);
        (void)tunnel_rpc_resolve_unregister(g_bgp_local->dev_ipc_ctx, &req);
    }
    else
    {
        route_nh_iter_req_t req;
        bgp_relay_fill_nh_iter_req(&req, &watch->key);
        (void)route_rpc_nh_unregister(g_bgp_local->dev_ipc_ctx, &req);
    }

    g_hash_table_remove(g_bgp_relay_nh_table, &watch->key);
}

static void bgp_relay_value_from_watch(const bgp_relay_nh_watch_t *watch, bgp_nexthop_value_t *value)
{
    if (!value)
    {
        return;
    }

    memset(value, 0, sizeof(*value));
    if (!watch)
    {
        return;
    }

    value->iter_watched = 1u;
    value->iter_resolved = watch->resolved ? 1u : 0u;
    value->iter_out_ifindex = watch->out_ifindex;
    value->tunnel_id = watch->tunnel_id;
    value->iter_relay_addr = watch->relay_addr;
    value->updated_at_usec = watch->updated_at_usec;
}

static void bgp_relay_route_iter_clear(bgp_route_node_t *route)
{
    if (!route || route->nexthop_id == 0u || !route->head || !route->head->inst)
    {
        return;
    }

    bgp_nexthop_clear_value(route->head->inst, route->nexthop_id);
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

    if (route->nexthop_id == 0u || !route->head || !route->head->inst)
    {
        return;
    }

    bgp_nexthop_value_t value;
    bgp_relay_value_from_watch(watch, &value);

    (void)bgp_nexthop_set_value(route->head->inst, route->nexthop_id, &value);
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

    GList *link = g_list_find(watch->route_list, route);
    if (link)
    {
        watch->route_list = g_list_delete_link(watch->route_list, link);
        /* 借用计数与 add_route 配对：watch 不再持有该指针 */
        bgp_route_node_borrow_unref(route);
    }
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
        watch->tunnel_id = 0u;
        watch->relay_addr.family = 0;
        watch->updated_at_usec = g_get_real_time();
        watch->route_list = NULL;

        int register_rc = ERRCODE_FAIL;
        /* 按 key 类型分派（与 unregister 一致）：隧道 key（nexthop_id==0，含 endpoint）走隧道迭代，
         * 路由 nh key（nexthop_id!=0）走 IP 迭代。这样 labeled / REMOTE_CROSS 走隧道，
         * vpnv4 公网（route nh key）走 IP 迭代。 */
        if (watch->key.nexthop_id == 0u)
        {
            tunnel_resolve_req_t req;
            bgp_relay_fill_tunnel_resolve_req(&req, &watch->key);
            register_rc = tunnel_rpc_resolve_register(g_bgp_local->dev_ipc_ctx, &req);
        }
        else
        {
            route_nh_iter_req_t req;
            bgp_relay_fill_nh_iter_req(&req, &watch->key);
            register_rc = route_rpc_nh_register(g_bgp_local->dev_ipc_ctx, &req);
        }

        if (register_rc != ERRCODE_SUCCESS)
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

int bgp_relay_get_route_iter_value(const bgp_route_node_t *route, bgp_nexthop_value_t *value_out)
{
    if (!route || !value_out)
    {
        return ERRCODE_FAIL;
    }

    memset(value_out, 0, sizeof(*value_out));

    /* REMOTE_CROSS 用自己的隧道 watch；私网 unicast LOCAL_CROSS 用自己的 nexthop-vrf watch；
     * vpnv4 export LOCAL_CROSS 与 import-rib mirror 仍通过 src_route 继承源解析。 */
    const bgp_route_node_t *owner = (route->src_route && !BIT_TEST(route->flags, BGP_ROUTE_FLAG_REMOTE_CROSS) &&
                                     !bgp_relay_route_is_local_cross_uc(route))
                                        ? route->src_route
                                        : route;
    if (owner->nexthop_id != 0u && owner->head && owner->head->inst &&
        bgp_nexthop_get_value(owner->head->inst, owner->nexthop_id, value_out) == ERRCODE_SUCCESS)
    {
        return ERRCODE_SUCCESS;
    }

    bgp_relay_nh_key_t nh_key;
    if (!bgp_relay_build_nh_key_from_route(owner, &nh_key))
    {
        return ERRCODE_FAIL;
    }

    const bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(&nh_key);
    if (!watch)
    {
        return ERRCODE_FAIL;
    }

    bgp_relay_value_from_watch(watch, value_out);
    return ERRCODE_SUCCESS;
}

gboolean bgp_relay_synthetic_nexthop_register(bgp_route_node_t *route)
{
    if (!route)
    {
        return FALSE;
    }
    bgp_relay_nh_key_t nh_key;
    if (!bgp_relay_build_nh_key_from_route(route, &nh_key))
    {
        return FALSE;
    }
    bgp_relay_nh_watch_t *watch = bgp_relay_attach_route_to_watch(route, &nh_key);
    return (watch && watch->resolved) ? TRUE : FALSE;
}

void bgp_relay_synthetic_nexthop_unregister(bgp_route_node_t *route)
{
    if (!route)
    {
        return;
    }
    bgp_relay_nh_key_t nh_key;
    if (!bgp_relay_build_nh_key_from_route(route, &nh_key))
    {
        return;
    }
    bgp_relay_detach_route_from_watch(route, &nh_key, TRUE);
}

static int bgp_relay_route_remove_from_inst(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri,
                                            const net_addr_t *source)
{
    if (!inst || !nlri || !source || source->family == 0)
    {
        return -1;
    }

    bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, nlri);
    if (!rib)
    {
        return 0;
    }
    const bgp_rthead_t *head_ro = bgp_rib_lookup_head(rib, nlri);
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
        bgp_relay_publish_lu_candidate(route, FALSE);
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
                                  const bgp_attr_t *attr, const bgp_attr_t *base_attr, const bgp_nexthop_t *nexthop)
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
    if (!inst)
    {
        return ERRCODE_FAIL;
    }

    bgp_rib_t *rib = bgp_inst_rib_for_nlri(inst, nlri);
    const bgp_rthead_t *head_ro = rib ? bgp_rib_lookup_head(rib, nlri) : NULL;
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
    if (bgp_relay_reach_route_to_rib(inst, nlri, source, attr, base_attr, nexthop, &route, NULL) < 0 || !route)
    {
        return ERRCODE_FAIL;
    }

    if (bgp_nexthop_set_route(route, nexthop) != ERRCODE_SUCCESS)
    {
        if (old_route && had_old_nh)
        {
            bgp_relay_detach_route_from_watch(route, &old_nh_key, TRUE);
        }
        bgp_relay_publish_lu_candidate(route, FALSE);
        (void)bgp_relay_set_route_valid(inst, nlri, source, FALSE);
        return ERRCODE_SUCCESS;
    }

    /* QP 路由不依赖 nexthop 迭代：直接置 valid 并触发优选。 */
    if (nlri->type == BGP_NLRI_QP && nlri->safi == BGP_SAFI_QP)
    {
        if (old_route && had_old_nh)
        {
            bgp_relay_detach_route_from_watch(route, &old_nh_key, TRUE);
        }
        if (bgp_relay_set_route_valid(inst, nlri, source, TRUE) < 0)
        {
            return ERRCODE_FAIL;
        }
        bgp_relay_trigger_calc(inst, nlri);
        return ERRCODE_SUCCESS;
    }

    /* 无可迭代 nexthop：路由仍保留在 Loc-RIB，仅置 invalid（不优选），绝不撤销。 */
    bgp_relay_nh_key_t new_nh_key;
    if (!bgp_relay_build_nh_key_from_route(route, &new_nh_key))
    {
        if (old_route && had_old_nh)
        {
            bgp_relay_detach_route_from_watch(route, &old_nh_key, TRUE);
        }
        bgp_relay_publish_lu_candidate(route, FALSE);
        (void)bgp_relay_set_route_valid(inst, nlri, source, FALSE); /* 内部按需触发 calc */
        return ERRCODE_SUCCESS;
    }

    gboolean nh_changed = (old_route && (!had_old_nh || !bgp_relay_nh_key_equal(&old_nh_key, &new_nh_key)));

    /* 迭代 watch 注册失败：同样保留在 Loc-RIB 置 invalid，不撤销。 */
    bgp_relay_nh_watch_t *watch = bgp_relay_attach_route_to_watch(route, &new_nh_key);
    if (!watch)
    {
        if (old_route && had_old_nh)
        {
            bgp_relay_detach_route_from_watch(route, &old_nh_key, TRUE);
        }
        bgp_relay_publish_lu_candidate(route, FALSE);
        (void)bgp_relay_set_route_valid(inst, nlri, source, FALSE); /* 内部按需触发 calc */
        return ERRCODE_SUCCESS;
    }

    bgp_relay_publish_lu_candidate(route, TRUE);

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

void bgp_relay_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd, const bgp_attr_t *base_attr,
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
    const bgp_attr_t *route_base_attr = base_attr ? base_attr : &upd->attr;

    /* vpnv4 入向 RT 过滤判据(本 UPDATE 内 RT 属性共享，仅需计算一次)：-1 未算/1 命中/0 不命中 */
    int vpn_rt_match = -1;

    for (uint32_t i = 0; i < upd->reach_len; ++i)
    {
        const bgp_nlri_entry_t *nlri = &upd->reach[i];

        /* QP 路由直入 RIB，不依赖 nexthop 迭代。
         * VPN 路由不走此分支：它需要（隧道）迭代，由下方正常路径经 route_upsert 完成。 */
        if (nlri->type == BGP_NLRI_QP && nlri->safi == BGP_SAFI_QP)
        {
            if (bgp_relay_route_upsert(vrf_id, nlri, &session->neighbor_addr, &upd->attr, route_base_attr,
                                       &upd->nexthop) == ERRCODE_SUCCESS)
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
            continue;
        }

        /* 仅处理 IP 前缀类（unicast/labeled/vpn）；NLRI 格式与 nexthop 合法性（含族兼容/
         * ext-nexthop）已在解析侧各 AF 回调校验完毕，非法路由在解析时即丢弃，此处不再重复判断。
         * 其它 NLRI 类型（EVPN/FlowSpec 等）不在本 nexthop 迭代路径处理。 */
        if (nlri->type != BGP_NLRI_PREFIX)
        {
            continue;
        }

        /* vpnv4 入向按 import-RT 过滤：本路由的 RT 不命中任何私网 VRF → 整条丢弃(不入 vpnv4 RIB)。
         * 命中则正常入公网 vpnv4 RIB，导入对应 VRF 由 vpnv4 calc 完成后的 reconcile 处理。 */
        if (nlri->safi == BGP_SAFI_VPN_UNICAST)
        {
            if (vpn_rt_match < 0)
            {
                vpn_rt_match = bgp_vrf_import_attr_has_match(&upd->attr) ? 1 : 0;
            }
            if (vpn_rt_match == 0)
            {
                continue; /* 无 IRT 匹配，丢弃；不计入 injected/failed */
            }
        }

        if (bgp_relay_route_upsert(vrf_id, nlri, &session->neighbor_addr, &upd->attr, route_base_attr, &upd->nexthop) ==
            ERRCODE_SUCCESS)
        {
            if (stats)
            {
                stats->reach_injected++;
            }
            /* vpnv4 路由恒因远端 PE 下一跳无 LSP 而 invalid、不触发 calc，
             * 显式触发按 import-RT 导入评估(IRT 匹配即接受，与 FIB 可达解耦)。 */
            if (nlri->safi == BGP_SAFI_VPN_UNICAST)
            {
                bgp_vrf_import_on_vpn_received(nlri);
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

        if (nlri->type == BGP_NLRI_QP && nlri->safi == BGP_SAFI_QP)
        {
            (void)bgp_relay_route_remove(vrf_id, nlri, &session->neighbor_addr);
            if (stats)
            {
                stats->unreach_injected++;
            }
            continue;
        }

        if (nlri->type != BGP_NLRI_PREFIX)
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
        if (!inst || !inst->rd_entries)
        {
            continue;
        }

        GPtrArray *nlri_list = g_ptr_array_new_with_free_func(g_free);
        if (!nlri_list)
        {
            continue;
        }

        /* 遍历该 AF 下所有 RD 的 RIB 收集来自指定来源的 NLRI */
        GHashTableIter rd_iter;
        gpointer rd_key, rd_val;
        g_hash_table_iter_init(&rd_iter, inst->rd_entries);
        while (g_hash_table_iter_next(&rd_iter, &rd_key, &rd_val))
        {
            (void)rd_key;
            bgp_rd_entry_t *e = (bgp_rd_entry_t *)rd_val;
            if (!e || !e->rib)
            {
                continue;
            }
            bgp_rib_foreach_source(e->rib, source, bgp_relay_collect_nlri_cb, nlri_list);
        }
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
    if (notify->nexthop_id == 0u)
    {
        return 0;
    }

    bgp_relay_nh_key_t key;
    if (!bgp_relay_make_route_nh_key(&key, notify->nexthop_id, notify->safi))
    {
        return 0;
    }

    bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(&key);
    if (!watch)
    {
        return 0;
    }

    uint8_t new_state = notify->resolved ? 1u : 0u;
    gboolean state_changed = (watch->resolved != new_state);

    watch->resolved = new_state;
    watch->out_ifindex = notify->out_ifindex;
    watch->tunnel_id = 0u;
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

uint32_t bgp_relay_reregister_route_nexthops(void)
{
    if (!g_bgp_relay_nh_table || !g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return 0;
    }

    uint32_t registered = 0;
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_bgp_relay_nh_table);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        bgp_relay_nh_watch_t *watch = (bgp_relay_nh_watch_t *)value;
        if (!watch || watch->key.nexthop_id == 0u)
        {
            continue;
        }

        route_nh_iter_req_t req;
        bgp_relay_fill_nh_iter_req(&req, &watch->key);
        if (route_rpc_nh_register(g_bgp_local->dev_ipc_ctx, &req) == ERRCODE_SUCCESS)
        {
            registered++;
        }
    }

    if (registered > 0)
    {
        LOG_INFO("BGP: replayed %u ROUTE nexthop watch registration(s)", registered);
    }
    return registered;
}

uint32_t bgp_relay_handle_tunnel_notify(const tunnel_resolve_notify_t *notify)
{
    if (!notify || !g_bgp_relay_nh_table)
    {
        return 0;
    }
    if (notify->endpoint.family != AF_INET && notify->endpoint.family != AF_INET6)
    {
        return 0;
    }

    bgp_relay_nh_key_t key;
    bgp_relay_make_tunnel_key(&key, notify->vrf_id, notify->afi, BGP_SAFI_LABELED, &notify->endpoint);

    bgp_relay_nh_watch_t *watch = bgp_relay_nh_watch_lookup(&key);
    if (!watch)
    {
        return 0;
    }

    uint8_t new_state = notify->resolved ? 1u : 0u;
    gboolean state_changed = (watch->resolved != new_state);
    gboolean tunnel_changed = (watch->tunnel_id != notify->tunnel_id);

    watch->resolved = new_state;
    watch->out_ifindex = notify->out_ifindex;
    watch->tunnel_id = notify->tunnel_id;
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
        if (!state_changed && !tunnel_changed)
        {
            continue;
        }

        bgp_instance_t *inst = route->head->inst;
        int rc_valid =
            bgp_relay_set_route_valid(inst, &route->head->nlri, &route->source, watch->resolved ? TRUE : FALSE);
        if (rc_valid > 0)
        {
            touched++;
        }
        else if (rc_valid == 0 && tunnel_changed && watch->resolved)
        {
            bgp_relay_trigger_calc(inst, &route->head->nlri);
            touched++;
        }
    }

    return touched;
}

void bgp_relay_session_lu_adj_sync(bgp_session_t *session, gboolean up)
{
    if (!session || session->neighbor_addr.family == 0)
    {
        return;
    }

    for (GList *l = session->peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        /* labeled-unicast 与 vpnv4 邻居都下发「邻接假隧道」：endpoint=邻居地址，out_ifindex
         * 取直连接口（非直连=0 时不下发，天然只对直连 eBGP 生效）。vpnv4 用它让导入私网的
         * REMOTE_CROSS 路由能在公网表迭代到该假隧道、走隧道转发。 */
        if (!peer || !peer->inst || (peer->inst->safi != BGP_SAFI_LABELED && peer->inst->safi != BGP_SAFI_VPN_UNICAST))
        {
            continue;
        }
        if (up && peer->state != BGP_PEER_STATE_ESTABLISHED)
        {
            continue;
        }

        tunnel_candidate_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        candidate.vrf_id = peer->vrf ? peer->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
        candidate.afi = peer->inst->afi;
        candidate.source_type = TUNNEL_SOURCE_BGP_ADJ;
        candidate.preference = 10u;
        candidate.owner_module_id = DEV_MODULE_ID_BGP;
        candidate.owner_id = ((uint32_t)candidate.afi << 16) | (uint32_t)peer->inst->safi;
        candidate.endpoint = session->neighbor_addr;
        candidate.relay_addr = session->neighbor_addr;
        candidate.out_ifindex = bgp_if_cache_direct_ifindex(&session->neighbor_addr);

        if (up && candidate.out_ifindex != 0u)
        {
            (void)tunnel_rpc_candidate_add(g_bgp_local->dev_ipc_ctx, &candidate);
        }
        else
        {
            (void)tunnel_rpc_candidate_del(g_bgp_local->dev_ipc_ctx, &candidate);
        }
    }
}
