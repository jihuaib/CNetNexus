/**
 * @file   bgp_vrf_import.c
 * @brief  VPNv4/VPNv6 路由按同 AF import-RT 导入私网 VRF，支持 MPLS/SRv6 BE
 * @author jhb
 * @date   2026/06/02
 */
#include "bgp_vrf_import.h"

#include <arpa/inet.h>
#include <string.h>

#include "bgp.h"
#include "bgp_attr_intern.h"
#include "bgp_calc.h"
#include "bgp_ext_community.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_nexthop.h"
#include "bgp_peer.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_rd.h"
#include "bgp_relay.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "bit.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "tunnel.h"
#include "vrf.h"

typedef struct bgp_irt_key
{
    uint16_t afi;
    uint8_t rt[8];
} bgp_irt_key_t;

/* IRT 索引：(afi, 规范化 8 字节 RT)(g_malloc 持有)
 * → inner GHashTable<vrf_id → refcount>。
 * 仅在 BGP worker 线程内访问，无需加锁。 */
static GHashTable *g_bgp_irt_index;

/* ============================================================================
 * IRT 索引
 * ========================================================================== */

static guint irt_key_hash(gconstpointer p)
{
    const bgp_irt_key_t *key = (const bgp_irt_key_t *)p;
    if (!key)
    {
        return 0;
    }
    guint h = 2166136261u; /* FNV-1a 32 */
    h ^= (uint8_t)(key->afi >> 8);
    h *= 16777619u;
    h ^= (uint8_t)key->afi;
    h *= 16777619u;
    for (int i = 0; i < 8; i++)
    {
        h ^= key->rt[i];
        h *= 16777619u;
    }
    return h;
}

static gboolean irt_key_equal(gconstpointer a, gconstpointer b)
{
    const bgp_irt_key_t *ka = (const bgp_irt_key_t *)a;
    const bgp_irt_key_t *kb = (const bgp_irt_key_t *)b;
    return ka && kb && ka->afi == kb->afi && memcmp(ka->rt, kb->rt, sizeof(ka->rt)) == 0;
}

static gboolean irt_key_build(bgp_irt_key_t *key, bgp_afi_t afi, const vrf_rt_t *rt)
{
    if (!key || !rt || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return FALSE;
    }
    memset(key, 0, sizeof(*key));
    key->afi = (uint16_t)afi;
    return bgp_ext_community_rt_canon(rt, key->rt) ? TRUE : FALSE;
}

static void irt_key_build_canon(bgp_irt_key_t *key, bgp_afi_t afi, const uint8_t rt[8])
{
    memset(key, 0, sizeof(*key));
    key->afi = (uint16_t)afi;
    memcpy(key->rt, rt, sizeof(key->rt));
}

void bgp_vrf_import_init(void)
{
    if (g_bgp_irt_index)
    {
        return;
    }
    g_bgp_irt_index = g_hash_table_new_full(irt_key_hash, irt_key_equal, g_free, (GDestroyNotify)g_hash_table_destroy);
}

void bgp_vrf_import_cleanup(void)
{
    if (g_bgp_irt_index)
    {
        g_hash_table_destroy(g_bgp_irt_index);
        g_bgp_irt_index = NULL;
    }
}

void bgp_vrf_import_irt_add(uint32_t vrf_id, bgp_afi_t afi, const vrf_rt_t *rt)
{
    if (!g_bgp_irt_index || vrf_id == BGP_VRF_PUBLIC_ID || !rt)
    {
        return;
    }
    bgp_irt_key_t key;
    if (!irt_key_build(&key, afi, rt))
    {
        return;
    }

    GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, &key);
    if (!inner)
    {
        inner = g_hash_table_new(g_direct_hash, g_direct_equal);
        bgp_irt_key_t *kdup = (bgp_irt_key_t *)g_malloc(sizeof(*kdup));
        *kdup = key;
        g_hash_table_insert(g_bgp_irt_index, kdup, inner);
    }
    guint cnt = GPOINTER_TO_UINT(g_hash_table_lookup(inner, GUINT_TO_POINTER(vrf_id)));
    g_hash_table_insert(inner, GUINT_TO_POINTER(vrf_id), GUINT_TO_POINTER(cnt + 1));
}

void bgp_vrf_import_irt_del(uint32_t vrf_id, bgp_afi_t afi, const vrf_rt_t *rt)
{
    if (!g_bgp_irt_index || !rt)
    {
        return;
    }
    bgp_irt_key_t key;
    if (!irt_key_build(&key, afi, rt))
    {
        return;
    }
    GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, &key);
    if (!inner)
    {
        return;
    }
    gpointer orig_k = NULL;
    gpointer orig_v = NULL;
    if (!g_hash_table_lookup_extended(inner, GUINT_TO_POINTER(vrf_id), &orig_k, &orig_v))
    {
        return;
    }
    guint cnt = GPOINTER_TO_UINT(orig_v);
    if (cnt <= 1)
    {
        g_hash_table_remove(inner, GUINT_TO_POINTER(vrf_id));
    }
    else
    {
        g_hash_table_insert(inner, GUINT_TO_POINTER(vrf_id), GUINT_TO_POINTER(cnt - 1));
    }
    if (g_hash_table_size(inner) == 0)
    {
        g_hash_table_remove(g_bgp_irt_index, &key); /* 触发 inner 销毁 + key free */
    }
}

void bgp_vrf_import_purge_vrf(uint32_t vrf_id)
{
    if (!g_bgp_irt_index)
    {
        return;
    }
    GPtrArray *empties = g_ptr_array_new();
    GHashTableIter it;
    gpointer k = NULL;
    gpointer v = NULL;
    g_hash_table_iter_init(&it, g_bgp_irt_index);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        GHashTable *inner = (GHashTable *)v;
        g_hash_table_remove(inner, GUINT_TO_POINTER(vrf_id));
        if (g_hash_table_size(inner) == 0)
        {
            g_ptr_array_add(empties, k); /* key 仍由 g_bgp_irt_index 持有，迭代结束后再删 */
        }
    }
    for (guint i = 0; i < empties->len; i++)
    {
        g_hash_table_remove(g_bgp_irt_index, g_ptr_array_index(empties, i));
    }
    g_ptr_array_free(empties, TRUE);
}

void bgp_vrf_import_purge_vrf_afi(uint32_t vrf_id, bgp_afi_t afi)
{
    if (!g_bgp_irt_index || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return;
    }
    GPtrArray *empties = g_ptr_array_new();
    GHashTableIter it;
    gpointer k = NULL;
    gpointer v = NULL;
    g_hash_table_iter_init(&it, g_bgp_irt_index);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        const bgp_irt_key_t *key = (const bgp_irt_key_t *)k;
        GHashTable *inner = (GHashTable *)v;
        if (!key || key->afi != (uint16_t)afi)
        {
            continue;
        }
        g_hash_table_remove(inner, GUINT_TO_POINTER(vrf_id));
        if (g_hash_table_size(inner) == 0)
        {
            g_ptr_array_add(empties, k);
        }
    }
    for (guint i = 0; i < empties->len; i++)
    {
        g_hash_table_remove(g_bgp_irt_index, g_ptr_array_index(empties, i));
    }
    g_ptr_array_free(empties, TRUE);
}

void bgp_vrf_import_purge_all(void)
{
    if (g_bgp_irt_index)
    {
        g_hash_table_remove_all(g_bgp_irt_index);
    }
}

/** 收集 attr 中所有 RT 命中的私网 vrf_id 到 set(key=GUINT_TO_POINTER(vrf_id))；返回命中条数 */
static guint collect_matched_vrfs(const bgp_attr_t *attr, bgp_afi_t afi, GHashTable *set)
{
    if (!attr || !g_bgp_irt_index || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return 0;
    }
    const uint8_t *ec = attr->ext_communities;
    uint16_t len = attr->ext_communities_len;
    if (!ec || (len % 8) != 0)
    {
        return 0;
    }
    guint matched = 0;
    for (uint16_t off = 0; off + 8 <= len; off = (uint16_t)(off + 8))
    {
        if (!bgp_ext_community_is_rt(ec + off))
        {
            continue;
        }
        bgp_irt_key_t key;
        irt_key_build_canon(&key, afi, ec + off);
        GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, &key);
        if (!inner)
        {
            continue;
        }
        GHashTableIter it;
        gpointer k = NULL;
        gpointer v = NULL;
        g_hash_table_iter_init(&it, inner);
        while (g_hash_table_iter_next(&it, &k, &v))
        {
            if (set)
            {
                g_hash_table_add(set, k); /* k = GUINT_TO_POINTER(vrf_id) */
            }
            matched++;
        }
    }
    return matched;
}

gboolean bgp_vrf_import_attr_has_match(const bgp_attr_t *attr, bgp_afi_t afi)
{
    if (!attr || !g_bgp_irt_index || (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6))
    {
        return FALSE;
    }
    const uint8_t *ec = attr->ext_communities;
    uint16_t len = attr->ext_communities_len;
    if (!ec || (len % 8) != 0)
    {
        return FALSE;
    }
    for (uint16_t off = 0; off + 8 <= len; off = (uint16_t)(off + 8))
    {
        if (!bgp_ext_community_is_rt(ec + off))
        {
            continue;
        }
        bgp_irt_key_t key;
        irt_key_build_canon(&key, afi, ec + off);
        GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, &key);
        if (inner && g_hash_table_size(inner) > 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/* ============================================================================
 * inter-AS Option B 中转换标（ASBR 转发面）：本地标签申请/释放
 *
 * SWAP 绑定的 owner_id 用全局自增序列保证每路由唯一；fec 用固定占位（owner_id 已唯一区分），
 * 以便 alloc/release 用同一 (owner_id + 占位 fec) 命中同一 TUNNEL 绑定。仅 worker 线程访问。
 * ========================================================================== */

static uint32_t g_bgp_transit_owner_seq = 0x01000000u; /* 高位起始，避开 export 的小 owner_id */

#define BGP_TRANSIT_LABEL_TIMEOUT_MS 1000u

/** 构造一条 SWAP 中转标签请求（alloc/release 共用，确保命中同一绑定） */
static void transit_build_req(tunnel_label_req_t *req, uint32_t owner_id, uint16_t afi, uint32_t swap_label,
                              const net_addr_t *endpoint)
{
    memset(req, 0, sizeof(*req));
    req->vrf_id = BGP_VRF_PUBLIC_ID; /* ASBR 无 VRF，公网迭代 */
    req->afi = afi;
    req->source_type = TUNNEL_SOURCE_BGP_LU;
    req->owner_module_id = DEV_MODULE_ID_BGP;
    req->owner_id = owner_id;
    req->fec.vrf_id = BGP_VRF_PUBLIC_ID;
    req->fec.afi = afi;
    req->fec.prefix_len = 0;        /* 占位：owner_id 已唯一标识本绑定 */
    req->fec.addr.family = AF_INET; /* TUNNEL 要求 fec.addr.family 非 0 */
    req->action = TUNNEL_ACTION_SWAP;
    req->swap_label = swap_label;
    if (endpoint)
    {
        req->endpoint = *endpoint;
    }
}

uint32_t bgp_vrf_import_transit_alloc_label(bgp_route_node_t *best)
{
    if (!best || !best->has_label || best->label == 0u)
    {
        return 0; /* 无收到的 VPN 标签，无从换标 */
    }
    if (!g_bgp_local || !g_bgp_local->dev_ipc_ctx)
    {
        return 0;
    }

    /* 下游端点 = 本路由的 BGP 下一跳（改下一跳前的原始下一跳，公网可迭代到隧道） */
    bgp_nexthop_t nh;
    memset(&nh, 0, sizeof(nh));
    if (bgp_nexthop_get_route_bgp(best, &nh) != ERRCODE_SUCCESS || nh.global.family == 0)
    {
        return 0;
    }
    uint16_t afi = (nh.global.family == AF_INET6) ? (uint16_t)BGP_AFI_IPV6 : (uint16_t)BGP_AFI_IPV4;

    if (best->out_local_label != 0u)
    {
        /* peer reach 更新会原地复用 route node。只有收到的 VPN label
         * 和传输 next-hop 都未变时，旧 SWAP 绑定才可复用。 */
        if (best->transit_swap_label == best->label && best->transit_afi == afi &&
            net_addr_equal(&best->transit_endpoint, &nh.global))
        {
            return best->out_local_label;
        }

        tunnel_label_req_t old_req;
        transit_build_req(&old_req, best->transit_owner_id, best->transit_afi, 0, NULL);
        if (tunnel_rpc_label_release(g_bgp_local->dev_ipc_ctx, &old_req) != ERRCODE_SUCCESS)
        {
            LOG_WARN("BGP vrf-import: stale transit SWAP release failed, hold advertise");
            return 0;
        }
        best->out_local_label = 0u;
        best->transit_owner_id = 0u;
        best->transit_swap_label = 0u;
        best->transit_afi = 0u;
        memset(&best->transit_endpoint, 0, sizeof(best->transit_endpoint));
    }

    uint32_t owner_id = ++g_bgp_transit_owner_seq;
    tunnel_label_req_t req;
    transit_build_req(&req, owner_id, afi, best->label, &nh.global);

    uint32_t label = 0;
    if (tunnel_rpc_label_alloc(g_bgp_local->dev_ipc_ctx, &req, &label, BGP_TRANSIT_LABEL_TIMEOUT_MS) !=
            ERRCODE_SUCCESS ||
        label == 0u)
    {
        LOG_WARN("BGP vrf-import: transit swap-label alloc pending (TUNNEL unavailable), hold advertise");
        return 0;
    }
    best->out_local_label = label;
    best->transit_owner_id = owner_id;
    best->transit_swap_label = best->label;
    best->transit_afi = afi;
    best->transit_endpoint = nh.global;
    LOG_INFO("BGP vrf-import: transit swap-label %u -> recv-label %u allocated (Option B ASBR)", label, best->label);
    return label;
}

void bgp_vrf_import_transit_release_label(bgp_route_node_t *route)
{
    if (!route || route->out_local_label == 0u)
    {
        return;
    }
    if (g_bgp_local && g_bgp_local->dev_ipc_ctx)
    {
        tunnel_label_req_t req;
        uint16_t afi = (route->transit_afi == BGP_AFI_IPV6) ? (uint16_t)BGP_AFI_IPV6 : (uint16_t)BGP_AFI_IPV4;
        transit_build_req(&req, route->transit_owner_id, afi, 0, NULL);
        (void)tunnel_rpc_label_release(g_bgp_local->dev_ipc_ctx, &req);
    }
    route->out_local_label = 0u;
    route->transit_owner_id = 0u;
    route->transit_swap_label = 0u;
    route->transit_afi = 0u;
    memset(&route->transit_endpoint, 0, sizeof(route->transit_endpoint));
}

/* ============================================================================
 * 导入 reconcile
 * ========================================================================== */

static bgp_protocol_t *bgp_proto(void)
{
    return g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
}

/** 当前 public VPN instance(导入源)，未使能返回 NULL */
static bgp_instance_t *vpn_inst_lookup(bgp_afi_t afi)
{
    if (afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6)
    {
        return NULL;
    }
    bgp_protocol_t *proto = bgp_proto();
    if (!proto)
    {
        return NULL;
    }
    bgp_vrf_t *pub = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    if (!pub || !pub->inst_hash)
    {
        return NULL;
    }
    return (bgp_instance_t *)g_hash_table_lookup(pub->inst_hash, bgp_inst_hash_key(afi, BGP_SAFI_VPN_UNICAST));
}

void bgp_vrf_import_request_refresh_afi(bgp_afi_t afi)
{
    bgp_instance_t *vpn_inst = vpn_inst_lookup(afi);
    if (!vpn_inst || !vpn_inst->peer_hash || !vpn_inst->vrf)
    {
        return;
    }
    bgp_vrf_t *pub = vpn_inst->vrf;

    GHashTableIter it;
    gpointer k = NULL;
    gpointer v = NULL;
    uint32_t sent = 0;
    g_hash_table_iter_init(&it, vpn_inst->peer_hash);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        bgp_peer_t *peer = (bgp_peer_t *)v;
        if (!peer)
        {
            continue;
        }
        bgp_session_t *sess = bgp_vrf_find_session(pub, &peer->addr);
        if (!sess || sess->fsm_state != BGP_FSM_STATE_ESTABLISHED || !sess->pri_conn)
        {
            continue;
        }
        if (BIT_TEST(sess->negotiated_caps, BGP_SESS_CAP_ROUTE_REFRESH))
        {
            (void)bgp_pkt_send_route_refresh(sess->pri_conn, (uint16_t)afi, (uint8_t)BGP_SAFI_VPN_UNICAST);
            sent++;
        }
    }
    if (sent > 0)
    {
        LOG_INFO("BGP vrf-import: import-RT changed, sent VPN afi=%u ROUTE-REFRESH to %u peer(s)", (unsigned)afi, sent);
    }
}

void bgp_vrf_import_request_refresh(void)
{
    bgp_vrf_import_request_refresh_afi(BGP_AFI_IPV4);
    bgp_vrf_import_request_refresh_afi(BGP_AFI_IPV6);
}

/** 是否为 public VPN-unicast instance */
static gboolean is_public_vpn_inst(const bgp_instance_t *inst)
{
    return inst && inst->vrf && inst->vrf->vrf_id == BGP_VRF_PUBLIC_ID &&
           (inst->afi == BGP_AFI_IPV4 || inst->afi == BGP_AFI_IPV6) && inst->safi == BGP_SAFI_VPN_UNICAST;
}

/** VPN NLRI → 私网 unicast NLRI(剥 RD/标签) */
static void derive_unicast_nlri(const bgp_nlri_entry_t *vpn, bgp_nlri_entry_t *out)
{
    *out = *vpn;
    out->safi = BGP_SAFI_UNICAST;
    if (out->type == BGP_NLRI_PREFIX)
    {
        memset(out->prefix.rd.bytes, 0, sizeof(out->prefix.rd.bytes));
        out->prefix.has_rd = false;
        out->prefix.label = 0;
        out->prefix.has_label = false;
    }
}

/**
 * 整 SID 模式的 SRv6 L3 Service 路径可用性校验。
 *
 * 本实现不支持 transposition，因而 SRv6 路径必须在 Prefix-SID 中携带
 * 完整 IPv6 SID，VPN NLRI label 必须是 Implicit NULL(3)，且 behavior 与客户
 * AF 严格对应。该函数只回答“能否采用 SRv6 BE incarnation”；没有
 * Prefix-SID 的普通 MPLS VPN 路径返回 FALSE，由调用方另行校验服务标签。
 */
static gboolean srv6_service_route_usable(const bgp_route_node_t *route, const bgp_nlri_entry_t *vpn_nlri,
                                          bgp_afi_t afi)
{
    if (!route || !route->attr || !vpn_nlri || vpn_nlri->type != BGP_NLRI_PREFIX)
    {
        return FALSE;
    }
    const bgp_attr_t *attr = BGP_ROUTE_ATTR(route);
    if (!attr->has_srv6_l3_service_tlv || !attr->has_srv6_l3_service)
    {
        return FALSE;
    }
    uint16_t expected_behavior =
        (afi == BGP_AFI_IPV4) ? (uint16_t)SRV6_BEHAVIOR_END_DT4 : (uint16_t)SRV6_BEHAVIOR_END_DT6;
    if ((afi != BGP_AFI_IPV4 && afi != BGP_AFI_IPV6) || attr->srv6_behavior != expected_behavior ||
        attr->srv6_sid.family != AF_INET6 || net_addr_is_zero(&attr->srv6_sid) ||
        (attr->has_srv6_sid_structure &&
         (attr->transposition_len != 0u || attr->transposition_offset != 0u || attr->argument_len != 0u)) ||
        !route->has_label || route->label != 3u)
    {
        return FALSE;
    }
    return TRUE;
}

/** MPLS L3VPN 必须有真实的非保留服务标签；Implicit NULL(3) 仅是
 * whole-SID SRv6 VPN NLRI 的占位值，绝不能下成 tunnel + out-label 3。 */
static gboolean mpls_service_route_usable(const bgp_route_node_t *route)
{
    return route && route->has_label && route->label >= 16u;
}

/**
 * @brief 由源 vpnv4 RD 派生导入路径的合成来源地址(每 RD 唯一)
 *
 * 用 AF_INET6 承载 RD：s6_addr[0..7]=RD 原始字节。不同 RD → 不同来源键，使同前缀经多个 RD
 * 导入同一 VRF 时成为各自独立的竞争路径；与该 VRF 内真实 peer(IPv4)来源天然区分。
 */
static void synth_source_from_rd(const bgp_rd_t *rd, net_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->family = AF_INET6;
    memcpy(out->u.v6.s6_addr, rd->bytes, 8);
}

/** 仅释放来源 borrow；调用方需先按旧 key 注销 synthetic nexthop watch。 */
static void import_drop_src_borrow(bgp_route_node_t *route)
{
    if (!route || !route->src_route)
    {
        return;
    }
    bgp_route_node_t *old = route->src_route;
    route->src_route = NULL;
    bgp_route_node_borrow_unref(old);
}

/** protocol teardown 专用：清空 source borrow，但不在全图遍历期间同步 reap。 */
static void import_drop_src_borrow_no_reap(bgp_route_node_t *route)
{
    if (!route || !route->src_route)
    {
        return;
    }
    bgp_route_node_t *old = route->src_route;
    route->src_route = NULL;
    bgp_route_node_borrow_unref_no_reap(old);
}

/** 解除导入/泄漏节点的 synthetic nexthop watch，并清空 src_route borrow */
static void import_detach_src(bgp_route_node_t *route)
{
    if (!route)
    {
        return;
    }
    /* 先按当前 route key 注销 watch，再断 src_route；LOCAL_CROSS 的 watch key 依赖旧 nexthop_id。 */
    bgp_relay_synthetic_nexthop_unregister(route);
    import_drop_src_borrow(route);
}

/** 把 VPN best 作为合成导入路径写入某 VRF 的 unicast RIB */
static void import_upsert(bgp_instance_t *uc, const bgp_nlri_entry_t *uc_nlri, const net_addr_t *synth,
                          const bgp_route_node_t *best)
{
    const bgp_nlri_entry_t *vpn_nlri = (best && best->head) ? &best->head->nlri : NULL;
    const gboolean srv6_be_enabled = uc && BIT_TEST(uc->flags, BGP_INST_FLAG_SRV6_BE);
    const gboolean has_srv6_service_tlv = best && best->attr && BGP_ROUTE_ATTR(best)->has_srv6_l3_service_tlv;
    const gboolean use_srv6 = srv6_be_enabled && srv6_service_route_usable(best, vpn_nlri, uc->afi);
    /* BE 开启后，对方显式携带 Service TLV 却无法构成完整 SID 时
     * 必须 fail closed，不能借同一 NLRI 的 label 静默降级到 MPLS。 */
    const gboolean use_mpls =
        !use_srv6 && mpls_service_route_usable(best) && (!srv6_be_enabled || !has_srv6_service_tlv);

    bgp_rib_t *rib = bgp_inst_rib_ensure_for_nlri(uc, uc_nlri);
    if (!rib)
    {
        return;
    }
    bgp_rthead_t *head = bgp_rib_ensure_head(rib, uc_nlri);
    if (!head)
    {
        return;
    }
    bgp_route_node_t *route = bgp_rthead_lookup_route_mut(head, synth);
    if (!route)
    {
        route = bgp_rthead_create_route(rib, head, synth);
        if (!route)
        {
            return;
        }
    }

    /* 必须在覆盖 attr/nexthop/src_route 前按旧 key 拆 watch。否则同一
     * 合成节点由 MPLS↔SRv6 或 best 切换时，旧 watch 会永久残留。 */
    import_detach_src(route);
    bgp_nexthop_reset_route(route);

    /* 拷贝 best 属性作为合成导入路径 */
    bgp_attr_t attr = *BGP_ROUTE_ATTR(best);
    if (bgp_rib_route_apply_reach(route, ROUTE_PROTOCOL_BGP, &attr) != 0)
    {
        return;
    }
    /* 远端跨表合成路由：清 IMPORT、置 REMOTE_CROSS。来源是 peer 的 VPN
     * 路由（pick_import_source 已跳过本地导出合成），绝不可被 vrf-export 回灌。 */
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS);
    BIT_SET(route->flags, BGP_ROUTE_FLAG_REMOTE_CROSS);
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_SRV6_BE);
    /* 明确清除上一个 incarnation 的服务标签。apply_reach 当前也会
     * 清理，但模式代码不依赖该内部副作用，避免 MPLS→SID/不可用路径残留旧 label。 */
    route->label = 0u;
    route->has_label = 0u;
    route->label_source = BGP_ROUTE_LABEL_SOURCE_NONE;
    if (use_srv6)
    {
        BIT_SET(route->flags, BGP_ROUTE_FLAG_SRV6_BE);
    }

    if (use_mpls)
    {
        /* MPLS 路径：保留对端真实 VPN service label，经 TUNNEL watch 迭代远端 PE。 */
        route->label = best->label;
        route->has_label = 1u;
        route->label_source = BGP_ROUTE_LABEL_SOURCE_RECEIVED;
    }

    route->src_route = (bgp_route_node_t *)best;
    bgp_route_node_borrow_ref((bgp_route_node_t *)best);

    /* 没有可用的完整 service SID，也没有真实 MPLS service label。
     * 典型场景是 srv6-be 未使能却收到 label=3 的 SID-only VPN 路径。
     * 保留合成节点供重评估，但不注册 tunnel watch，并撤销任何旧 FIB incarnation。 */
    if (!use_srv6 && !use_mpls)
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
        if (uc->calc_queue)
        {
            bgp_calc_queue_push(uc->calc_queue, uc, uc_nlri);
        }
        return;
    }

    if (use_srv6)
    {
        /* SRv6 BE：不迭代 MP_REACH 的 PE nexthop，而是为完整 service SID
         * 申请 public IPv6 nexthop 对象。ROUTE 在公网 IPv6 表解析该 SID，
         * relay NH_NOTIFY 决定私网合成路由是否 VALID。 */
        route_nhobj_key_t key;
        memset(&key, 0, sizeof(key));
        key.vrf_id = BGP_VRF_PUBLIC_ID;
        key.protocol = ROUTE_PROTOCOL_BGP;
        key.afi = ROUTE_AFI_IPV6;
        key.nh_type = ROUTE_NH_TYPE_IP;
        key.nexthop = attr.srv6_sid;
        if (bgp_nexthop_set_route_key(route, &key) != ERRCODE_SUCCESS)
        {
            BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
            if (uc->calc_queue)
            {
                bgp_calc_queue_push(uc->calc_queue, uc, uc_nlri);
            }
            return;
        }
    }

    /* MPLS 挂 tunnel watch，SRv6 挂 public IPv6 nexthop watch；未解析均 fail closed。 */
    gboolean nh_resolved = bgp_relay_synthetic_nexthop_register(route);
    if (!nh_resolved)
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
    }

    if (uc->calc_queue)
    {
        bgp_calc_queue_push(uc->calc_queue, uc, uc_nlri);
    }
}

/** 撤销某 VRF unicast RIB 中由指定 RD 导入的合成路径 */
static void import_withdraw(bgp_instance_t *uc, const bgp_nlri_entry_t *uc_nlri, const net_addr_t *synth)
{
    bgp_rib_t *rib = bgp_inst_rib_for_nlri(uc, uc_nlri);
    if (!rib)
    {
        return;
    }
    bgp_rthead_t *head = (bgp_rthead_t *)bgp_rib_lookup_head(rib, uc_nlri);
    bgp_route_node_t *route = head ? bgp_rthead_lookup_route_mut(head, synth) : NULL;
    if (!route)
    {
        return;
    }
    import_detach_src(route);
    if (bgp_rib_unreach_one(rib, uc_nlri, synth) == 1 && uc->calc_queue)
    {
        bgp_calc_queue_push(uc->calc_queue, uc, uc_nlri);
    }
}

/**
 * @brief 选取一个 VPN head 的导入来源路径
 *
 * 导入的"接受"判据是 import-RT 匹配，而非 FIB 下一跳可达性：L3VPN 的 vpnv4 下一跳是远端 PE，
 * 需经隧道/LSP 迭代(本实现的转发部分属后续工作)，此处不要求 BGP_ROUTE_FLAG_VALID，否则在无
 * LSP 的拓扑里收到的 vpnv4 路由永远 Unresolved → 永远无法导入。优先取 VALID best，其次取
 * route_list 中首个非本地导出(LOCAL_CROSS)的收来路径。跳过本地导出合成路径(避免把导出路由再导入成环)，
 * 也跳过 STALE(已撤销待回收)路径——否则源被 withdraw 后仍会被当作来源不断重新导入，撤销撤不掉。
 */
static gboolean import_source_eligible(const bgp_route_node_t *route)
{
    return route && route->attr && !BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) &&
           !BIT_TEST(route->flags, BGP_ROUTE_FLAG_STALE);
}

static const bgp_route_node_t *pick_import_source(bgp_rib_t *src_rib, bgp_rthead_t *head)
{
    const bgp_route_node_t *best = src_rib ? bgp_rib_find_best(src_rib, &head->nlri) : NULL;
    if (import_source_eligible(best))
    {
        return best;
    }
    for (GList *l = head->route_list; l; l = l->next)
    {
        bgp_route_node_t *r = (bgp_route_node_t *)l->data;
        if (import_source_eligible(r))
        {
            return r;
        }
    }
    return NULL;
}

/** 对一个目标私网 unicast instance 应用单个 VPN head 的期望状态。 */
static void reconcile_target(bgp_instance_t *uc, const bgp_route_node_t *best, gboolean eligible, GHashTable *matched,
                             const bgp_nlri_entry_t *uc_nlri, const net_addr_t *synth)
{
    if (!uc || !uc->vrf || uc->vrf->vrf_id == BGP_VRF_PUBLIC_ID || uc->safi != BGP_SAFI_UNICAST ||
        (uc->afi != BGP_AFI_IPV4 && uc->afi != BGP_AFI_IPV6))
    {
        return;
    }

    const vrf_api_af_t *af_cfg = vrf_api_cache_get_af(uc->vrf->vrf_id, (uint16_t)uc->afi);
    gboolean want = eligible && af_cfg && matched && g_hash_table_contains(matched, GUINT_TO_POINTER(uc->vrf->vrf_id));
    if (want)
    {
        /* 绝不因 IRT 命中自动创建私网 AF：只有已存在的
         * unicast instance 才允许导入。 */
        import_upsert(uc, uc_nlri, synth, best);
    }
    else
    {
        import_withdraw(uc, uc_nlri, synth);
    }
}

/** 对一个 VPN head：把来源路径导入命中同 AF IRT 的 VRF。
 * target_uc 非 NULL 时只重评该目标，供 srv6-be 运行态切换使用。 */
static void reconcile_head_internal(bgp_instance_t *vpn_inst, bgp_rthead_t *head, bgp_instance_t *target_uc)
{
    bgp_protocol_t *proto = bgp_proto();
    if (!vpn_inst || !head || !proto || !proto->vrf_hash)
    {
        return;
    }

    /* 来源路径(IRT 匹配即接受，不要求 FIB 可达；跳过本地导出合成路径防环) */
    bgp_rib_t *src_rib = bgp_inst_rib_for_nlri(vpn_inst, &head->nlri);
    const bgp_route_node_t *best = pick_import_source(src_rib, head);
    gboolean eligible = (best != NULL);

    GHashTable *matched = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (eligible)
    {
        (void)collect_matched_vrfs(BGP_ROUTE_ATTR(best), vpn_inst->afi, matched);
    }

    bgp_nlri_entry_t uc_nlri;
    derive_unicast_nlri(&head->nlri, &uc_nlri);
    net_addr_t synth;
    synth_source_from_rd(&head->nlri.prefix.rd, &synth);

    bgp_afi_t uc_afi = vpn_inst->afi;

    if (target_uc)
    {
        if (target_uc->afi == uc_afi)
        {
            reconcile_target(target_uc, best, eligible, matched, &uc_nlri, &synth);
        }
        g_hash_table_destroy(matched);
        return;
    }

    GHashTableIter it;
    gpointer k = NULL;
    gpointer v = NULL;
    g_hash_table_iter_init(&it, proto->vrf_hash);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)v;
        if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID)
        {
            continue;
        }
        bgp_instance_t *uc =
            vrf->inst_hash
                ? (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(uc_afi, BGP_SAFI_UNICAST))
                : NULL;
        reconcile_target(uc, best, eligible, matched, &uc_nlri, &synth);
    }
    g_hash_table_destroy(matched);
}

static void reconcile_head(bgp_instance_t *vpn_inst, bgp_rthead_t *head)
{
    reconcile_head_internal(vpn_inst, head, NULL);
}

void bgp_vrf_import_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head)
{
    if (!src_inst || !head || !is_public_vpn_inst(src_inst))
    {
        return;
    }
    reconcile_head(src_inst, head);
}

void bgp_vrf_import_on_vpn_received(const bgp_nlri_entry_t *vpn_nlri)
{
    if (!vpn_nlri || (vpn_nlri->afi != BGP_AFI_IPV4 && vpn_nlri->afi != BGP_AFI_IPV6) ||
        vpn_nlri->safi != BGP_SAFI_VPN_UNICAST)
    {
        return;
    }
    bgp_instance_t *vpn_inst = vpn_inst_lookup((bgp_afi_t)vpn_nlri->afi);
    if (!vpn_inst || !vpn_inst->calc_queue)
    {
        return;
    }
    /* 推 calc：calc_run_one 即便走 all-invalid 分支也会调用 bgp_vrf_import_on_calc_done */
    (void)bgp_calc_queue_push(vpn_inst->calc_queue, vpn_inst, vpn_nlri);
}

/* ============================================================================
 * backfill（import-RT 配置变更后重评估已有 VPN 路由）
 * ========================================================================== */

/** g_tree_foreach 回调：收集 head 指针(避免遍历期触发的 reconcile 影响 tree 迭代) */
static gboolean collect_head_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    GList **pp = (GList **)user_data;
    *pp = g_list_prepend(*pp, value);
    return FALSE;
}

static void backfill_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer ud)
{
    (void)entry;
    bgp_instance_t *target_uc = (bgp_instance_t *)ud;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (head)
        {
            reconcile_head_internal(inst, head, target_uc);
        }
    }
    g_list_free(heads);
}

void bgp_vrf_import_backfill_afi(bgp_afi_t afi)
{
    bgp_instance_t *vpn_inst = vpn_inst_lookup(afi);
    if (!vpn_inst)
    {
        return;
    }
    bgp_inst_foreach_rib(vpn_inst, backfill_rib_cb, NULL);
}

int bgp_vrf_import_reprocess_target_inst(bgp_instance_t *uc)
{
    if (!uc || !uc->vrf || uc->vrf->vrf_id == BGP_VRF_PUBLIC_ID || uc->safi != BGP_SAFI_UNICAST ||
        (uc->afi != BGP_AFI_IPV4 && uc->afi != BGP_AFI_IPV6))
    {
        return -1;
    }

    bgp_instance_t *vpn_inst = vpn_inst_lookup(uc->afi);
    if (!vpn_inst)
    {
        return 0;
    }
    bgp_inst_foreach_rib(vpn_inst, backfill_rib_cb, uc);
    return 0;
}

void bgp_vrf_import_backfill(void)
{
    bgp_vrf_import_backfill_afi(BGP_AFI_IPV4);
    bgp_vrf_import_backfill_afi(BGP_AFI_IPV6);
}

/* ============================================================================
 * public VPN instance 销毁前清理（撤销所有导入到各 VRF 的合成路径）
 * ========================================================================== */

typedef struct
{
    bgp_instance_t *vpn_inst; /**< 即将销毁的源 VPN instance */
    GPtrArray *victims;       /**< 收集到的导入节点(其 src_route 来自 vpn_inst) */
} purge_ctx_t;

static void collect_victims_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer ud)
{
    (void)inst;
    (void)entry;
    purge_ctx_t *ctx = (purge_ctx_t *)ud;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (!head)
        {
            continue;
        }
        for (GList *r = head->route_list; r; r = r->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)r->data;
            /* src_route->head 此刻仍存活(源 VPN inst 尚未销毁 RIB) */
            if (route && route->src_route && route->src_route->head && route->src_route->head->inst == ctx->vpn_inst)
            {
                g_ptr_array_add(ctx->victims, route);
            }
        }
    }
    g_list_free(heads);
}

void bgp_vrf_import_purge_target_inst(bgp_instance_t *vpn_inst)
{
    if (!is_public_vpn_inst(vpn_inst))
    {
        return;
    }
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || !proto->vrf_hash)
    {
        return;
    }

    GPtrArray *victims = g_ptr_array_new();
    purge_ctx_t ctx = {.vpn_inst = vpn_inst, .victims = victims};

    GHashTableIter it;
    gpointer k = NULL;
    gpointer v = NULL;
    g_hash_table_iter_init(&it, proto->vrf_hash);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)v;
        if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID || !vrf->inst_hash)
        {
            continue;
        }
        bgp_instance_t *uc =
            (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(vpn_inst->afi, BGP_SAFI_UNICAST));
        if (uc)
        {
            bgp_inst_foreach_rib(uc, collect_victims_cb, &ctx);
        }
    }

    for (guint i = 0; i < victims->len; i++)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)g_ptr_array_index(victims, i);
        if (!route || !route->head)
        {
            continue;
        }
        bgp_instance_t *uc = route->head->inst;
        bgp_rib_t *rib = uc ? bgp_inst_rib_for_nlri(uc, &route->head->nlri) : NULL;
        net_addr_t synth = route->source;
        bgp_nlri_entry_t nlri = route->head->nlri;
        import_detach_src(route);
        if (rib && bgp_rib_unreach_one(rib, &nlri, &synth) == 1 && uc->calc_queue)
        {
            bgp_calc_queue_push(uc->calc_queue, uc, &nlri);
        }
    }
    if (victims->len > 0)
    {
        LOG_INFO("BGP vrf-import: purged %u imported routes (VPN afi=%u inst teardown)", victims->len,
                 (unsigned)vpn_inst->afi);
    }
    g_ptr_array_free(victims, TRUE);
}

/** 私网 unicast instance 销毁时，解除其 REMOTE_CROSS 节点对公网 VPN route 的借用。 */
static void import_detach_target_unicast_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib,
                                            gpointer user_data)
{
    (void)inst;
    (void)entry;
    (void)user_data;
    if (!rib || !rib->head_tree)
    {
        return;
    }

    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (!head)
        {
            continue;
        }
        for (GList *r = head->route_list; r; r = r->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)r->data;
            if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_REMOTE_CROSS))
            {
                import_detach_src(route);
            }
        }
    }
    g_list_free(heads);
}

void bgp_vrf_import_purge_target_unicast_inst(bgp_instance_t *inst)
{
    if (!inst || !inst->vrf || inst->vrf->vrf_id == BGP_VRF_PUBLIC_ID || inst->safi != BGP_SAFI_UNICAST ||
        (inst->afi != BGP_AFI_IPV4 && inst->afi != BGP_AFI_IPV6))
    {
        return;
    }
    bgp_inst_foreach_rib(inst, import_detach_target_unicast_cb, NULL);
}

/** protocol 全量析构 pre-pass：只解除跨表 route 对 source route 的 borrow。
 *
 * 这里不做逐 route watch 注销或普通 source unref；全图析构统一采用 no-reap
 * 降计数，watch 由紧随其后的 bgp_relay_cleanup() 回收，避免遍历期间改动 RIB。 */
static void import_detach_cross_route_sources_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib,
                                                 gpointer user_data)
{
    (void)inst;
    (void)entry;
    (void)user_data;
    if (!rib || !rib->head_tree)
    {
        return;
    }

    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        for (GList *r = head ? head->route_list : NULL; r; r = r->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)r->data;
            if (route && (BIT_TEST(route->flags, BGP_ROUTE_FLAG_REMOTE_CROSS) ||
                          BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS)))
            {
                import_drop_src_borrow_no_reap(route);
            }
        }
    }
    g_list_free(heads);
}

void bgp_vrf_import_detach_cross_route_sources(bgp_instance_t *inst)
{
    if (inst)
    {
        bgp_inst_foreach_rib(inst, import_detach_cross_route_sources_cb, NULL);
    }
}

/* ============================================================================
 * VRF 本地交叉（local route leaking）：本机 VRF→VRF 直接泄漏
 *
 * 复用上面的 IRT 索引：源在本机（某私网 VRF unicast best），按源 VRF 的 export-RT 直接命中
 * 其它私网 VRF 的 import-RT，命中即把该前缀作为 LOCAL_CROSS 合成路径插入目标 VRF unicast RIB。
 * 与 vpnv4 导入对称（reconcile / borrow / detach 约定一致），但：源是本机 unicast（非远端 vpnv4）、
 * 转发不走隧道（目标路由自有 nexthop 对象在源 VRF 内递归解析）、单跳不传递。
 * ========================================================================== */

/** 由源私网 VRF 派生本地交叉合成来源键（每源 VRF 唯一，与 REMOTE_CROSS 的 RD 键、真实 peer 键不冲突）。
 *  用 AF_INET6 承载：s6_addr[8]=0x4C('L') 标记 + s6_addr[12..15]=htonl(src_vrf_id)。
 *  REMOTE_CROSS 的 synth_source_from_rd 把 RD 放在 s6_addr[0..7] 且 s6_addr[8..15]=0，故二者天然区分。 */
static void synth_source_from_local_vrf(uint32_t src_vrf_id, net_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    out->family = AF_INET6;
    out->u.v6.s6_addr[8] = 0x4Cu; /* 'L' 标记，区别于 RD 键 */
    uint32_t be = htonl(src_vrf_id);
    memcpy(&out->u.v6.s6_addr[12], &be, 4);
}

/** 收集源 VRF 的 export-RT 命中的目标私网 VRF 到 set（排除源自身与 public）；返回目标数 */
static guint local_collect_targets(uint32_t src_vrf_id, GHashTable *set)
{
    if (!g_bgp_irt_index || src_vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return 0;
    }
    const vrf_api_af_t *af = vrf_api_cache_get_af(src_vrf_id, VRF_AFI_IPV4);
    if (!af || !af->export_rts || af->export_rt_count == 0)
    {
        return 0;
    }
    for (uint16_t i = 0; i < af->export_rt_count; i++)
    {
        bgp_irt_key_t key;
        if (!irt_key_build(&key, BGP_AFI_IPV4, &af->export_rts[i]))
        {
            continue;
        }
        GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, &key);
        if (!inner)
        {
            continue;
        }
        GHashTableIter it;
        gpointer k = NULL;
        gpointer v = NULL;
        g_hash_table_iter_init(&it, inner);
        while (g_hash_table_iter_next(&it, &k, &v))
        {
            uint32_t tgt = GPOINTER_TO_UINT(k);
            if (tgt == src_vrf_id || tgt == BGP_VRF_PUBLIC_ID)
            {
                continue; /* 不泄漏给自己/public */
            }
            g_hash_table_add(set, k);
        }
    }
    return g_hash_table_size(set);
}

/** 选取本地交叉的泄漏源：VALID best，且跳过 LOCAL_CROSS/REMOTE_CROSS（单跳防环，不再外泄）。 */
static const bgp_route_node_t *local_pick_source(bgp_rib_t *src_rib, bgp_rthead_t *head)
{
    const bgp_route_node_t *best = src_rib ? bgp_rib_find_best(src_rib, &head->nlri) : NULL;
    if (!best || !BIT_TEST(best->flags, BGP_ROUTE_FLAG_VALID))
    {
        return NULL;
    }
    if (BIT_TEST(best->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) || BIT_TEST(best->flags, BGP_ROUTE_FLAG_REMOTE_CROSS))
    {
        return NULL;
    }
    return best;
}

/** 把源 unicast best 作为 LOCAL_CROSS 合成路径写入目标 VRF unicast RIB，并在源 VRF 内迭代其 nexthop */
static void local_upsert(bgp_instance_t *tgt_uc, const bgp_nlri_entry_t *uc_nlri, const net_addr_t *synth,
                         const bgp_route_node_t *src_best)
{
    bgp_rib_t *rib = bgp_inst_rib_ensure_for_nlri(tgt_uc, uc_nlri);
    if (!rib)
    {
        return;
    }
    bgp_rthead_t *head = bgp_rib_ensure_head(rib, uc_nlri);
    if (!head)
    {
        return;
    }
    bgp_route_node_t *route = bgp_rthead_lookup_route_mut(head, synth);
    if (!route)
    {
        route = bgp_rthead_create_route(rib, head, synth);
        if (!route)
        {
            return;
        }
    }

    /* 先拆旧 LOCAL_CROSS watch/nexthop。该路由会重新生成一个 nexthop 对象并注册给 ROUTE，
     * 若先 reset nexthop_id，旧 watch 将无法按原 key unregister。 */
    gboolean src_changed = (route->src_route != src_best) ? TRUE : FALSE;
    bgp_relay_synthetic_nexthop_unregister(route);
    bgp_nexthop_reset_route(route);
    if (src_changed)
    {
        import_drop_src_borrow(route);
    }

    /* 拷贝源 best 属性作为合成泄漏路径（不带标签；本地交叉无 VPN 标签） */
    bgp_attr_t attr = *BGP_ROUTE_ATTR(src_best);
    if (bgp_rib_route_apply_reach(route, ROUTE_PROTOCOL_MAX, &attr) != 0)
    {
        return;
    }
    /* 本地跨表合成路由：清 IMPORT、置 LOCAL_CROSS（本地起源、可正常通告；单跳防环不再外泄）。 */
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
    BIT_SET(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS);
    if (BIT_TEST(src_best->flags, BGP_ROUTE_FLAG_LOCAL_DELIVERY))
    {
        BIT_SET(route->flags, BGP_ROUTE_FLAG_LOCAL_DELIVERY);
    }
    else
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_LOCAL_DELIVERY);
    }

    /* 维护溯源前向指针：best 切换来源时换 borrow 引用（钉住源直到本泄漏撤销）。
     * 必须在申请 nexthop 之前设好 src_route：bgp_nexthop_make_route_key 对 LOCAL_CROSS 路由
     * 经 src_route 取「源 VRF」作 nexthop 对象的迭代 VRF。 */
    if (src_changed)
    {
        route->src_route = (bgp_route_node_t *)src_best;
        bgp_route_node_borrow_ref((bgp_route_node_t *)src_best);
    }

    /* 转发(本地交叉，nexthop-vrf 模型，复用 BGP 现成下一跳迭代流程)：给泄漏路由生成 BGP 下一跳对象，
     * 其迭代 VRF=源 VRF1(由 make_route_key 经 src_route 决定)。ROUTE 在源 VRF1 迭代该对象→解析出真实
     * 出接口/网关→NH_NOTIFY 回 BGP→calc/flush 把路由装进目标 VRF2 FIB。下一跳必须继承源 best 的原始
     * BGP 下一跳；若源路由无可用下一跳，则保持未解析，不能用目的前缀伪造递归目标。 */
    route_nhobj_key_t src_nh_key;
    memset(&src_nh_key, 0, sizeof(src_nh_key));
    if (bgp_nexthop_get_route_key(src_best, &src_nh_key) != ERRCODE_SUCCESS || src_nh_key.nexthop.family == 0 ||
        (src_nh_key.nh_type == ROUTE_NH_TYPE_IP && net_addr_is_zero(&src_nh_key.nexthop)))
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
        if (tgt_uc->calc_queue)
        {
            bgp_calc_queue_push(tgt_uc->calc_queue, tgt_uc, uc_nlri);
        }
        return;
    }

    route_nhobj_key_t target_nh_key;
    bgp_nexthop_make_route_key(route, &src_nh_key.nexthop, &target_nh_key);
    if (bgp_nexthop_set_route_key(route, &target_nh_key) != ERRCODE_SUCCESS)
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
        if (tgt_uc->calc_queue)
        {
            bgp_calc_queue_push(tgt_uc->calc_queue, tgt_uc, uc_nlri);
        }
        return;
    }
    if (target_nh_key.nh_type == ROUTE_NH_TYPE_BLACKHOLE)
    {
        BIT_SET(route->flags, BGP_ROUTE_FLAG_VALID);
        if (tgt_uc->calc_queue)
        {
            bgp_calc_queue_push(tgt_uc->calc_queue, tgt_uc, uc_nlri);
        }
        return;
    }

    /* 注册迭代 watch（IP 路由按 nexthop_id 注册，ROUTE 在 nexthop 对象的 vrf=源 VRF1 里迭代）。
     * 未解析则置 invalid，待 ROUTE NH_NOTIFY 解析后由 relay 重评/重刷。 */
    gboolean nh_resolved = bgp_relay_synthetic_nexthop_register(route);
    if (!nh_resolved)
    {
        BIT_CLR(route->flags, BGP_ROUTE_FLAG_VALID);
    }

    if (tgt_uc->calc_queue)
    {
        bgp_calc_queue_push(tgt_uc->calc_queue, tgt_uc, uc_nlri);
    }
}

/** 对一个源 unicast head：按源 VRF export-RT 泄漏到命中的本机 VRF，撤销不再命中的 */
static void local_reconcile_head(bgp_instance_t *src_inst, bgp_rthead_t *head)
{
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || !proto->vrf_hash || !src_inst->vrf)
    {
        return;
    }
    uint32_t src_vrf_id = src_inst->vrf->vrf_id;

    bgp_rib_t *src_rib = bgp_inst_rib_for_nlri(src_inst, &head->nlri);
    const bgp_route_node_t *best = local_pick_source(src_rib, head);

    GHashTable *targets = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (best)
    {
        (void)local_collect_targets(src_vrf_id, targets);
    }
    {
        char nbuf[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(&head->nlri, nbuf, sizeof(nbuf));
        LOG_DEBUG("BGP local-cross reconcile: src_vrf=%u %s best=%s targets=%u", src_vrf_id, nbuf, best ? "yes" : "no",
                  best ? g_hash_table_size(targets) : 0);
    }

    /* 目标 NLRI = 源 unicast NLRI（同前缀、afi/safi 不变） */
    const bgp_nlri_entry_t *uc_nlri = &head->nlri;
    net_addr_t synth;
    synth_source_from_local_vrf(src_vrf_id, &synth);
    bgp_afi_t uc_afi = src_inst->afi;

    GHashTableIter it;
    gpointer k = NULL;
    gpointer v = NULL;
    g_hash_table_iter_init(&it, proto->vrf_hash);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)v;
        if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID || vrf->vrf_id == src_vrf_id)
        {
            continue; /* 不泄漏给 public / 自己 */
        }
        gboolean want = best && g_hash_table_contains(targets, GUINT_TO_POINTER(vrf->vrf_id));
        if (want)
        {
            bgp_instance_t *uc = bgp_vrf_get_or_create_instance(vrf, uc_afi, BGP_SAFI_UNICAST);
            if (uc)
            {
                local_upsert(uc, uc_nlri, &synth, best);
            }
        }
        else
        {
            bgp_instance_t *uc =
                vrf->inst_hash
                    ? (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(uc_afi, BGP_SAFI_UNICAST))
                    : NULL;
            if (uc)
            {
                import_withdraw(uc, uc_nlri, &synth);
            }
        }
    }
    g_hash_table_destroy(targets);
}

/** 该 instance 是否为私网 VRF 的 ipv4-unicast（本地交叉的源/目标载体） */
static gboolean is_private_unicast_inst(const bgp_instance_t *inst)
{
    return inst && inst->vrf && inst->vrf->vrf_id != BGP_VRF_PUBLIC_ID && inst->afi == BGP_AFI_IPV4 &&
           inst->safi == BGP_SAFI_UNICAST;
}

void bgp_vrf_import_local_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head)
{
    if (!src_inst || !head || !is_private_unicast_inst(src_inst))
    {
        return;
    }
    local_reconcile_head(src_inst, head);
}

/** foreach_rib 回调：对一个 unicast inst 的所有 head 重新做本地交叉 reconcile */
static void local_reconcile_rib_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer ud)
{
    (void)entry;
    (void)ud;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (head)
        {
            local_reconcile_head(inst, head);
        }
    }
    g_list_free(heads);
}

/** 重扫某私网 VRF 的 ipv4-unicast RIB 全部 head（源侧重评估） */
static void local_reconcile_vrf(uint32_t vrf_id)
{
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return;
    }
    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, vrf_id);
    if (!vrf || !vrf->inst_hash)
    {
        return;
    }
    bgp_instance_t *uc =
        (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_UNICAST));
    if (uc)
    {
        bgp_inst_foreach_rib(uc, local_reconcile_rib_cb, NULL);
    }
}

void bgp_vrf_import_local_backfill_target_vrf(uint32_t tgt_vrf_id)
{
    (void)tgt_vrf_id; /* 全量重评：所有源 VRF 的路由都可能命中变更后的目标 import-RT */
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || !proto->vrf_hash)
    {
        return;
    }
    GHashTableIter it;
    gpointer k = NULL;
    gpointer v = NULL;
    g_hash_table_iter_init(&it, proto->vrf_hash);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)v;
        if (vrf && vrf->vrf_id != BGP_VRF_PUBLIC_ID)
        {
            local_reconcile_vrf(vrf->vrf_id);
        }
    }
}

void bgp_vrf_import_local_backfill_source_vrf(uint32_t src_vrf_id)
{
    local_reconcile_vrf(src_vrf_id);
}

/** purge_inst 角色(a)：detach 本 inst 内所有 LOCAL_CROSS 泄漏节点对源的 borrow */
static void local_detach_self_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer ud)
{
    (void)inst;
    (void)entry;
    (void)ud;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (!head)
        {
            continue;
        }
        for (GList *r = head->route_list; r; r = r->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)r->data;
            if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS))
            {
                import_detach_src(route);
            }
        }
    }
    g_list_free(heads);
}

/** purge_inst 角色(b)：收集其它 VRF 中由 ctx->src_inst 泄漏出去的 LOCAL_CROSS 节点 */
static void local_collect_sourced_cb(bgp_instance_t *inst, bgp_rd_entry_t *entry, bgp_rib_t *rib, gpointer ud)
{
    (void)inst;
    (void)entry;
    purge_ctx_t *ctx = (purge_ctx_t *)ud;
    if (!rib || !rib->head_tree)
    {
        return;
    }
    GList *heads = NULL;
    g_tree_foreach(rib->head_tree, collect_head_cb, &heads);
    for (GList *l = heads; l; l = l->next)
    {
        bgp_rthead_t *head = (bgp_rthead_t *)l->data;
        if (!head)
        {
            continue;
        }
        for (GList *r = head->route_list; r; r = r->next)
        {
            bgp_route_node_t *route = (bgp_route_node_t *)r->data;
            if (route && BIT_TEST(route->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) && route->src_route &&
                route->src_route->head && route->src_route->head->inst == ctx->vpn_inst)
            {
                g_ptr_array_add(ctx->victims, route);
            }
        }
    }
    g_list_free(heads);
}

void bgp_vrf_import_local_purge_inst(bgp_instance_t *inst)
{
    if (!is_private_unicast_inst(inst))
    {
        return;
    }
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || !proto->vrf_hash)
    {
        return;
    }

    /* 角色(a)：本 inst 作为目标——detach 自身泄漏节点对其它 VRF 源的 borrow（节点随本 inst RIB 销毁） */
    bgp_inst_foreach_rib(inst, local_detach_self_cb, NULL);

    /* 角色(b)：本 inst 作为源——撤销其它 VRF 中由本 inst 泄漏出去的合成节点（此刻本 inst RIB 仍存活） */
    GPtrArray *victims = g_ptr_array_new();
    purge_ctx_t ctx = {.vpn_inst = inst, .victims = victims};

    GHashTableIter it;
    gpointer k = NULL;
    gpointer v = NULL;
    g_hash_table_iter_init(&it, proto->vrf_hash);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)v;
        if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID || vrf->vrf_id == inst->vrf->vrf_id || !vrf->inst_hash)
        {
            continue;
        }
        bgp_instance_t *uc =
            (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_UNICAST));
        if (uc)
        {
            bgp_inst_foreach_rib(uc, local_collect_sourced_cb, &ctx);
        }
    }

    for (guint i = 0; i < victims->len; i++)
    {
        bgp_route_node_t *route = (bgp_route_node_t *)g_ptr_array_index(victims, i);
        if (!route || !route->head)
        {
            continue;
        }
        bgp_instance_t *uc = route->head->inst;
        bgp_rib_t *rib = uc ? bgp_inst_rib_for_nlri(uc, &route->head->nlri) : NULL;
        net_addr_t synth = route->source;
        bgp_nlri_entry_t nlri = route->head->nlri;
        import_detach_src(route);
        if (rib && bgp_rib_unreach_one(rib, &nlri, &synth) == 1 && uc->calc_queue)
        {
            bgp_calc_queue_push(uc->calc_queue, uc, &nlri);
        }
    }
    if (victims->len > 0)
    {
        LOG_INFO("BGP vrf local-cross: purged %u leaked routes sourced from VRF %u (inst teardown)", victims->len,
                 inst->vrf->vrf_id);
    }
    g_ptr_array_free(victims, TRUE);
}
