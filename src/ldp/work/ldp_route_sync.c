/**
 * @file   ldp_route_sync.c
 * @brief  ROUTE 订阅 + TUNNEL candidate 联动
 *
 * M5 简化策略（已被 M6 扩展，保留作为历史记录）：
 *   - 仅订阅 IPv4 单播，VRF=ROUTE_VRF_DEFAULT，全协议
 *   - 学到路由 → 通过 TUNNEL 申请 local label，写 local LIB，向所有 OPERATIONAL peer 通告 Label Mapping
 *   - 撤销路由 → Label Withdraw，释放 label
 *   - 收到 peer Label Mapping：查找对应路由，若 nexthop 与 peer 关联则注册 tunnel candidate
 *
 * M6 调整：
 *   - 学到/撤销路由只处理 host route（IPv4 /32），过滤直连子网与默认路由
 *   - peer 与 route nexthop 的匹配扩展为三选一：
 *       a) nexthop == peer LSR-ID（M5 原规则，保留兼容）
 *       b) nexthop == peer.peer_transport_v4（hello 中通告的 transport）
 *       c) nexthop == peer.peer_link_addr_v4（hello 报文源 IP，即对端直连接口）
 *     实际拓扑中 IGP 注入的 nexthop 通常是 (c)，由此让 candidate 在常规组网下能注册
 *
 * @author jhb
 * @date   2026/05/06
 */
#include "ldp_route_sync.h"

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

#include "errcode.h"
#include "ldp.h"
#include "ldp_main.h"
#include "ldp_pkt.h"
#include "ldp_session.h"
#include "log.h"
#include "route.h"
#include "tunnel.h"

#define LDP_LABEL_TIMEOUT_MS 3000u

static GHashTable *g_routes = NULL; /* key=ldp_fec_t* → ldp_learned_route_t* */
static int g_route_subscribed = 0;

// ============================================================================
// 内部 helper
// ============================================================================

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

static void send_label_mapping_all_peers(uint32_t prefix, uint8_t prefix_len, uint32_t label);
static void send_label_withdraw_all_peers(uint32_t prefix, uint8_t prefix_len);

/**
 * @brief M6 peer ↔ nexthop 匹配：检查给定 peer 是否承载该 nexthop。
 *
 * 三种命中方式（host order 比较）：
 *   - peer_lsr_id == nexthop（M5 原规则，保留以兼容 transport == LSR-ID 的场景）
 *   - peer.peer_transport_v4 == nexthop（hello 中通告的 transport）
 *   - peer.peer_link_addr_v4 == nexthop（hello 报文源 IP，即对端直连接口）
 *
 * @return 1 命中 / 0 不命中
 */
static int peer_matches_nexthop(uint32_t peer_lsr_id, uint16_t peer_label_space, uint32_t nexthop_v4)
{
    if (nexthop_v4 == 0u)
    {
        return 0;
    }
    if (peer_lsr_id == nexthop_v4)
    {
        return 1;
    }
    ldp_peer_t *peer = ldp_session_lookup(peer_lsr_id, peer_label_space);
    if (!peer)
    {
        return 0;
    }
    if (peer->peer_transport_v4 == nexthop_v4)
    {
        return 1;
    }
    if (peer->peer_link_addr_v4 == nexthop_v4)
    {
        return 1;
    }
    return 0;
}

static void fill_label_req(tunnel_label_req_t *req, const ldp_learned_route_t *route)
{
    memset(req, 0, sizeof(*req));
    req->vrf_id = route->vrf_id;
    req->afi = ROUTE_AFI_IPV4;
    req->source_type = TUNNEL_SOURCE_LDP;
    req->owner_module_id = DEV_MODULE_ID_LDP;
    req->owner_id = ((uint32_t)route->fec.prefix_len << 24) | (route->fec.prefix & 0x00FFFFFFu);
    req->out_ifindex = route->out_ifindex;
    req->fec.vrf_id = route->vrf_id;
    req->fec.afi = ROUTE_AFI_IPV4;
    req->fec.prefix_len = route->fec.prefix_len;
    req->fec.addr.family = AF_INET;
    req->fec.addr.u.v4.s_addr = htonl(route->fec.prefix);
}

static void fill_candidate(tunnel_candidate_t *cand, const ldp_learned_route_t *route, uint32_t remote_label)
{
    memset(cand, 0, sizeof(*cand));
    cand->vrf_id = route->vrf_id;
    cand->afi = ROUTE_AFI_IPV4;
    cand->source_type = TUNNEL_SOURCE_LDP;
    cand->preference = (uint8_t)LDP_TUNNEL_PREFERENCE;
    cand->owner_module_id = DEV_MODULE_ID_LDP;
    cand->owner_id = ((uint32_t)route->fec.prefix_len << 24) | (route->fec.prefix & 0x00FFFFFFu);
    cand->fec.vrf_id = route->vrf_id;
    cand->fec.afi = ROUTE_AFI_IPV4;
    cand->fec.prefix_len = route->fec.prefix_len;
    cand->fec.addr.family = AF_INET;
    cand->fec.addr.u.v4.s_addr = htonl(route->fec.prefix);
    cand->endpoint = cand->fec.addr;
    cand->nexthop.family = AF_INET;
    cand->nexthop.u.v4.s_addr = htonl(route->nexthop_v4);
    cand->relay_addr = cand->nexthop;
    cand->out_ifindex = route->out_ifindex;
    if (remote_label == LDP_LABEL_IMPLICIT_NULL)
    {
        cand->label_count = 0u;
    }
    else
    {
        cand->label_count = 1u;
        cand->labels[0] = remote_label;
    }
}

// ============================================================================
// 初始化 / 订阅
// ============================================================================

void ldp_route_sync_init(void)
{
    if (!g_routes)
    {
        g_routes = g_hash_table_new_full(fec_hash, fec_equal, g_free, g_free);
    }
}

void ldp_route_sync_subscribe(void)
{
    if (g_route_subscribed)
    {
        return;
    }

    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    route_subscribe_req_t *req = g_malloc0(sizeof(*req));
    req->protocol = ROUTE_PROTOCOL_MAX; /* 全协议 */
    req->vrf_id = ROUTE_VRF_DEFAULT;
    req->afi = ROUTE_AFI_IPV4;
    req->flags = ROUTE_SUBSCRIBE_FLAG_FULL;

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_SUBSCRIBE, DEV_MODULE_ID_LDP, DEV_MODULE_ID_ROUTE, 0,
                                                    req, sizeof(*req), g_free);
    if (msg)
    {
        dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, msg);
        dev_ipc_message_free(msg);
        g_route_subscribed = 1;
        LOG_INFO("LDP: subscribed to ROUTE (IPv4 unicast, default VRF)");
    }
}

void ldp_route_sync_unsubscribe(void)
{
    if (!g_route_subscribed)
    {
        return;
    }

    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    route_subscribe_req_t *req = g_malloc0(sizeof(*req));
    req->protocol = ROUTE_PROTOCOL_MAX;
    req->vrf_id = ROUTE_VRF_DEFAULT;
    req->afi = ROUTE_AFI_IPV4;

    dev_ipc_message_t *msg = dev_ipc_message_create(ROUTE_MSG_TYPE_UNSUBSCRIBE, DEV_MODULE_ID_LDP, DEV_MODULE_ID_ROUTE,
                                                    0, req, sizeof(*req), g_free);
    if (msg)
    {
        dev_ipc_send(ctx, DEV_MODULE_ID_ROUTE, msg);
        dev_ipc_message_free(msg);
        g_route_subscribed = 0;
        LOG_INFO("LDP: unsubscribed from ROUTE (IPv4 unicast, default VRF)");
    }
}

void ldp_route_sync_cleanup(void)
{
    ldp_route_sync_unsubscribe();
    if (g_routes)
    {
        g_hash_table_destroy(g_routes);
        g_routes = NULL;
    }
    g_route_subscribed = 0;
}

GHashTable *ldp_route_sync_routes_table(void)
{
    return g_routes;
}

// ============================================================================
// 学习/撤销路由
// ============================================================================

static void learn_route(const route_msg_entry_t *e)
{
    if (!g_routes || !e)
    {
        return;
    }
    if (e->afi != ROUTE_AFI_IPV4 || e->safi != ROUTE_SAFI_UNICAST)
    {
        return;
    }
    /* M6: 只为 IPv4 host route (/32) 申请 LSP；过滤直连子网、默认路由等 */
    if (e->prefix_len != 32u)
    {
        return;
    }
    if (e->flags & ROUTE_ENTRY_FLAG_NO_ADV)
    {
        return;
    }
    ldp_fec_t fec = {.prefix = ntohl(e->prefix_addr.u.v4.s_addr), .prefix_len = e->prefix_len};

    ldp_learned_route_t *r = (ldp_learned_route_t *)g_hash_table_lookup(g_routes, &fec);
    if (!r)
    {
        ldp_fec_t *kheap = g_malloc(sizeof(*kheap));
        *kheap = fec;
        r = g_malloc0(sizeof(*r));
        r->fec = fec;
        r->vrf_id = e->vrf_id;
        g_hash_table_insert(g_routes, kheap, r);
    }

    /* 取迭代后下一跳，否则原始 nexthop */
    const net_addr_t *nh = (e->iter_nexthop_addr.family == AF_INET) ? &e->iter_nexthop_addr : &e->nexthop_addr;
    if (nh->family == AF_INET)
    {
        r->nexthop_v4 = ntohl(nh->u.v4.s_addr);
    }
    else
    {
        r->nexthop_v4 = 0u;
    }
    r->out_ifindex = (e->iter_out_ifindex != 0u) ? e->iter_out_ifindex : e->out_ifindex;

    /* 申请 local label（如未分配） */
    if (r->local_label == 0u)
    {
        if (e->protocol == ROUTE_PROTOCOL_CONNECTED)
        {
            r->local_label = LDP_LABEL_IMPLICIT_NULL;
            ldp_lib_set_local_label(&fec, r->local_label);
            send_label_mapping_all_peers(fec.prefix, fec.prefix_len, r->local_label);
            return;
        }
        tunnel_label_req_t req;
        fill_label_req(&req, r);
        uint32_t label = 0u;
        if (tunnel_rpc_label_alloc(ldp_local_ipc_ctx(), &req, &label, LDP_LABEL_TIMEOUT_MS) == ERRCODE_SUCCESS &&
            label != 0u)
        {
            r->local_label = label;
            ldp_lib_set_local_label(&fec, label);
            send_label_mapping_all_peers(fec.prefix, fec.prefix_len, label);
        }
        else
        {
            LOG_WARN("LDP: tunnel label_alloc failed for FEC 0x%08X/%u", fec.prefix, fec.prefix_len);
        }
    }

    /* 若已存在 remote label，可能现在 nexthop 才解析出来 → 重新评估 candidate */
    if (g_ldp_work_local && g_ldp_work_local->peers && r->nexthop_v4 != 0u)
    {
        GHashTable *remote = ldp_lib_remote_table();
        if (remote)
        {
            GHashTableIter it;
            gpointer k = NULL, v = NULL;
            g_hash_table_iter_init(&it, remote);
            while (g_hash_table_iter_next(&it, &k, &v))
            {
                ldp_remote_label_t *rl = (ldp_remote_label_t *)v;
                if (!rl || !fec_equal(&rl->fec, &fec))
                {
                    continue;
                }
                if (peer_matches_nexthop(rl->peer_lsr_id, rl->peer_label_space, r->nexthop_v4))
                {
                    tunnel_candidate_t cand;
                    fill_candidate(&cand, r, rl->label);
                    (void)tunnel_rpc_candidate_add(ldp_local_ipc_ctx(), &cand);
                }
            }
        }
    }
}

static void withdraw_route(const route_msg_entry_t *e)
{
    if (!g_routes || !e)
    {
        return;
    }
    if (e->afi != ROUTE_AFI_IPV4 || e->safi != ROUTE_SAFI_UNICAST)
    {
        return;
    }
    if (e->prefix_len != 32u)
    {
        return;
    }
    ldp_fec_t fec = {.prefix = ntohl(e->prefix_addr.u.v4.s_addr), .prefix_len = e->prefix_len};
    ldp_learned_route_t *r = (ldp_learned_route_t *)g_hash_table_lookup(g_routes, &fec);
    if (!r)
    {
        return;
    }

    /* 撤销 candidate（按当前 nexthop 推断） */
    if (r->nexthop_v4 != 0u)
    {
        GHashTable *remote = ldp_lib_remote_table();
        if (remote)
        {
            GHashTableIter it;
            gpointer k = NULL, v = NULL;
            g_hash_table_iter_init(&it, remote);
            while (g_hash_table_iter_next(&it, &k, &v))
            {
                ldp_remote_label_t *rl = (ldp_remote_label_t *)v;
                if (!rl || !fec_equal(&rl->fec, &fec))
                {
                    continue;
                }
                if (peer_matches_nexthop(rl->peer_lsr_id, rl->peer_label_space, r->nexthop_v4))
                {
                    tunnel_candidate_t cand;
                    fill_candidate(&cand, r, rl->label);
                    (void)tunnel_rpc_candidate_del(ldp_local_ipc_ctx(), &cand);
                }
            }
        }
    }

    /* 释放本地 label */
    if (r->local_label != 0u)
    {
        send_label_withdraw_all_peers(fec.prefix, fec.prefix_len);
        if (r->local_label != LDP_LABEL_IMPLICIT_NULL)
        {
            tunnel_label_req_t req;
            fill_label_req(&req, r);
            (void)tunnel_rpc_label_release(ldp_local_ipc_ctx(), &req);
        }
        ldp_lib_free_local_label(&fec);
    }

    g_hash_table_remove(g_routes, &fec);
}

void ldp_route_sync_on_route_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return;
    }

    if (msg->msg_type == ROUTE_MSG_TYPE_REPORT)
    {
        const route_msg_report_t *rpt = (const route_msg_report_t *)msg->payload;
        size_t hdr_sz = sizeof(*rpt);
        if (msg->payload_len < hdr_sz)
        {
            return;
        }
        for (uint32_t i = 0; i < rpt->route_count; i++)
        {
            const route_msg_entry_t *e = &rpt->routes[i];
            if (e->is_withdraw)
            {
                withdraw_route(e);
            }
            else
            {
                learn_route(e);
            }
        }
    }
    else if (msg->msg_type == ROUTE_MSG_TYPE_UPDATE)
    {
        if (msg->payload_len < sizeof(route_msg_entry_t))
        {
            return;
        }
        const route_msg_entry_t *e = (const route_msg_entry_t *)msg->payload;
        if (e->is_withdraw)
        {
            withdraw_route(e);
        }
        else
        {
            learn_route(e);
        }
    }
}

// ============================================================================
// session 钩子
// ============================================================================

static void send_label_mapping_all_peers(uint32_t prefix, uint8_t prefix_len, uint32_t label)
{
    if (!g_ldp_work_local || !g_ldp_work_local->peers)
    {
        return;
    }
    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->peers);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        ldp_peer_t *p = (ldp_peer_t *)val;
        if (!p || p->state != LDP_PEER_OPERATIONAL)
        {
            continue;
        }
        uint8_t buf[64];
        int n = ldp_pkt_encode_label_mapping(g_ldp_work_local->proto.lsr_id, 0u, ++p->next_msg_id, prefix, prefix_len,
                                             label, buf, sizeof(buf));
        if (n > 0)
        {
            (void)send(p->fd, buf, (size_t)n, MSG_NOSIGNAL);
            p->last_tx_msec = ldp_worker_now_msec();
        }
    }
}

static void send_label_withdraw_all_peers(uint32_t prefix, uint8_t prefix_len)
{
    if (!g_ldp_work_local || !g_ldp_work_local->peers)
    {
        return;
    }
    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->peers);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        ldp_peer_t *p = (ldp_peer_t *)val;
        if (!p || p->state != LDP_PEER_OPERATIONAL)
        {
            continue;
        }
        uint8_t buf[64];
        int n = ldp_pkt_encode_label_withdraw(g_ldp_work_local->proto.lsr_id, 0u, ++p->next_msg_id, prefix, prefix_len,
                                              buf, sizeof(buf));
        if (n > 0)
        {
            (void)send(p->fd, buf, (size_t)n, MSG_NOSIGNAL);
            p->last_tx_msec = ldp_worker_now_msec();
        }
    }
}

void ldp_route_sync_on_session_up(uint32_t peer_lsr_id, uint16_t peer_label_space)
{
    (void)peer_label_space;
    /* 把所有已知 FEC 的 local label 通告给新 peer */
    if (!g_routes)
    {
        return;
    }
    ldp_peer_t *p = ldp_session_lookup(peer_lsr_id, peer_label_space);
    if (!p || p->state != LDP_PEER_OPERATIONAL)
    {
        return;
    }
    GHashTableIter it;
    gpointer k = NULL, v = NULL;
    g_hash_table_iter_init(&it, g_routes);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        ldp_learned_route_t *r = (ldp_learned_route_t *)v;
        if (!r || r->local_label == 0u)
        {
            continue;
        }
        uint8_t buf[64];
        int n = ldp_pkt_encode_label_mapping(g_ldp_work_local->proto.lsr_id, 0u, ++p->next_msg_id, r->fec.prefix,
                                             r->fec.prefix_len, r->local_label, buf, sizeof(buf));
        if (n > 0)
        {
            (void)send(p->fd, buf, (size_t)n, MSG_NOSIGNAL);
            p->last_tx_msec = ldp_worker_now_msec();
        }
    }
}

void ldp_route_sync_on_session_down(uint32_t peer_lsr_id, uint16_t peer_label_space)
{
    /* session 关闭后，candidate 已通过 ldp_lib_purge_peer 清掉的 remote 表里没了；
     * 仍需主动撤销已注册的 candidate */
    if (!g_routes)
    {
        return;
    }
    /* 此时 peer 尚未从表中移除（close 在 remove 之前），仍可解析其 transport / link_addr */
    GHashTableIter it;
    gpointer k = NULL, v = NULL;
    g_hash_table_iter_init(&it, g_routes);
    while (g_hash_table_iter_next(&it, &k, &v))
    {
        ldp_learned_route_t *r = (ldp_learned_route_t *)v;
        if (!r || !peer_matches_nexthop(peer_lsr_id, peer_label_space, r->nexthop_v4))
        {
            continue;
        }
        tunnel_candidate_t cand;
        fill_candidate(&cand, r, 0u);
        (void)tunnel_rpc_candidate_del(ldp_local_ipc_ctx(), &cand);
    }
}

void ldp_route_sync_on_remote_label(uint32_t peer_lsr_id, uint16_t peer_label_space, const ldp_fec_t *fec,
                                    uint32_t label)
{
    if (!fec || !g_routes)
    {
        return;
    }
    ldp_learned_route_t *r = (ldp_learned_route_t *)g_hash_table_lookup(g_routes, fec);
    if (!r || !peer_matches_nexthop(peer_lsr_id, peer_label_space, r->nexthop_v4))
    {
        return;
    }
    tunnel_candidate_t cand;
    fill_candidate(&cand, r, label);
    (void)tunnel_rpc_candidate_add(ldp_local_ipc_ctx(), &cand);
}

void ldp_route_sync_on_remote_label_withdraw(uint32_t peer_lsr_id, uint16_t peer_label_space, const ldp_fec_t *fec)
{
    if (!fec || !g_routes)
    {
        return;
    }
    ldp_learned_route_t *r = (ldp_learned_route_t *)g_hash_table_lookup(g_routes, fec);
    if (!r || !peer_matches_nexthop(peer_lsr_id, peer_label_space, r->nexthop_v4))
    {
        return;
    }
    tunnel_candidate_t cand;
    fill_candidate(&cand, r, 0u);
    (void)tunnel_rpc_candidate_del(ldp_local_ipc_ctx(), &cand);
}
