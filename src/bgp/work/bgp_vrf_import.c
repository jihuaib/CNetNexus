/**
 * @file   bgp_vrf_import.c
 * @brief  vpnv4 路由按 import-RT 导入私网 VRF 实现：IRT 索引 + 入向过滤 + 导入 reconcile
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
#include "vrf.h"

/* IRT 索引：规范化 8 字节 RT(g_malloc 持有) → inner GHashTable<vrf_id → refcount>。
 * 仅在 BGP worker 线程内访问，无需加锁。 */
static GHashTable *g_bgp_irt_index;

/* ============================================================================
 * IRT 索引
 * ========================================================================== */

static guint rt_key_hash(gconstpointer p)
{
    const uint8_t *b = (const uint8_t *)p;
    guint h = 2166136261u; /* FNV-1a 32 */
    for (int i = 0; i < 8; i++)
    {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static gboolean rt_key_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, 8) == 0;
}

void bgp_vrf_import_init(void)
{
    if (g_bgp_irt_index)
    {
        return;
    }
    g_bgp_irt_index = g_hash_table_new_full(rt_key_hash, rt_key_equal, g_free, (GDestroyNotify)g_hash_table_destroy);
}

void bgp_vrf_import_cleanup(void)
{
    if (g_bgp_irt_index)
    {
        g_hash_table_destroy(g_bgp_irt_index);
        g_bgp_irt_index = NULL;
    }
}

void bgp_vrf_import_irt_add(uint32_t vrf_id, const vrf_rt_t *rt)
{
    if (!g_bgp_irt_index || vrf_id == BGP_VRF_PUBLIC_ID || !rt)
    {
        return;
    }
    uint8_t key[8];
    if (!bgp_ext_community_rt_canon(rt, key))
    {
        return;
    }

    GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, key);
    if (!inner)
    {
        inner = g_hash_table_new(g_direct_hash, g_direct_equal);
        uint8_t *kdup = (uint8_t *)g_malloc(8);
        memcpy(kdup, key, 8);
        g_hash_table_insert(g_bgp_irt_index, kdup, inner);
    }
    guint cnt = GPOINTER_TO_UINT(g_hash_table_lookup(inner, GUINT_TO_POINTER(vrf_id)));
    g_hash_table_insert(inner, GUINT_TO_POINTER(vrf_id), GUINT_TO_POINTER(cnt + 1));
}

void bgp_vrf_import_irt_del(uint32_t vrf_id, const vrf_rt_t *rt)
{
    if (!g_bgp_irt_index || !rt)
    {
        return;
    }
    uint8_t key[8];
    if (!bgp_ext_community_rt_canon(rt, key))
    {
        return;
    }
    GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, key);
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
        g_hash_table_remove(g_bgp_irt_index, key); /* 触发 inner 销毁 + key free */
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

void bgp_vrf_import_purge_all(void)
{
    if (g_bgp_irt_index)
    {
        g_hash_table_remove_all(g_bgp_irt_index);
    }
}

/** 收集 attr 中所有 RT 命中的私网 vrf_id 到 set(key=GUINT_TO_POINTER(vrf_id))；返回命中条数 */
static guint collect_matched_vrfs(const bgp_attr_t *attr, GHashTable *set)
{
    if (!attr || !g_bgp_irt_index)
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
        GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, ec + off);
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

gboolean bgp_vrf_import_attr_has_match(const bgp_attr_t *attr)
{
    if (!attr || !g_bgp_irt_index)
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
        GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, ec + off);
        if (inner && g_hash_table_size(inner) > 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/* ============================================================================
 * 导入 reconcile
 * ========================================================================== */

static bgp_protocol_t *bgp_proto(void)
{
    return g_bgp_work_local ? g_bgp_work_local->protocol : NULL;
}

/** 当前 public vpnv4 instance(导入源)，未使能返回 NULL */
static bgp_instance_t *vpn_inst_lookup(void)
{
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
    return (bgp_instance_t *)g_hash_table_lookup(pub->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_VPN_UNICAST));
}

void bgp_vrf_import_request_refresh(void)
{
    bgp_instance_t *vpn_inst = vpn_inst_lookup();
    if (!vpn_inst || !vpn_inst->peer_hash || !vpn_inst->vrf)
    {
        return; /* vpnv4 未使能 */
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
            (void)bgp_pkt_send_route_refresh(sess->pri_conn, (uint16_t)BGP_AFI_IPV4, (uint8_t)BGP_SAFI_VPN_UNICAST);
            sent++;
        }
    }
    if (sent > 0)
    {
        LOG_INFO("BGP vrf-import: import-RT changed, sent vpnv4 ROUTE-REFRESH to %u peer(s)", sent);
    }
}

/** 是否为 public vpnv4(ipv4 vpn-unicast)instance —— 导入子系统只对它生效 */
static gboolean is_public_vpn_inst(const bgp_instance_t *inst)
{
    return inst && inst->vrf && inst->vrf->vrf_id == BGP_VRF_PUBLIC_ID && inst->afi == BGP_AFI_IPV4 &&
           inst->safi == BGP_SAFI_VPN_UNICAST;
}

/** vpnv4 NLRI → 私网 unicast NLRI(剥 RD/标签) */
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

/** 把 vpnv4 best 作为合成导入路径写入某 VRF 的 unicast RIB */
static void import_upsert(bgp_instance_t *uc, const bgp_nlri_entry_t *uc_nlri, const net_addr_t *synth,
                          const bgp_route_node_t *best, const bgp_nlri_entry_t *vpn_nlri)
{
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

    /* 拷贝 best 属性作为合成导入路径 */
    bgp_attr_t attr = *BGP_ROUTE_ATTR(best);
    bgp_nexthop_reset_route(route);
    if (bgp_rib_route_apply_reach(route, ROUTE_PROTOCOL_BGP, &attr) != 0)
    {
        return;
    }
    /* 远端跨表合成路由：清 IMPORT、置 REMOTE_CROSS。来源是 peer 的 vpnv4（pick_import_source
     * 已跳过本地导出合成），非本地起源，绝不可被 vrf-export 回灌 vpnv4（防环）。 */
    BIT_CLR(route->flags, BGP_ROUTE_FLAG_IMPORT);
    BIT_SET(route->flags, BGP_ROUTE_FLAG_REMOTE_CROSS);

    /* 携带 received VPN 标签，供后续基于标签的隧道转发迭代(转发实现属后续工作) */
    if (vpn_nlri && vpn_nlri->type == BGP_NLRI_PREFIX && vpn_nlri->prefix.has_label)
    {
        route->label = vpn_nlri->prefix.label;
        route->has_label = 1u;
        route->label_source = BGP_ROUTE_LABEL_SOURCE_RECEIVED;
    }

    /* 维护溯源前向指针：best 切换来源时换 borrow 引用 */
    if (route->src_route != best)
    {
        import_detach_src(route);
        route->src_route = (bgp_route_node_t *)best;
        bgp_route_node_borrow_ref((bgp_route_node_t *)best);
    }

    /* 注册自有隧道 watch：下一跳（远端 PE）在公网表迭代命中 eBGP-vpnv4 假隧道。
     * 隧道未解析则置 invalid（不优选/不下刷），待 TUNNEL notify 解析后由 relay 重评。 */
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
 * @brief 选取一个 vpnv4 head 的导入来源路径
 *
 * 导入的"接受"判据是 import-RT 匹配，而非 FIB 下一跳可达性：L3VPN 的 vpnv4 下一跳是远端 PE，
 * 需经隧道/LSP 迭代(本实现的转发部分属后续工作)，此处不要求 BGP_ROUTE_FLAG_VALID，否则在无
 * LSP 的拓扑里收到的 vpnv4 路由永远 Unresolved → 永远无法导入。优先取 VALID best，其次取
 * route_list 中首个非本地导出(LOCAL_CROSS)的收来路径。跳过本地导出合成路径(避免把导出路由再导入成环)，
 * 也跳过 STALE(已撤销待回收)路径——否则源被 withdraw 后仍会被当作来源不断重新导入，撤销撤不掉。
 */
static const bgp_route_node_t *pick_import_source(bgp_rib_t *src_rib, bgp_rthead_t *head)
{
    const bgp_route_node_t *best = src_rib ? bgp_rib_find_best(src_rib, &head->nlri) : NULL;
    if (best && !BIT_TEST(best->flags, BGP_ROUTE_FLAG_LOCAL_CROSS))
    {
        return best;
    }
    for (GList *l = head->route_list; l; l = l->next)
    {
        bgp_route_node_t *r = (bgp_route_node_t *)l->data;
        if (r && !BIT_TEST(r->flags, BGP_ROUTE_FLAG_LOCAL_CROSS) && !BIT_TEST(r->flags, BGP_ROUTE_FLAG_STALE))
        {
            return r;
        }
    }
    return NULL;
}

/** 对一个 vpnv4 head：把来源路径导入命中 IRT 的 VRF，撤销不再命中的 VRF */
static void reconcile_head(bgp_instance_t *vpn_inst, bgp_rthead_t *head)
{
    bgp_protocol_t *proto = bgp_proto();
    if (!proto || !proto->vrf_hash)
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
        (void)collect_matched_vrfs(BGP_ROUTE_ATTR(best), matched);
    }

    bgp_nlri_entry_t uc_nlri;
    derive_unicast_nlri(&head->nlri, &uc_nlri);
    net_addr_t synth;
    synth_source_from_rd(&head->nlri.prefix.rd, &synth);

    bgp_afi_t uc_afi = vpn_inst->afi; /* 当前仅 ipv4 */

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
        gboolean want = eligible && g_hash_table_contains(matched, GUINT_TO_POINTER(vrf->vrf_id));
        if (want)
        {
            bgp_instance_t *uc = bgp_vrf_get_or_create_instance(vrf, uc_afi, BGP_SAFI_UNICAST);
            if (uc)
            {
                import_upsert(uc, &uc_nlri, &synth, best, &head->nlri);
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
                import_withdraw(uc, &uc_nlri, &synth);
            }
        }
    }
    g_hash_table_destroy(matched);
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
    if (!vpn_nlri || vpn_nlri->afi != BGP_AFI_IPV4 || vpn_nlri->safi != BGP_SAFI_VPN_UNICAST)
    {
        return;
    }
    bgp_instance_t *vpn_inst = vpn_inst_lookup();
    if (!vpn_inst || !vpn_inst->calc_queue)
    {
        return;
    }
    /* 推 calc：calc_run_one 即便走 all-invalid 分支也会调用 bgp_vrf_import_on_calc_done */
    (void)bgp_calc_queue_push(vpn_inst->calc_queue, vpn_inst, vpn_nlri);
}

/* ============================================================================
 * backfill（import-RT 配置变更后重评估已有 vpnv4 路由）
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
            reconcile_head(inst, head);
        }
    }
    g_list_free(heads);
}

void bgp_vrf_import_backfill(void)
{
    bgp_instance_t *vpn_inst = vpn_inst_lookup();
    if (!vpn_inst)
    {
        return; /* vpnv4 未使能 */
    }
    bgp_inst_foreach_rib(vpn_inst, backfill_rib_cb, NULL);
}

/* ============================================================================
 * public vpnv4 instance 销毁前清理（撤销所有导入到各 VRF 的合成路径）
 * ========================================================================== */

typedef struct
{
    bgp_instance_t *vpn_inst; /**< 即将销毁的源 vpnv4 instance */
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
            /* src_route->head 此刻仍存活(源 vpnv4 inst 尚未销毁 RIB) */
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
            (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(BGP_AFI_IPV4, BGP_SAFI_UNICAST));
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
        LOG_INFO("BGP vrf-import: purged %u imported routes (vpnv4 inst teardown)", victims->len);
    }
    g_ptr_array_free(victims, TRUE);
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
    const vrf_api_af_t *af = vrf_api_cache_get_af(src_vrf_id, VRF_AFI_IPV4, VRF_SAFI_UNICAST);
    if (!af || !af->export_rts || af->export_rt_count == 0)
    {
        return 0;
    }
    for (uint16_t i = 0; i < af->export_rt_count; i++)
    {
        uint8_t key[8];
        if (!bgp_ext_community_rt_canon(&af->export_rts[i], key))
        {
            continue;
        }
        GHashTable *inner = (GHashTable *)g_hash_table_lookup(g_bgp_irt_index, key);
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
