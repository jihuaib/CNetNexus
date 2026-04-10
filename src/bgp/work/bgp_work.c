/**
 * @file   bgp_work.c
 * @brief  BGP 路由处理工作队列实现
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_work.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bgp_calc.h"
#include "bgp_fsm.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

// ============================================================================
// 发布队列内部辅助
// ============================================================================

typedef struct bgp_pub_item
{
    bgp_nlri_entry_t nlri;
} bgp_pub_item_t;

static int bgp_work_process_calc_event(bgp_instance_t *inst, gboolean allow_reschedule);
static int bgp_work_process_route_flush_event(bgp_instance_t *inst, gboolean allow_reschedule);
static int bgp_work_process_session_pub_event(bgp_instance_t *inst, gboolean allow_reschedule);
static void bgp_work_schedule_calc(bgp_instance_t *inst);
static void bgp_work_schedule_route_flush(bgp_instance_t *inst);
static void bgp_work_schedule_session_pub(bgp_instance_t *inst);

static const char *bgp_work_conn_state_name(const bgp_session_t *sess, const bgp_conn_t *conn)
{
    if (!conn || conn->fd < 0)
    {
        return "Idle";
    }
    if (conn->is_connecting)
    {
        return "Connect";
    }

    if (!sess || conn != sess->pri_conn)
    {
        return "Collision";
    }

    return bgp_fsm_state_str(sess->fsm_state);
}

static gboolean bgp_session_is_publish_ready(const bgp_session_t *sess)
{
    return sess && sess->pri_conn && sess->pri_conn->fd >= 0 && sess->fsm_state == BGP_FSM_STATE_ESTABLISHED;
}

static uint32_t bgp_work_local_as_number(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        return 0u;
    }
    return g_bgp_work_local->protocol->as_number;
}

static gboolean bgp_as_path_contains_as(const char *as_path, uint32_t asn)
{
    if (!as_path || as_path[0] == '\0' || asn == 0u)
    {
        return FALSE;
    }

    const char *p = as_path;
    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t' || *p == '{' || *p == '}' || *p == ',')
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }

        char *end = NULL;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p)
        {
            p++;
            continue;
        }
        if ((uint32_t)v == asn)
        {
            return TRUE;
        }
        p = end;
    }

    return FALSE;
}

static const bgp_session_t *bgp_best_source_session(const bgp_route_node_t *best)
{
    if (!best || BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT) || !best->head || !best->head->inst ||
        !best->head->inst->vrf)
    {
        return NULL;
    }
    return bgp_vrf_find_session(best->head->inst->vrf, &best->source);
}

static gboolean bgp_best_can_publish_to_session(const bgp_session_t *sess, const bgp_route_node_t *best)
{
    if (!sess || !best)
    {
        return FALSE;
    }

    /* Split-horizon: 不向“该最优路径的来源邻居”回灌同一路由。
     * import 路由无来源邻居，不触发 split-horizon。 */
    if (!BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT) && net_addr_equal(&best->source, &sess->neighbor_addr))
    {
        return FALSE;
    }

    /* AS_PATH 防环：若目标邻居 AS 已在路径中，禁止发布。 */
    if (sess->remote_as != 0u && bgp_as_path_contains_as(BGP_ROUTE_ATTR(best)->as_path, sess->remote_as))
    {
        return FALSE;
    }

    /* iBGP split-horizon：iBGP 学到的路由不再发给 iBGP 邻居。 */
    if (sess->sess_type == BGP_SESS_TYPE_IBGP && !BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT))
    {
        const bgp_session_t *src_sess = bgp_best_source_session(best);
        if (src_sess && src_sess->sess_type == BGP_SESS_TYPE_IBGP)
        {
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean bgp_prepare_update_attr(const bgp_session_t *sess, const bgp_route_node_t *best, bgp_attr_t *send_attr)
{
    if (!best || !send_attr)
    {
        return FALSE;
    }

    memcpy(send_attr, &best->attr, sizeof(*send_attr));

    if (sess->sess_type != BGP_SESS_TYPE_EBGP)
    {
        return TRUE;
    }
    uint32_t local_as = bgp_work_local_as_number();

    int n = 0;
    if (BGP_ROUTE_ATTR(best)->as_path[0] != '\0')
    {
        n = g_snprintf(send_attr->as_path, sizeof(send_attr->as_path), "%u %s", local_as,
                       BGP_ROUTE_ATTR(best)->as_path);
    }
    else
    {
        n = g_snprintf(send_attr->as_path, sizeof(send_attr->as_path), "%u", local_as);
    }

    if (n < 0 || (size_t)n >= sizeof(send_attr->as_path))
    {
        char peer[64];
        net_addr_to_str(&sess->neighbor_addr, peer, sizeof(peer));
        LOG_WARN("BGP: skip UPDATE to %s: AS_PATH prepend overflow(local_as=%u)", peer, local_as);
        return FALSE;
    }

    return TRUE;
}

static void bgp_prepare_update_nexthop(const bgp_session_t *sess, const bgp_route_node_t *best,
                                       bgp_nexthop_t *send_nexthop)
{
    if (!best || !send_nexthop)
    {
        return;
    }

    memcpy(send_nexthop, &best->nexthop, sizeof(*send_nexthop));

    if (!BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT) || !sess || !sess->pri_conn)
    {
        return;
    }

    net_addr_t local_addr;
    if (bgp_conn_get_local_addr(sess->pri_conn, &local_addr) != 0 || local_addr.family == 0 ||
        net_addr_is_zero(&local_addr))
    {
        return;
    }

    /* 同族 nexthop 替换（传统场景） */
    if (local_addr.family == send_nexthop->global.family || send_nexthop->global.family == 0)
    {
        send_nexthop->global = local_addr;
        send_nexthop->has_link_local = false;
        memset(&send_nexthop->link_local, 0, sizeof(send_nexthop->link_local));
        return;
    }

    /* 双栈场景：IPv6 路由通过 IPv4 peer 发送时，使用本地 IPv4 地址作为 nexthop。 */
    if (local_addr.family == AF_INET && send_nexthop->global.family == AF_INET6)
    {
        send_nexthop->global = local_addr;
        send_nexthop->has_link_local = false;
        memset(&send_nexthop->link_local, 0, sizeof(send_nexthop->link_local));
        return;
    }

    /* RFC 8950：IPv4 路由通过 IPv6 peer 发送时，使用本地 IPv6 地址作为 nexthop。 */
    if (local_addr.family == AF_INET6 && send_nexthop->global.family == AF_INET &&
        BIT_TEST(sess->negotiated_caps, BGP_SESS_CAP_EXT_NEXTHOP))
    {
        send_nexthop->global = local_addr;
        send_nexthop->has_link_local = false;
        memset(&send_nexthop->link_local, 0, sizeof(send_nexthop->link_local));
    }
}

static gboolean bgp_work_on_worker_thread(void)
{
    return g_bgp_work_local && g_bgp_work_local->worker_thread != 0 &&
           pthread_equal(pthread_self(), g_bgp_work_local->worker_thread);
}

static void bgp_pub_item_free(gpointer data, gpointer user_data)
{
    (void)user_data;
    g_free(data);
}

/** 遍历 peer_hash 时的 WITHDRAW 发送上下文 */
typedef struct
{
    bgp_instance_t *inst;
    const bgp_nlri_entry_t *nlri; /**< 待撤销的 NLRI（借用引用） */
} withdraw_send_ctx_t;

/** g_hash_table_foreach 回调：向各 ESTABLISHED 对端发送 WITHDRAW */
static void foreach_withdraw_send(gpointer key, gpointer value, gpointer user_data)
{
    (void)value;
    const net_addr_t *addr = key;
    withdraw_send_ctx_t *ctx = user_data;

    bgp_session_t *sess = bgp_vrf_find_session(ctx->inst->vrf, addr);
    if (!sess || !sess->pri_conn || sess->pri_conn->fd < 0 || sess->fsm_state != BGP_FSM_STATE_ESTABLISHED)
    {
        return;
    }

    bgp_pkt_send_withdraw(sess->pri_conn, ctx->nlri);
}

typedef struct
{
    bgp_instance_t *inst;
    const bgp_nlri_entry_t *nlri;
    uint32_t queued;
} enqueue_established_ctx_t;

static void foreach_enqueue_established(gpointer key, gpointer value, gpointer user_data)
{
    (void)value;
    const net_addr_t *addr = key;
    enqueue_established_ctx_t *ctx = (enqueue_established_ctx_t *)user_data;
    bgp_session_t *sess = bgp_vrf_find_session(ctx->inst->vrf, addr);
    if (!bgp_session_is_publish_ready(sess) || !sess->pub_queue)
    {
        return;
    }

    if (bgp_pub_queue_push(sess->pub_queue, ctx->nlri) == 0)
    {
        ctx->queued++;
    }
}

static int route_node_to_route_entry(uint32_t vrf_id, const bgp_nlri_entry_t *nlri, const bgp_route_node_t *route,
                                     route_msg_entry_t *entry_out)
{
    if (!nlri || !route || !entry_out)
    {
        return 0;
    }
    if (nlri->type != BGP_NLRI_PREFIX || nlri->safi != BGP_SAFI_UNICAST)
    {
        return 0;
    }

    const net_addr_t *prefix = &nlri->prefix.prefix.addr;
    if (prefix->family != AF_INET && prefix->family != AF_INET6)
    {
        return 0;
    }

    uint8_t max_len = (prefix->family == AF_INET) ? 32u : 128u;
    if (nlri->prefix.prefix.prefix_len > max_len)
    {
        return 0;
    }

    /* 允许跨族 nexthop/source（双栈场景：IPv4 前缀 + IPv6 nexthop/source, RFC 8950） */
    if (route->nexthop.global.family != AF_INET && route->nexthop.global.family != AF_INET6)
    {
        return 0;
    }

    net_addr_t iter_nh;
    memset(&iter_nh, 0, sizeof(iter_nh));
    uint32_t iter_oif = 0u;
    if (route->iter_watched && route->iter_resolved)
    {
        if (route->iter_relay_addr.family == AF_INET || route->iter_relay_addr.family == AF_INET6)
        {
            iter_nh = route->iter_relay_addr;
        }
        iter_oif = route->iter_out_ifindex;
    }

    int32_t metric = 0;
    if (BGP_ROUTE_ATTR(route)->has_med)
    {
        metric = (BGP_ROUTE_ATTR(route)->med > (uint32_t)INT32_MAX) ? INT32_MAX : (int32_t)BGP_ROUTE_ATTR(route)->med;
    }

    memset(entry_out, 0, sizeof(*entry_out));
    entry_out->vrf_id = vrf_id;
    entry_out->afi = (prefix->family == AF_INET) ? ROUTE_AFI_IPV4 : ROUTE_AFI_IPV6;
    entry_out->safi = ROUTE_SAFI_UNICAST;
    entry_out->prefix_len = nlri->prefix.prefix.prefix_len;
    entry_out->protocol = ROUTE_PROTOCOL_BGP;
    entry_out->metric = metric;
    entry_out->preference = ROUTE_ADMIN_DIST_BGP;
    entry_out->is_withdraw = 0u;
    entry_out->flags = 0u;
    entry_out->out_ifindex = 0u;
    entry_out->iter_out_ifindex = iter_oif;
    entry_out->prefix_addr = *prefix;
    entry_out->nexthop_addr = route->nexthop.global;
    entry_out->iter_nexthop_addr = iter_nh;
    entry_out->source_addr = route->source;
    return 1;
}

// ============================================================================
// 优选队列
// ============================================================================

bgp_calc_queue_t *bgp_calc_queue_create(void)
{
    bgp_calc_queue_t *q = g_malloc0(sizeof(bgp_calc_queue_t));
    q->q = g_queue_new();
    return q;
}

void bgp_calc_queue_destroy(bgp_calc_queue_t *q, bgp_instance_t *inst)
{
    if (!q)
    {
        return;
    }
    bgp_rthead_t *head = NULL;
    while ((head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        if (inst && inst->rib)
        {
            bgp_rib_head_unref(head);
        }
    }
    g_queue_free(q->q);
    g_free(q);
}

int bgp_calc_queue_push(bgp_calc_queue_t *q, bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!q || !inst || !inst->rib || !nlri)
    {
        return -1;
    }

    bgp_rthead_t *head = bgp_rib_ensure_head(inst->rib, nlri);
    if (!head)
    {
        return -1;
    }

    bgp_rib_head_ref(head);
    g_queue_push_tail(q->q, head);
    q->count++;
    bgp_work_schedule_calc(inst);
    return 0;
}

int bgp_calc_queue_process(bgp_calc_queue_t *q, bgp_instance_t *inst, int batch_size)
{
    if (!q || !inst || batch_size <= 0)
    {
        return 0;
    }
    int processed = 0;
    bgp_rthead_t *head = NULL;
    while (processed < batch_size && (head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        q->count--;
        bgp_calc_run_one(inst, &head->nlri);
        bgp_rib_head_unref(head);
        processed++;
    }
    if (processed > 0)
    {
        LOG_DEBUG("BGP: calc_queue afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", (unsigned)inst->afi,
                  (unsigned)inst->safi, processed, q->count);
    }
    return processed;
}

// ============================================================================
// 发布队列
// ============================================================================

bgp_pub_queue_t *bgp_pub_queue_create(void)
{
    bgp_pub_queue_t *q = g_malloc0(sizeof(bgp_pub_queue_t));
    q->q = g_queue_new();
    return q;
}

void bgp_pub_queue_destroy(bgp_pub_queue_t *q)
{
    if (!q)
    {
        return;
    }
    bgp_pub_queue_clear(q);
    if (q->q)
    {
        g_queue_free(q->q);
        q->q = NULL;
    }
    g_free(q);
}

void bgp_pub_queue_clear(bgp_pub_queue_t *q)
{
    if (!q || !q->q)
    {
        return;
    }

    g_queue_foreach(q->q, bgp_pub_item_free, NULL);
    g_queue_clear(q->q);
    q->count = 0;
}

void bgp_pub_queue_drop_instance(bgp_pub_queue_t *q, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!q || !q->q)
    {
        return;
    }

    for (GList *l = q->q->head; l;)
    {
        GList *next = l->next;
        bgp_pub_item_t *item = (bgp_pub_item_t *)l->data;
        if (item && item->nlri.afi == afi && item->nlri.safi == safi)
        {
            g_queue_delete_link(q->q, l);
            bgp_pub_item_free(item, NULL);
            if (q->count > 0)
            {
                q->count--;
            }
        }
        l = next;
    }
}

uint32_t bgp_pub_queue_count_for_instance(const bgp_pub_queue_t *q, const bgp_instance_t *inst)
{
    if (!q || !q->q || !inst)
    {
        return 0u;
    }

    uint32_t count = 0u;
    for (GList *l = q->q->head; l; l = l->next)
    {
        const bgp_pub_item_t *item = (const bgp_pub_item_t *)l->data;
        if (item && item->nlri.afi == inst->afi && item->nlri.safi == inst->safi)
        {
            count++;
        }
    }
    return count;
}

int bgp_pub_queue_push(bgp_pub_queue_t *q, const bgp_nlri_entry_t *nlri)
{
    if (!q || !q->q || !nlri)
    {
        return -1;
    }

    bgp_pub_item_t *item = g_malloc(sizeof(*item));
    if (!item)
    {
        return -1;
    }

    memcpy(&item->nlri, nlri, sizeof(*nlri));
    g_queue_push_tail(q->q, item);
    q->count++;
    return 0;
}

int bgp_pub_queue_process(bgp_pub_queue_t *q, bgp_session_t *sess, bgp_instance_t *inst, int batch_size)
{
    if (!q || !q->q || !sess || !inst || batch_size <= 0)
    {
        return 0;
    }

    if (!bgp_session_is_publish_ready(sess))
    {
        uint32_t pending = bgp_pub_queue_count_for_instance(q, inst);
        if (pending > 0)
        {
            char addr_str[64];
            net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
            LOG_WARN(
                "BGP: neighbor=%s afi=%u safi=%u pub_queue stalled: pending=%u but primary is not publish-ready "
                "(pri fd=%d state=%s active=%d connecting=%d, sec fd=%d state=%s active=%d connecting=%d)",
                addr_str, (unsigned)inst->afi, (unsigned)inst->safi, pending, sess->pri_conn ? sess->pri_conn->fd : -1,
                bgp_work_conn_state_name(sess, sess->pri_conn), (sess->pri_conn && sess->pri_conn->is_active) ? 1 : 0,
                (sess->pri_conn && sess->pri_conn->is_connecting) ? 1 : 0, sess->sec_conn ? sess->sec_conn->fd : -1,
                bgp_work_conn_state_name(sess, sess->sec_conn), (sess->sec_conn && sess->sec_conn->is_active) ? 1 : 0,
                (sess->sec_conn && sess->sec_conn->is_connecting) ? 1 : 0);
        }
        return 0;
    }

    if (!g_hash_table_lookup(inst->peer_hash, &sess->neighbor_addr))
    {
        bgp_pub_queue_drop_instance(q, inst->afi, inst->safi);
        return 0;
    }

    int processed = 0;
    for (GList *l = q->q->head; l && processed < batch_size;)
    {
        GList *next = l->next;
        bgp_pub_item_t *item = (bgp_pub_item_t *)l->data;
        if (!item || item->nlri.afi != inst->afi || item->nlri.safi != inst->safi)
        {
            l = next;
            continue;
        }

        g_queue_delete_link(q->q, l);
        if (q->count > 0)
        {
            q->count--;
        }

        const bgp_route_node_t *best = bgp_rib_find_best(inst->rib, &item->nlri);
        if (best && bgp_best_can_publish_to_session(sess, best))
        {
            bgp_attr_t send_attr;
            if (!bgp_prepare_update_attr(sess, best, &send_attr))
            {
                bgp_pub_item_free(item, NULL);
                processed++;
                l = next;
                continue;
            }
            bgp_nexthop_t send_nexthop;
            bgp_prepare_update_nexthop(sess, best, &send_nexthop);
            bgp_pkt_send_update(sess->pri_conn, &item->nlri, &send_attr, &send_nexthop);
        }

        bgp_pub_item_free(item, NULL);
        processed++;
        l = next;
    }

    if (processed > 0)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_DEBUG("BGP: pub_queue neighbor=%s afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", addr_str, (unsigned)inst->afi,
                  (unsigned)inst->safi, processed, q->count);
    }
    return processed;
}

bgp_route_flush_queue_t *bgp_route_flush_queue_create(void)
{
    bgp_route_flush_queue_t *q = g_malloc0(sizeof(bgp_route_flush_queue_t));
    if (!q)
    {
        return NULL;
    }

    q->q = g_queue_new();
    if (!q->q)
    {
        g_free(q);
        return NULL;
    }

    return q;
}

void bgp_route_flush_queue_destroy(bgp_route_flush_queue_t *q, bgp_instance_t *inst)
{
    if (!q)
    {
        return;
    }

    bgp_rthead_t *head = NULL;
    while ((head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        if (inst && inst->rib)
        {
            bgp_rib_head_unref(head);
        }
    }

    if (q->q)
    {
        g_queue_free(q->q);
        q->q = NULL;
    }
    g_free(q);
}

int bgp_route_flush_queue_push(bgp_route_flush_queue_t *q, bgp_rthead_t *head)
{
    if (!q || !head)
    {
        return -1;
    }

    bgp_rib_head_ref(head);
    g_queue_push_tail(q->q, head);
    q->count++;
    if (head->inst)
    {
        bgp_work_schedule_route_flush(head->inst);
    }
    return 0;
}

int bgp_route_flush_queue_process(bgp_route_flush_queue_t *q, bgp_instance_t *inst, int batch_size)
{
    if (!q || !inst || batch_size <= 0)
    {
        return 0;
    }

    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx)
    {
        return 0;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : ROUTE_VRF_DEFAULT;
    int processed = 0;
    bgp_rthead_t *head = NULL;

    while (processed < batch_size && (head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        q->count--;
        const bgp_route_node_t *best = bgp_rib_find_best(inst->rib, &head->nlri);
        /* import-route 仅用于 BGP 内部参考，不下刷到 ROUTE。 */
        const bgp_route_node_t *flush_best = (best && !BIT_TEST(best->flags, BGP_ROUTE_FLAG_IMPORT)) ? best : NULL;

        if (head)
        {
            char nlri_str[BGP_NLRI_KEY_MAX];
            bgp_nlri_to_str(&head->nlri, nlri_str, sizeof(nlri_str));

            /* 先撤销已下刷但不再是 best+valid 的路径 */
            for (GList *l = head->route_list; l; l = l->next)
            {
                bgp_route_node_t *route = (bgp_route_node_t *)l->data;
                if (!route)
                {
                    continue;
                }

                if (!BIT_TEST(route->flags, BGP_ROUTE_FLAG_FLUSHED) || route == flush_best)
                {
                    continue;
                }

                route_msg_entry_t withdraw_entry;
                if (!route_node_to_route_entry(vrf_id, &head->nlri, route, &withdraw_entry))
                {
                    continue;
                }

                if (route_rpc_del(ctx, &withdraw_entry) == ERRCODE_SUCCESS)
                {
                    BIT_CLR(route->flags, BGP_ROUTE_FLAG_FLUSHED);
                }
                else
                {
                    LOG_WARN("BGP: route flush withdraw failed nlri=%s", nlri_str);
                }
            }

            /* 仅下刷当前 best+valid 路由 */
            if (flush_best)
            {
                bgp_route_node_t *best_mut = (bgp_route_node_t *)flush_best;
                if (!BIT_TEST(best_mut->flags, BGP_ROUTE_FLAG_FLUSHED))
                {
                    route_msg_entry_t add_entry;
                    if (route_node_to_route_entry(vrf_id, &head->nlri, flush_best, &add_entry) &&
                        route_rpc_add(ctx, &add_entry) == ERRCODE_SUCCESS)
                    {
                        BIT_SET(best_mut->flags, BGP_ROUTE_FLAG_FLUSHED);
                    }
                    else
                    {
                        LOG_WARN("BGP: route flush add failed nlri=%s", nlri_str);
                    }
                }
            }

            /* 删除时机统一放在 unref 之后，避免 cleanup 提前回收。 */
        }

        bgp_rib_head_unref(head);
        (void)bgp_rib_gc_head(inst->rib, head);
        processed++;
    }

    if (processed > 0)
    {
        LOG_DEBUG("BGP: route_flush_queue afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", (unsigned)inst->afi,
                  (unsigned)inst->safi, processed, q->count);
    }

    return processed;
}

// ============================================================================
// WITHDRAW 同步发送
// ============================================================================

void bgp_work_send_withdraw_to_all(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !nlri || !inst->peer_hash)
    {
        return;
    }
    withdraw_send_ctx_t ctx = {.inst = inst, .nlri = nlri};
    g_hash_table_foreach(inst->peer_hash, foreach_withdraw_send, &ctx);
}

typedef struct
{
    bgp_session_t *sess;
    uint32_t queued;
} enqueue_best_ctx_t;

static void enqueue_best_for_session_cb(const bgp_rthead_t *head, const bgp_route_node_t *route, gpointer user_data)
{
    (void)route;
    enqueue_best_ctx_t *ctx = (enqueue_best_ctx_t *)user_data;
    if (!ctx || !ctx->sess || !ctx->sess->pub_queue)
    {
        return;
    }

    if (bgp_pub_queue_push(ctx->sess->pub_queue, &head->nlri) == 0)
    {
        ctx->queued++;
    }
}

void bgp_work_enqueue_announce_to_established(bgp_instance_t *inst, const bgp_nlri_entry_t *nlri)
{
    if (!inst || !nlri || !inst->peer_hash)
    {
        return;
    }

    enqueue_established_ctx_t ctx = {.inst = inst, .nlri = nlri, .queued = 0};
    g_hash_table_foreach(inst->peer_hash, foreach_enqueue_established, &ctx);

    if (ctx.queued > 0)
    {
        bgp_work_schedule_session_pub(inst);
        char nlri_str[BGP_NLRI_KEY_MAX];
        bgp_nlri_to_str(nlri, nlri_str, sizeof(nlri_str));
        LOG_DEBUG("BGP: enqueue announce nlri=%s afi=%u safi=%u queued=%u", nlri_str, (unsigned)inst->afi,
                  (unsigned)inst->safi, ctx.queued);
    }
}

void bgp_work_enqueue_best_for_session(bgp_session_t *sess)
{
    if (!sess || !sess->pub_queue)
    {
        return;
    }

    enqueue_best_ctx_t ctx = {.sess = sess, .queued = 0};
    for (GList *l = sess->peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        if (!peer || !peer->inst || !peer->inst->rib)
        {
            continue;
        }
        bgp_rib_foreach_best(peer->inst->rib, enqueue_best_for_session_cb, &ctx);
    }

    if (ctx.queued > 0)
    {
        for (GList *l = sess->peer_list; l; l = l->next)
        {
            bgp_peer_t *peer = (bgp_peer_t *)l->data;
            if (!peer || !peer->inst)
            {
                continue;
            }
            if (bgp_pub_queue_count_for_instance(sess->pub_queue, peer->inst) > 0u)
            {
                bgp_work_schedule_session_pub(peer->inst);
            }
        }

        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: neighbor=%s queued %u best route(s) after Established "
                 "(pri fd=%d state=%s, sec fd=%d state=%s)",
                 addr_str, ctx.queued, sess->pri_conn ? sess->pri_conn->fd : -1,
                 bgp_work_conn_state_name(sess, sess->pri_conn), sess->sec_conn ? sess->sec_conn->fd : -1,
                 bgp_work_conn_state_name(sess, sess->sec_conn));
    }
}

static bgp_instance_t *bgp_work_lookup_instance(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        return NULL;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_work_local->protocol, vrf_id);
    if (!vrf || !vrf->inst_hash)
    {
        return NULL;
    }

    return (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));
}

static int bgp_work_process_calc_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst || !inst->calc_queue)
    {
        return 0;
    }

    int processed = bgp_calc_queue_process(inst->calc_queue, inst, BGP_WORK_BATCH_SIZE);
    if (allow_reschedule && processed > 0 && inst->calc_queue->count > 0u)
    {
        bgp_work_schedule_calc(inst);
    }
    return processed;
}

static int bgp_work_process_route_flush_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst || !inst->route_flush_queue)
    {
        return 0;
    }

    int processed = bgp_route_flush_queue_process(inst->route_flush_queue, inst, BGP_WORK_BATCH_SIZE);
    if (allow_reschedule && processed > 0 && inst->route_flush_queue->count > 0u)
    {
        bgp_work_schedule_route_flush(inst);
    }
    return processed;
}

static int bgp_work_process_session_pub_event(bgp_instance_t *inst, gboolean allow_reschedule)
{
    if (!inst || !inst->peer_hash)
    {
        return 0;
    }

    int total_processed = 0;
    gboolean made_progress = FALSE;
    gboolean need_more = FALSE;
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer val = NULL;

    g_hash_table_iter_init(&iter, inst->peer_hash);
    while (g_hash_table_iter_next(&iter, &key, &val))
    {
        (void)val;
        bgp_session_t *sess = bgp_vrf_find_session(inst->vrf, (const net_addr_t *)key);
        if (!sess || !sess->pub_queue)
        {
            continue;
        }

        int processed = bgp_pub_queue_process(sess->pub_queue, sess, inst, BGP_WORK_BATCH_SIZE);
        if (processed > 0)
        {
            made_progress = TRUE;
            total_processed += processed;
            if (bgp_pub_queue_count_for_instance(sess->pub_queue, inst) > 0u)
            {
                need_more = TRUE;
            }
        }
    }

    if (allow_reschedule && made_progress && need_more)
    {
        bgp_work_schedule_session_pub(inst);
    }

    return total_processed;
}

static void bgp_work_schedule_calc(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
    if (bgp_worker_post_calc_event(vrf_id, inst->afi, inst->safi) == 0)
    {
        return;
    }

    if (bgp_work_on_worker_thread())
    {
        (void)bgp_work_process_calc_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue calc work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

static void bgp_work_schedule_route_flush(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
    if (bgp_worker_post_route_flush_event(vrf_id, inst->afi, inst->safi) == 0)
    {
        return;
    }

    if (bgp_work_on_worker_thread())
    {
        (void)bgp_work_process_route_flush_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue route-flush work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

static void bgp_work_schedule_session_pub(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    uint32_t vrf_id = (inst->vrf) ? inst->vrf->vrf_id : BGP_VRF_PUBLIC_ID;
    if (bgp_worker_post_session_pub_event(vrf_id, inst->afi, inst->safi) == 0)
    {
        return;
    }

    if (bgp_work_on_worker_thread())
    {
        (void)bgp_work_process_session_pub_event(inst, FALSE);
        return;
    }

    LOG_WARN("BGP: failed to enqueue session-pub work event vrf=%u afi=%u safi=%u", vrf_id, (unsigned)inst->afi,
             (unsigned)inst->safi);
}

void bgp_work_handle_calc_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_work_lookup_instance(vrf_id, afi, safi);
    (void)bgp_work_process_calc_event(inst, TRUE);
}

void bgp_work_handle_route_flush_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_work_lookup_instance(vrf_id, afi, safi);
    (void)bgp_work_process_route_flush_event(inst, TRUE);
}

void bgp_work_handle_session_pub_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_instance_t *inst = bgp_work_lookup_instance(vrf_id, afi, safi);
    (void)bgp_work_process_session_pub_event(inst, TRUE);
}

void bgp_work_process_pending(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    for (;;)
    {
        int processed = 0;
        processed += bgp_work_process_calc_event(inst, FALSE);
        processed += bgp_work_process_route_flush_event(inst, FALSE);
        processed += bgp_work_process_session_pub_event(inst, FALSE);
        if (processed <= 0)
        {
            break;
        }
    }
}
