/**
 * @file   bgp_worker.c
 * @brief  BGP worker 线程：epoll 事件循环、连接管理与命令队列
 */
#include "bgp_worker.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "bgp_adj_rib_in.h"
#include "bgp_attr_intern.h"
#include "bgp_bmp_thread.h"
#include "bgp_bulk.h"
#include "bgp_calc.h"
#include "bgp_cmd.h"
#include "bgp_conn.h"
#include "bgp_fsm.h"
#include "bgp_nexthop.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_relay.h"
#include "bgp_route_flush.h"
#include "bgp_session.h"
#include "bgp_update_group.h"
#include "bgp_vrf.h"
#include "bgp_vrf_export.h"
#include "bgp_vrf_import.h"
#include "errcode.h"
#include "if.h"
#include "log.h"
#include "net_addr.h"

/** BGP server epoll 单次最大事件数 */
#define BGP_MAX_EPOLL_EVENTS 16

/** epoll data.ptr sentinel：区分工作事件 */
static char bgp_work_tag;

bgp_work_local_t *g_bgp_work_local = NULL;

static void bgp_worker_runtime_cleanup(void);

typedef enum bgp_worker_event_type
{
    BGP_WORKER_EVENT_CALC = 1,
    BGP_WORKER_EVENT_ROUTE_FLUSH = 2,
    BGP_WORKER_EVENT_SESSION_PUB = 3,
    BGP_WORKER_EVENT_VRF_EXPORT = 4,
    BGP_WORKER_EVENT_BULK = 5,
} bgp_worker_event_type_t;

typedef struct bgp_worker_event
{
    bgp_worker_event_type_t type;
    uint32_t vrf_id;
    bgp_afi_t afi;
    bgp_safi_t safi;
    void *payload;
} bgp_worker_event_t;

// ============================================================================
// 共享 helper（供 bgp_calc / bgp_route_flush / bgp_update_group 使用）
// ============================================================================

gboolean bgp_worker_is_current_thread(void)
{
    return g_bgp_work_local && g_bgp_work_local->worker_thread != 0 &&
           pthread_equal(pthread_self(), g_bgp_work_local->worker_thread);
}

bgp_instance_t *bgp_worker_lookup_instance(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
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

// ============================================================================
// 工作事件队列（worker 线程自投递）
// ============================================================================

static void bgp_worker_signal_work_event(void)
{
    if (!g_bgp_work_local || g_bgp_work_local->work_eventfd < 0)
    {
        return;
    }

    uint64_t one = 1;
    if (write(g_bgp_work_local->work_eventfd, &one, sizeof(one)) != (ssize_t)sizeof(one))
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("BGP: work eventfd write failed");
        }
    }
}

static bgp_worker_event_t *bgp_worker_event_create(bgp_worker_event_type_t type, uint32_t vrf_id, bgp_afi_t afi,
                                                   bgp_safi_t safi)
{
    bgp_worker_event_t *evt = g_malloc(sizeof(*evt));
    if (!evt)
    {
        return NULL;
    }

    evt->type = type;
    evt->vrf_id = vrf_id;
    evt->afi = afi;
    evt->safi = safi;
    evt->payload = NULL;
    return evt;
}

static void bgp_worker_event_destroy(bgp_worker_event_t *evt)
{
    if (!evt)
    {
        return;
    }
    if (evt->type == BGP_WORKER_EVENT_BULK && evt->payload)
    {
        bgp_bulk_task_destroy((bgp_bulk_task_t *)evt->payload);
        evt->payload = NULL;
    }
    g_free(evt);
}

static int bgp_worker_event_enqueue(bgp_worker_event_t *evt)
{
    if (!evt || !g_bgp_work_local || !g_bgp_work_local->work_queue || g_bgp_work_local->work_eventfd < 0)
    {
        return -1;
    }

    g_async_queue_push(g_bgp_work_local->work_queue, evt);
    bgp_worker_signal_work_event();
    return 0;
}

static void bgp_worker_work_drain_queue(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->work_queue)
    {
        return;
    }

    bgp_worker_event_t *evt = NULL;
    while ((evt = (bgp_worker_event_t *)g_async_queue_try_pop(g_bgp_work_local->work_queue)) != NULL)
    {
        bgp_worker_event_destroy(evt);
    }
}

void bgp_worker_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd,
                                   bgp_peer_update_ingest_stats_t *stats)
{
    if (stats)
    {
        memset(stats, 0, sizeof(*stats));
    }
    if (!session || !upd)
    {
        return;
    }

    uint32_t local_as = 0u;
    if (g_bgp_work_local && g_bgp_work_local->protocol)
    {
        local_as = g_bgp_work_local->protocol->as_number;
    }

    /* 收到带本地 AS 的 reach 路由时丢弃（AS loop），但保留同报文内 withdraw 处理。 */
    if (upd->reach_len > 0 && bgp_attr_is_as_loop(&upd->attr, local_as))
    {
        char peer[64];
        net_addr_to_str(&session->neighbor_addr, peer, sizeof(peer));
        LOG_WARN("BGP: drop UPDATE reach from %s: AS_PATH contains local AS %u (path=%s)", peer, local_as,
                 (upd->attr.as_path[0] != '\0') ? upd->attr.as_path : "-");

        bgp_update_result_t filtered = *upd;
        filtered.reach = NULL;
        filtered.reach_len = 0;

        bgp_peer_update_ingest_stats_t relay_stats = {0};
        bgp_adj_rib_in_ingest_peer_update(session, &filtered, &relay_stats);
        relay_stats.reach_failed += upd->reach_len;

        if (stats)
        {
            *stats = relay_stats;
        }
        return;
    }

    /* RFC 4456 §7：iBGP 收到的 reach 路由若 ORIGINATOR_ID == 本端 router-id，
     * 或 CLUSTER_LIST 已含本端 cluster-id，视为反射环路。
     * 处理方式：把这批 reach NLRI 拼到 unreach 数组里走标准撤销链——
     * 若 RIB 中已存在历史副本（如本端 cluster-id 刚变更前接收的）会被正常撤销，
     * 新条目则相当于 no-op（unreach 未命中），无需额外回扫机制。 */
    if (upd->reach_len > 0 && session->sess_type == BGP_SESS_TYPE_IBGP && session->vrf)
    {
        bool drop = false;
        const bgp_vrf_t *vrf = session->vrf;
        uint32_t local_rid = vrf->router_id;
        /* cluster-id 是 per-AF 的，按 UPDATE 的 (afi, safi) 找对应 instance */
        bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(
            vrf->inst_hash, bgp_inst_hash_key((bgp_afi_t)upd->afi, (bgp_safi_t)upd->safi));
        uint32_t local_cid = bgp_inst_effective_cluster_id(inst);

        if (upd->attr.has_originator_id && upd->attr.originator_id.family == AF_INET && local_rid != 0)
        {
            uint32_t orig = ntohl(upd->attr.originator_id.u.v4.s_addr);
            if (orig == local_rid)
            {
                drop = true;
            }
        }
        if (!drop && local_cid != 0)
        {
            for (uint8_t i = 0; i < upd->attr.cluster_list_len; i++)
            {
                if (upd->attr.cluster_list[i].family != AF_INET)
                {
                    continue;
                }
                uint32_t cid = ntohl(upd->attr.cluster_list[i].u.v4.s_addr);
                if (cid == local_cid)
                {
                    drop = true;
                    break;
                }
            }
        }
        if (drop)
        {
            char peer[64];
            net_addr_to_str(&session->neighbor_addr, peer, sizeof(peer));
            LOG_WARN("BGP: UPDATE from %s reach RR-loop, treating as withdraw (count=%u)", peer, upd->reach_len);

            uint32_t total = upd->unreach_len + upd->reach_len;
            bgp_nlri_entry_t *merged = g_malloc_n(total, sizeof(*merged));
            if (upd->unreach_len > 0)
            {
                memcpy(merged, upd->unreach, upd->unreach_len * sizeof(*merged));
            }
            if (upd->reach_len > 0)
            {
                memcpy(merged + upd->unreach_len, upd->reach, upd->reach_len * sizeof(*merged));
            }

            bgp_update_result_t filtered = *upd;
            filtered.reach = NULL;
            filtered.reach_len = 0;
            filtered.unreach = merged;
            filtered.unreach_len = total;

            bgp_peer_update_ingest_stats_t relay_stats = {0};
            bgp_adj_rib_in_ingest_peer_update(session, &filtered, &relay_stats);
            relay_stats.reach_failed += upd->reach_len;
            g_free(merged);
            if (stats)
            {
                *stats = relay_stats;
            }
            return;
        }
    }

    bgp_adj_rib_in_ingest_peer_update(session, upd, stats);
}

void bgp_worker_flush_peer_routes(uint32_t vrf_id, const net_addr_t *source)
{
    bgp_relay_flush_peer_routes(vrf_id, source);
}

int bgp_worker_post_calc_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_worker_event_t *evt = bgp_worker_event_create(BGP_WORKER_EVENT_CALC, vrf_id, afi, safi);
    if (!evt)
    {
        return -1;
    }
    if (bgp_worker_event_enqueue(evt) != 0)
    {
        bgp_worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

int bgp_worker_post_route_flush_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_worker_event_t *evt = bgp_worker_event_create(BGP_WORKER_EVENT_ROUTE_FLUSH, vrf_id, afi, safi);
    if (!evt)
    {
        return -1;
    }
    if (bgp_worker_event_enqueue(evt) != 0)
    {
        bgp_worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

int bgp_worker_post_session_pub_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi)
{
    bgp_worker_event_t *evt = bgp_worker_event_create(BGP_WORKER_EVENT_SESSION_PUB, vrf_id, afi, safi);
    if (!evt)
    {
        return -1;
    }
    if (bgp_worker_event_enqueue(evt) != 0)
    {
        bgp_worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

int bgp_worker_post_vrf_export_event(void)
{
    bgp_worker_event_t *evt =
        bgp_worker_event_create(BGP_WORKER_EVENT_VRF_EXPORT, BGP_VRF_PUBLIC_ID, BGP_AFI_IPV4, BGP_SAFI_VPN_UNICAST);
    if (!evt)
    {
        return -1;
    }
    if (bgp_worker_event_enqueue(evt) != 0)
    {
        bgp_worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

int bgp_worker_post_bulk_task(bgp_bulk_task_t *task)
{
    if (!task)
    {
        return -1;
    }
    bgp_worker_event_t *evt =
        bgp_worker_event_create(BGP_WORKER_EVENT_BULK, BGP_VRF_PUBLIC_ID, BGP_AFI_IPV4, BGP_SAFI_UNICAST);
    if (!evt)
    {
        return -1;
    }
    evt->payload = task;
    if (bgp_worker_event_enqueue(evt) != 0)
    {
        evt->payload = NULL;
        bgp_worker_event_destroy(evt);
        return -1;
    }
    return 0;
}

void bgp_worker_drain_work_events(void)
{
    if (!g_bgp_work_local || g_bgp_work_local->work_eventfd < 0 || !g_bgp_work_local->work_queue)
    {
        return;
    }

    uint64_t v;
    while (read(g_bgp_work_local->work_eventfd, &v, sizeof(v)) > 0)
    {
        /* drain eventfd counter */
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
    {
        LOG_PERROR("BGP: work eventfd read failed");
    }

    int processed = 0;
    bgp_worker_event_t *evt = NULL;
    while (processed < BGP_WORK_BATCH_SIZE &&
           (evt = (bgp_worker_event_t *)g_async_queue_try_pop(g_bgp_work_local->work_queue)) != NULL)
    {
        switch (evt->type)
        {
            case BGP_WORKER_EVENT_CALC:
                bgp_calc_handle_event(evt->vrf_id, evt->afi, evt->safi);
                break;
            case BGP_WORKER_EVENT_ROUTE_FLUSH:
                bgp_route_flush_handle_event(evt->vrf_id, evt->afi, evt->safi);
                break;
            case BGP_WORKER_EVENT_SESSION_PUB:
                bgp_update_group_handle_pub_event(evt->vrf_id, evt->afi, evt->safi);
                break;
            case BGP_WORKER_EVENT_VRF_EXPORT:
            {
                bgp_instance_t *vpn_inst = bgp_vrf_export_target_inst();
                if (vpn_inst)
                {
                    (void)bgp_vrf_export_queue_process(vpn_inst, BGP_VRF_EXPORT_BATCH);
                    /* pending 未抽干则再投事件自重排，让出 epoll 处理报文/定时器 */
                    if (vpn_inst->vrf_export_state &&
                        ((bgp_vrf_export_state_t *)vpn_inst->vrf_export_state)->pending_count > 0)
                    {
                        (void)bgp_worker_post_vrf_export_event();
                    }
                }
                break;
            }
            case BGP_WORKER_EVENT_BULK:
            {
                bgp_bulk_task_t *task = (bgp_bulk_task_t *)evt->payload;
                evt->payload = NULL;
                if (bgp_bulk_task_handle(task))
                {
                    bgp_bulk_task_destroy(task);
                }
                break;
            }
            default:
                LOG_WARN("BGP: unknown work event type=%d", (int)evt->type);
                break;
        }

        bgp_worker_event_destroy(evt);
        processed++;
    }

    if (g_async_queue_length(g_bgp_work_local->work_queue) > 0)
    {
        bgp_worker_signal_work_event();
    }
}

// ============================================================================
// BGP server 线程 — 事件处理函数
// ============================================================================

static void bgp_handle_ka_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->ka_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read ka timerfd");
    }
    bgp_fsm_event(sess, BGP_EVT_KEEPALIVE_TIMER_EXPIRED);
}

static void bgp_handle_hold_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->hold_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read hold timerfd");
    }
    bgp_fsm_event(sess, BGP_EVT_HOLD_TIMER_EXPIRED);
}

static void bgp_handle_retry_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->retry_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read timerfd");
    }

    /* 守卫：若邻居 AF 已删除，取消定时器并退出 */
    if (!sess->vrf || !bgp_vrf_neighbor_has_any_af(sess->vrf, &sess->neighbor_addr))
    {
        bgp_session_cancel_retry(sess, g_bgp_work_local->epoll_fd);
        return;
    }

    bgp_fsm_event(sess, BGP_EVT_CONNECT_RETRY_EXPIRED);
}

// ============================================================================
// BGP server 线程
// ============================================================================

static void *bgp_worker_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "bgp-worker");
    log_set_tag("bgp");

    struct epoll_event events[BGP_MAX_EPOLL_EVENTS];

    while (g_bgp_work_local->running)
    {
        int nfds = epoll_wait(g_bgp_work_local->epoll_fd, events, BGP_MAX_EPOLL_EVENTS, 1000);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("BGP: epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            uintptr_t raw = (uintptr_t)events[i].data.ptr;

            if (bgp_listen_handle_event_ptr(events[i].data.ptr))
            {
                continue;
            }
            if (events[i].data.ptr == (void *)&bgp_cmd_tag)
            {
                if (bgp_cmd_process_event())
                {
                    break;
                }
                continue;
            }
            if (events[i].data.ptr == (void *)&bgp_work_tag)
            {
                bgp_worker_drain_work_events();
                continue;
            }

            if (raw & 1UL)
            {
                bgp_timer_sentinel_t *sentinel = (bgp_timer_sentinel_t *)(raw & ~1UL);
                switch (sentinel->type)
                {
                    case BGP_TIMER_TYPE_RETRY:
                        bgp_handle_retry_timer(sentinel->session);
                        break;
                    case BGP_TIMER_TYPE_KEEPALIVE:
                        bgp_handle_ka_timer(sentinel->session);
                        break;
                    case BGP_TIMER_TYPE_HOLD:
                        bgp_handle_hold_timer(sentinel->session);
                        break;
                    default:
                        break;
                }
                continue;
            }

            bgp_conn_t *conn = (bgp_conn_t *)events[i].data.ptr;
            if (!conn || conn->fd < 0)
            {
                continue;
            }

            if (conn->is_connecting)
            {
                bgp_conn_handle_active_connect(conn);
            }
            else
            {
                bgp_pkt_on_data(conn);
            }
        }

        /* 释放本轮 epoll 事件处理期间关闭的连接 */
        bgp_conn_flush_deferred();
    }

    bgp_conn_flush_deferred();
    bgp_worker_runtime_cleanup();
    return NULL;
}

static void bgp_worker_runtime_cleanup(void)
{
    /* 先把所有已下刷到 ROUTE 的路由主动撤销:此刻 IPC ctx 仍可用,worker 仍存活,
     * 不依赖 route_flush_queue/calc_queue(关闭路径上它们只销毁不处理)。 */
    (void)bgp_route_flush_withdraw_all_for_shutdown();

    if (g_bgp_work_local->protocol && g_bgp_work_local->protocol->vrf_hash)
    {
        GHashTableIter vrf_iter;
        gpointer vrf_key, vrf_val;
        g_hash_table_iter_init(&vrf_iter, g_bgp_work_local->protocol->vrf_hash);
        while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
        {
            bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
            GHashTableIter sess_iter;
            gpointer sess_key, sess_val;
            g_hash_table_iter_init(&sess_iter, vrf->sess_hash);
            while (g_hash_table_iter_next(&sess_iter, &sess_key, &sess_val))
            {
                bgp_session_t *sess = (bgp_session_t *)sess_val;
                bgp_session_stop_all(sess);
            }
        }
    }

    /* bgp_session_stop_all → bgp_conn_close 会追加到 deferred_conns，
     * 需要在此处 flush，否则这些连接对象会泄漏。 */
    bgp_conn_flush_deferred();

    bgp_listen_stop();
    bgp_cmd_drain_queue();
    bgp_work_show_cleanup();

    if (g_bgp_work_local->epoll_fd >= 0)
    {
        close(g_bgp_work_local->epoll_fd);
        g_bgp_work_local->epoll_fd = DEV_INVALID_FD;
    }

    if (g_bgp_work_local->protocol)
    {
        bgp_protocol_destroy(g_bgp_work_local->protocol);
        g_bgp_work_local->protocol = NULL;
    }
    if_api_cache_cleanup();
}

static int bgp_worker_channel_init(void)
{
    if (!g_bgp_work_local->cmd_queue)
    {
        g_bgp_work_local->cmd_queue = g_async_queue_new();
        if (!g_bgp_work_local->cmd_queue)
        {
            LOG_ERROR("BGP: Failed to create command queue");
            return ERRCODE_FAIL;
        }
    }
    if (!g_bgp_work_local->work_queue)
    {
        g_bgp_work_local->work_queue = g_async_queue_new();
        if (!g_bgp_work_local->work_queue)
        {
            LOG_ERROR("BGP: Failed to create work event queue");
            return ERRCODE_FAIL;
        }
    }

    if (g_bgp_work_local->cmd_eventfd < 0)
    {
        g_bgp_work_local->cmd_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_bgp_work_local->cmd_eventfd < 0)
        {
            LOG_PERROR("BGP: Failed to create cmd eventfd");
            return ERRCODE_FAIL;
        }
    }
    if (g_bgp_work_local->work_eventfd < 0)
    {
        g_bgp_work_local->work_eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (g_bgp_work_local->work_eventfd < 0)
        {
            LOG_PERROR("BGP: Failed to create work eventfd");
            return ERRCODE_FAIL;
        }
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &bgp_cmd_tag;
    if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, g_bgp_work_local->cmd_eventfd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD cmd eventfd failed");
        return ERRCODE_FAIL;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &bgp_work_tag;
    if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, g_bgp_work_local->work_eventfd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD work eventfd failed");
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

static void bgp_worker_channel_cleanup(void)
{
    if (g_bgp_work_local->epoll_fd >= 0 && g_bgp_work_local->cmd_eventfd >= 0)
    {
        epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_work_local->cmd_eventfd, NULL);
    }
    if (g_bgp_work_local->epoll_fd >= 0 && g_bgp_work_local->work_eventfd >= 0)
    {
        epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_work_local->work_eventfd, NULL);
    }

    if (g_bgp_work_local->cmd_eventfd >= 0)
    {
        close(g_bgp_work_local->cmd_eventfd);
        g_bgp_work_local->cmd_eventfd = -1;
    }
    if (g_bgp_work_local->work_eventfd >= 0)
    {
        close(g_bgp_work_local->work_eventfd);
        g_bgp_work_local->work_eventfd = -1;
    }

    bgp_cmd_drain_queue();
    bgp_worker_work_drain_queue();

    if (g_bgp_work_local->cmd_queue)
    {
        g_async_queue_unref(g_bgp_work_local->cmd_queue);
        g_bgp_work_local->cmd_queue = NULL;
    }
    if (g_bgp_work_local->work_queue)
    {
        g_async_queue_unref(g_bgp_work_local->work_queue);
        g_bgp_work_local->work_queue = NULL;
    }
}

int bgp_worker_prepare(void)
{
    if (!g_bgp_work_local)
    {
        g_bgp_work_local = g_malloc0(sizeof(*g_bgp_work_local));
        if (!g_bgp_work_local)
        {
            LOG_ERROR("BGP: Failed to allocate worker local context");
            return ERRCODE_FAIL;
        }
        g_bgp_work_local->epoll_fd = DEV_INVALID_FD;
        g_bgp_work_local->cmd_eventfd = -1;
        g_bgp_work_local->work_eventfd = -1;
    }

    if_api_cache_init();

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("BGP: Failed to create epoll");
        return ERRCODE_FAIL;
    }

    g_bgp_work_local->epoll_fd = epoll_fd;

    if (bgp_worker_channel_init() != ERRCODE_SUCCESS)
    {
        close(epoll_fd);
        g_bgp_work_local->epoll_fd = DEV_INVALID_FD;
        bgp_worker_channel_cleanup();
        return ERRCODE_FAIL;
    }

    bgp_relay_init();
    bgp_vrf_import_init();

    return ERRCODE_SUCCESS;
}

int bgp_worker_launch(void)
{
    g_bgp_work_local->running = 1;
    if (pthread_create(&g_bgp_work_local->worker_thread, NULL, bgp_worker_thread, NULL) != 0)
    {
        LOG_PERROR("BGP: Failed to create server thread");
        g_bgp_work_local->running = 0;
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

void bgp_worker_handle_bmp_initial_peers(const char *inst_name)
{
    if (!inst_name || !g_bgp_work_local || !g_bgp_work_local->protocol)
    {
        return;
    }

    GHashTable *vrf_hash = g_bgp_work_local->protocol->vrf_hash;
    if (!vrf_hash)
    {
        return;
    }

    GHashTableIter vrf_iter;
    gpointer vrf_key, vrf_val;
    g_hash_table_iter_init(&vrf_iter, vrf_hash);

    while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
    {
        bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
        if (!vrf || !vrf->sess_hash)
        {
            continue;
        }

        GHashTableIter sess_iter;
        gpointer sess_key, sess_val;
        g_hash_table_iter_init(&sess_iter, vrf->sess_hash);

        while (g_hash_table_iter_next(&sess_iter, &sess_key, &sess_val))
        {
            bgp_session_t *sess = (bgp_session_t *)sess_val;
            if (!sess || sess->fsm_state != BGP_FSM_STATE_ESTABLISHED)
            {
                continue;
            }

            bgp_bmp_peer_info_t info;
            bgp_bmp_fill_peer_info(sess, &info);
            bgp_bmp_post_initial_peer_up(&info, inst_name);
        }
    }
}

void bgp_worker_shutdown(void)
{
    static pthread_mutex_t s_shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&s_shutdown_mutex);

    if (!g_bgp_work_local)
    {
        pthread_mutex_unlock(&s_shutdown_mutex);
        return;
    }

    if (g_bgp_work_local->worker_thread)
    {
        bgp_cmd_post_shutdown();
        pthread_join(g_bgp_work_local->worker_thread, NULL);
        g_bgp_work_local->worker_thread = 0;
    }
    else
    {
        bgp_worker_runtime_cleanup();
    }

    g_bgp_work_local->running = 0;
    bgp_worker_channel_cleanup();

    g_free(g_bgp_work_local);
    g_bgp_work_local = NULL;

    pthread_mutex_unlock(&s_shutdown_mutex);
}
