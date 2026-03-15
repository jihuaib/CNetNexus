/**
 * @file   bgp_work.c
 * @brief  BGP 路由处理工作队列实现
 * @author jhb
 * @date   2026/03/15
 */
#include "bgp_work.h"

#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "bgp_calc.h"
#include "bgp_instance.h"
#include "bgp_pkt.h"
#include "bgp_vrf.h"
#include "log.h"

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
    const bgp_bestpath_entry_t *best; /**< 从 bestlist 查到的最优路径条目 */
} announce_send_ctx_t;

/** g_hash_table_foreach 回调：向各 ESTABLISHED 对端发送 UPDATE（ANNOUNCE） */
static void foreach_announce_send(gpointer key, gpointer value, gpointer user_data)
{
    (void)value;
    const net_addr_t *addr = key;
    announce_send_ctx_t *ctx = user_data;

    bgp_session_t *sess = bgp_vrf_find_session(ctx->inst->vrf, addr);
    if (!sess || sess->state != BGP_CONN_STATE_ESTABLISHED || !sess->pri_conn || sess->pri_conn->fd < 0)
    {
        return;
    }

    bgp_pkt_send_update(sess->pri_conn, &ctx->best->nlri, &ctx->best->attr, &ctx->best->nexthop);
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
    if (!sess || sess->state != BGP_CONN_STATE_ESTABLISHED || !sess->pri_conn || sess->pri_conn->fd < 0)
    {
        return;
    }

    bgp_pkt_send_withdraw(sess->pri_conn, ctx->nlri);
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

void bgp_calc_queue_destroy(bgp_calc_queue_t *q)
{
    if (!q)
    {
        return;
    }
    bgp_nlri_entry_t *nlri;
    while ((nlri = g_queue_pop_head(q->q)) != NULL)
    {
        g_free(nlri);
    }
    g_queue_free(q->q);
    g_free(q);
}

int bgp_calc_queue_push(bgp_calc_queue_t *q, const bgp_nlri_entry_t *nlri)
{
    if (!q || !nlri)
    {
        return -1;
    }
    bgp_nlri_entry_t *copy = g_malloc0(sizeof(*copy));
    *copy = *nlri;
    g_queue_push_tail(q->q, copy);
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
    bgp_nlri_entry_t *nlri;
    while (processed < batch_size && (nlri = g_queue_pop_head(q->q)) != NULL)
    {
        q->count--;
        bgp_calc_run_one(inst, nlri);
        g_free(nlri);
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
    bgp_nlri_entry_t *nlri;
    while ((nlri = g_queue_pop_head(q->q)) != NULL)
    {
        g_free(nlri);
    }
    g_queue_free(q->q);
    g_free(q);
}

int bgp_pub_queue_push(bgp_pub_queue_t *q, const bgp_nlri_entry_t *nlri)
{
    if (!q || !nlri)
    {
        return -1;
    }
    bgp_nlri_entry_t *copy = g_malloc0(sizeof(*copy));
    *copy = *nlri;
    g_queue_push_tail(q->q, copy);
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
    bgp_nlri_entry_t *nlri;
    while (processed < batch_size && (nlri = g_queue_pop_head(q->q)) != NULL)
    {
        q->count--;

        /* 通过 NLRI 在 bestlist 中查找最优路径信息 */
        const bgp_bestpath_entry_t *best = bgp_bestlist_find(inst->bestlist, nlri);
        if (best)
        {
            /* 找到最优路径，向所有 ESTABLISHED 邻居发送 UPDATE */
            announce_send_ctx_t ctx = {.inst = inst, .best = best};
            g_hash_table_foreach(inst->peer_hash, foreach_announce_send, &ctx);
        }
        /* 未找到则跳过（路由已被撤销，WITHDRAW 由 bgp_calc_run_one 同步发出） */

        g_free(nlri);
        processed++;
    }
    if (processed > 0)
    {
        LOG_DEBUG("BGP: pub_queue afi=%u safi=%u 批量处理 %d 条，剩余 %u 条", (unsigned)inst->afi, (unsigned)inst->safi,
                  processed, q->count);
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
        (void)read(inst->work_timerfd, &expirations, sizeof(expirations));
    }

    /* 批量处理优选队列：NLRI → best-path 计算 → 入 pub_queue 或同步 WITHDRAW */
    if (inst->calc_queue)
    {
        bgp_calc_queue_process(inst->calc_queue, inst, BGP_WORK_BATCH_SIZE);
    }

    /* 批量处理发布队列：通过 NLRI 查 bestlist → 发送 UPDATE 给所有 ESTABLISHED 邻居 */
    if (inst->pub_queue)
    {
        bgp_pub_queue_process(inst->pub_queue, inst, BGP_WORK_BATCH_SIZE);
    }
}
