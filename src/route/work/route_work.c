/**
 * @file   route_work.c
 * @brief  Route 工作队列实现：优选队列 + timerfd 批量处理
 * @author jhb
 * @date   2026/03/28
 */
#include "route_work.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "log.h"
#include "route_calc.h"
#include "route_main.h"
#include "route_relay.h"
#include "route_rib.h"
#include "route_worker.h"

/* ---- 全局 epoll fd（由 route_worker 通过 route_work_set_epoll_fd 设置） ---- */
static int g_work_epoll_fd = -1;

void route_work_set_epoll_fd(int epoll_fd)
{
    g_work_epoll_fd = epoll_fd;
}

// ============================================================================
// 优选队列
// ============================================================================

route_calc_queue_t *route_calc_queue_create(void)
{
    route_calc_queue_t *q = g_malloc0(sizeof(route_calc_queue_t));
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

void route_calc_queue_destroy(route_calc_queue_t *q)
{
    if (!q)
    {
        return;
    }
    route_head_key_t *key = NULL;
    while ((key = (route_head_key_t *)g_queue_pop_head(q->q)) != NULL)
    {
        g_free(key);
    }
    g_queue_free(q->q);
    g_free(q);
}

int route_calc_queue_push(route_calc_queue_t *q, const route_head_key_t *key)
{
    if (!q || !key)
    {
        return -1;
    }

    /* 深拷贝 key，队列持有所有权 */
    route_head_key_t *copy = (route_head_key_t *)g_malloc(sizeof(route_head_key_t));
    if (!copy)
    {
        return -1;
    }
    memcpy(copy, key, sizeof(route_head_key_t));
    g_queue_push_tail(q->q, copy);
    q->count++;
    return 0;
}

int route_calc_queue_process(route_calc_queue_t *q, int batch_size)
{
    if (!q || batch_size <= 0 || !g_route_work_local || !g_route_work_local->rib)
    {
        return 0;
    }

    int processed = 0;
    route_head_key_t *key = NULL;
    while (processed < batch_size && (key = (route_head_key_t *)g_queue_pop_head(q->q)) != NULL)
    {
        q->count--;

        /* 在 RIB 中查找前缀头并触发优选计算 */
        const route_head_t *head =
            route_rib_lookup_head(g_route_work_local->rib, key->vrf_id, key->afi, &key->addr, key->prefix_len);
        if (head)
        {
            route_calc_on_path_add(head);
        }
        /* head 为 NULL 说明前缀已被完全删除，OS 和订阅者通知由 rib_del 回调同步处理 */

        g_free(key);
        processed++;
    }

    if (processed > 0)
    {
        LOG_DEBUG("[route_work] calc_queue 批量处理 %d 条，剩余 %u 条", processed, q->count);
    }
    return processed;
}

// ============================================================================
// 工作定时器
// ============================================================================

int route_work_timer_start(route_work_sentinel_t *sentinel, int *timerfd_out, uint32_t interval_ms)
{
    if (!sentinel || !timerfd_out)
    {
        return -1;
    }
    if (g_work_epoll_fd < 0)
    {
        LOG_WARN("[route_work] work timer start skipped, epoll fd not ready");
        return -1;
    }

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
    {
        LOG_PERROR("[route_work] timerfd_create 失败");
        return -1;
    }

    struct itimerspec its;
    its.it_value.tv_sec = (long)(interval_ms / 1000);
    its.it_value.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
    its.it_interval = its.it_value; /* 周期触发 */

    if (timerfd_settime(fd, 0, &its, NULL) < 0)
    {
        LOG_PERROR("[route_work] timerfd_settime 失败");
        close(fd);
        return -1;
    }

    sentinel->type = ROUTE_WORK_TIMER_TYPE;

    /* 注册到 epoll：data.ptr = (&sentinel | 1)，bit0=1 标记定时器事件 */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)((uintptr_t)sentinel | 1UL);
    if (epoll_ctl(g_work_epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("[route_work] epoll_ctl ADD work timer 失败");
        close(fd);
        return -1;
    }

    *timerfd_out = fd;
    LOG_DEBUG("[route_work] work timer 启动 interval=%ums fd=%d", interval_ms, fd);
    return 0;
}

void route_work_timer_stop(int *timerfd)
{
    if (!timerfd || *timerfd < 0)
    {
        return;
    }
    if (g_work_epoll_fd >= 0)
    {
        epoll_ctl(g_work_epoll_fd, EPOLL_CTL_DEL, *timerfd, NULL);
    }
    close(*timerfd);
    *timerfd = -1;
    LOG_DEBUG("[route_work] work timer 停止");
}

void route_work_process(int timerfd, route_calc_queue_t *q)
{
    /* 读取 timerfd 计数（必须，否则 epoll 持续触发） */
    if (timerfd >= 0)
    {
        uint64_t expirations;
        ssize_t n = read(timerfd, &expirations, sizeof(expirations));
        if (n < 0 && errno != EINTR && errno != EAGAIN)
        {
            LOG_PERROR("[route_work] 读取 work timerfd 失败");
        }
    }

    if (q)
    {
        int processed = route_calc_queue_process(q, ROUTE_WORK_BATCH_SIZE);
        if (processed > 0)
        {
            /* calc 可能改变依赖路由的 OS-installed 状态，随后重算迭代 watch。 */
            route_recompute_iter_paths();
        }
    }
}
