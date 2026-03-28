/**
 * @file   bgp_work.c
 * @brief  BGP 路由处理工作队列实现
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_work.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "bgp_calc.h"
#include "bgp_instance.h"
#include "bgp_main.h"
#include "bgp_pkt.h"
#include "bgp_rib.h"
#include "bgp_vrf.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

/* ---- 全局 epoll fd（由 bgp_main 在初始化后通过 bgp_work_set_epoll_fd 设置） ---- */
static int g_work_epoll_fd = -1;

void bgp_work_set_epoll_fd(int epoll_fd)
{
    g_work_epoll_fd = epoll_fd;
}

// ============================================================================
// 发布队列内部回调
// ============================================================================

/** 遍历 peer_hash 时的 ANNOUNCE 发送上下文 */
typedef struct
{
    bgp_instance_t *inst;
    const bgp_nlri_entry_t *nlri; /**< 来自 rthead 的 NLRI（借用） */
    const bgp_route_node_t *best; /**< 当前最优路径（route_list 首元素，借用） */
} announce_send_ctx_t;

/** g_hash_table_foreach 回调：向各 ESTABLISHED 对端发送 UPDATE（ANNOUNCE） */
static void foreach_announce_send(gpointer key, gpointer value, gpointer user_data)
{
    (void)value;
    const net_addr_t *addr = key;
    announce_send_ctx_t *ctx = user_data;

    /* Split-horizon: 不向”该最优路径的来源邻居”回灌同一路由，避免双节点反射风暴。
     * import 路由（BGP_ROUTE_FLAG_IMPORT）无来源邻居，不触发 split-horizon。 */
    if (ctx->best && !BIT_TEST(ctx->best->flags, BGP_ROUTE_FLAG_IMPORT) && net_addr_equal(&ctx->best->source, addr))
    {
        return;
    }

    bgp_session_t *sess = bgp_vrf_find_session(ctx->inst->vrf, addr);
    if (!sess || !sess->pri_conn || sess->pri_conn->fd < 0 || sess->pri_conn->state != BGP_CONN_STATE_ESTABLISHED)
    {
        return;
    }

    bgp_pkt_send_update(sess->pri_conn, ctx->nlri, &ctx->best->attr, &ctx->best->nexthop);
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
    if (!sess || !sess->pri_conn || sess->pri_conn->fd < 0 || sess->pri_conn->state != BGP_CONN_STATE_ESTABLISHED)
    {
        return;
    }

    bgp_pkt_send_withdraw(sess->pri_conn, ctx->nlri);
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

    if (route->source.family != prefix->family || route->nexthop.global.family != prefix->family)
    {
        return 0;
    }

    int32_t metric = 0;
    if (route->attr.has_med)
    {
        metric = (route->attr.med > (uint32_t)INT32_MAX) ? INT32_MAX : (int32_t)route->attr.med;
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
    entry_out->prefix_addr = *prefix;
    entry_out->nexthop_addr = route->nexthop.global;
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

void bgp_pub_queue_destroy(bgp_pub_queue_t *q, bgp_instance_t *inst)
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

int bgp_pub_queue_push(bgp_pub_queue_t *q, bgp_rthead_t *head)
{
    if (!q || !head)
    {
        return -1;
    }

    bgp_rib_head_ref(head);
    g_queue_push_tail(q->q, head);
    q->count++;
    return 0;
}

int bgp_pub_queue_process(bgp_pub_queue_t *q, bgp_instance_t *inst, int batch_size)
{
    if (!q || !inst || batch_size <= 0 || !inst->peer_hash)
    {
        return 0;
    }
    int processed = 0;
    bgp_rthead_t *head = NULL;
    while (processed < batch_size && (head = (bgp_rthead_t *)g_queue_pop_head(q->q)) != NULL)
    {
        q->count--;

        const bgp_route_node_t *best = bgp_rib_find_best(inst->rib, &head->nlri);
        if (head && best)
        {
            /* 找到最优路径，向所有 ESTABLISHED 邻居发送 UPDATE */
            announce_send_ctx_t ctx = {.inst = inst, .nlri = &head->nlri, .best = best};
            g_hash_table_foreach(inst->peer_hash, foreach_announce_send, &ctx);
        }
        /* 未找到则跳过（路由已被撤销，WITHDRAW 由 bgp_calc_run_one 同步发出） */

        bgp_rib_head_unref(head);
        processed++;
    }
    if (processed > 0)
    {
        LOG_DEBUG("BGP: pub_queue afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", (unsigned)inst->afi, (unsigned)inst->safi,
                  processed, q->count);
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

// ============================================================================
// 工作定时器
// ============================================================================

int bgp_work_timer_start(bgp_instance_t *inst, uint32_t interval_ms)
{
    if (!inst)
    {
        return -1;
    }
    if (g_work_epoll_fd < 0)
    {
        LOG_WARN("BGP: work timer start skipped, epoll fd not ready (afi=%u safi=%u)", (unsigned)inst->afi,
                 (unsigned)inst->safi);
        return -1;
    }

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
    {
        LOG_PERROR("BGP: timerfd_create 失败");
        return -1;
    }

    struct itimerspec its;
    its.it_value.tv_sec = interval_ms / 1000;
    its.it_value.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    its.it_interval = its.it_value; /* 周期触发 */

    if (timerfd_settime(fd, 0, &its, NULL) < 0)
    {
        LOG_PERROR("BGP: timerfd_settime 失败");
        close(fd);
        return -1;
    }

    /* 初始化哨兵（_dummy=NULL，type=WORK，inst=inst） */
    inst->work_sentinel._dummy = NULL;
    inst->work_sentinel.type = BGP_TIMER_TYPE_WORK;
    inst->work_sentinel.inst = inst;

    /* 注册到 epoll：data.ptr = (&sentinel | 1)，bit0=1 标记定时器事件 */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)((uintptr_t)&inst->work_sentinel | 1UL);
    if (epoll_ctl(g_work_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD work timer 失败");
        close(fd);
        return -1;
    }

    inst->work_timerfd = fd;
    LOG_DEBUG("BGP: work timer 启动 afi=%u safi=%u interval=%ums fd=%d", (unsigned)inst->afi, (unsigned)inst->safi,
              interval_ms, fd);
    return 0;
}

void bgp_work_timer_stop(bgp_instance_t *inst)
{
    if (!inst || inst->work_timerfd < 0)
    {
        return;
    }
    if (g_work_epoll_fd >= 0)
    {
        epoll_ctl(g_work_epoll_fd, EPOLL_CTL_DEL, inst->work_timerfd, NULL);
    }
    close(inst->work_timerfd);
    inst->work_timerfd = -1;
    LOG_DEBUG("BGP: work timer 停止 afi=%u safi=%u", (unsigned)inst->afi, (unsigned)inst->safi);
}

void bgp_work_process(bgp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    /* 读取 timerfd 计数（必须，否则 epoll 持续触发） */
    if (inst->work_timerfd >= 0)
    {
        uint64_t expirations;
        ssize_t n = read(inst->work_timerfd, &expirations, sizeof(expirations));
        if (n < 0 && errno != EINTR && errno != EAGAIN)
        {
            LOG_PERROR("BGP: 读取 work timerfd 失败");
        }
    }

    /* 批量处理优选队列：NLRI → best-path 计算 → 入 pub_queue 或同步 WITHDRAW */
    if (inst->calc_queue)
    {
        bgp_calc_queue_process(inst->calc_queue, inst, BGP_WORK_BATCH_SIZE);
    }

    /* 批量处理 route 下刷队列：只下刷 best+valid 路由到 ROUTE 模块 */
    if (inst->route_flush_queue)
    {
        bgp_route_flush_queue_process(inst->route_flush_queue, inst, BGP_WORK_BATCH_SIZE);
    }

    /* 批量处理发布队列：通过 NLRI 查 RIB is_best 路径 → 发送 UPDATE 给所有 ESTABLISHED 邻居 */
    if (inst->pub_queue)
    {
        bgp_pub_queue_process(inst->pub_queue, inst, BGP_WORK_BATCH_SIZE);
    }
}
