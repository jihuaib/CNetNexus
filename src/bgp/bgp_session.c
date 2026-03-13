/**
 * @file   bgp_session.c
 * @brief  BGP 会话结构生命周期及三类 timerfd 管理实现
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_session.h"

#include <glib.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "bgp_conn.h"
#include "log.h"

bgp_session_t *bgp_session_create(const net_addr_t *addr, uint32_t remote_as, bgp_vrf_t *vrf)
{
    bgp_session_t *sess = g_malloc0(sizeof(bgp_session_t));
    if (addr)
    {
        memcpy(&sess->neighbor_addr, addr, sizeof(*addr));
    }
    sess->remote_as = remote_as;
    sess->vrf = vrf;
    BIT_SET(sess->flags, BGP_SESS_CAP_DEFAULT);
    sess->pri_conn = NULL;
    sess->sec_conn = NULL;
    snprintf(sess->remote_id, sizeof(sess->remote_id), "0.0.0.0");
    sess->recv_len = 0;
    sess->negotiated_afs = NULL;

    /* 初始化三类 timerfd */
    sess->retry_timerfd = -1;
    sess->ka_timerfd = -1;
    sess->hold_timerfd = -1;

    /* 初始化哨兵：session 反向指针 + 类型标记 */
    sess->retry_sentinel.session = sess;
    sess->retry_sentinel.type = BGP_TIMER_TYPE_RETRY;
    sess->ka_sentinel.session = sess;
    sess->ka_sentinel.type = BGP_TIMER_TYPE_KEEPALIVE;
    sess->hold_sentinel.session = sess;
    sess->hold_sentinel.type = BGP_TIMER_TYPE_HOLD;

    char addr_str[64] = "";
    if (addr)
    {
        net_addr_to_str(addr, addr_str, sizeof(addr_str));
    }
    LOG_INFO("BGP 会话已创建: neighbor=%s AS=%u", addr_str, remote_as);
    return sess;
}

void bgp_session_destroy(bgp_session_t *session)
{
    if (!session)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&session->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP 会话已销毁: neighbor=%s", addr_str);

    bgp_conn_destroy(session->pri_conn);
    session->pri_conn = NULL;
    bgp_conn_destroy(session->sec_conn);
    session->sec_conn = NULL;

    if (session->negotiated_afs)
    {
        g_list_free_full(session->negotiated_afs, g_free);
        session->negotiated_afs = NULL;
    }

    /* peer_list 只存借用引用，仅释放链表节点 */
    if (session->peer_list)
    {
        g_list_free(session->peer_list);
        session->peer_list = NULL;
    }

    /* timerfd 兜底关闭（正常由各 cancel 函数提前处理） */
    if (session->retry_timerfd >= 0)
    {
        close(session->retry_timerfd);
        session->retry_timerfd = -1;
    }
    if (session->ka_timerfd >= 0)
    {
        close(session->ka_timerfd);
        session->ka_timerfd = -1;
    }
    if (session->hold_timerfd >= 0)
    {
        close(session->hold_timerfd);
        session->hold_timerfd = -1;
    }

    g_free(session);
}

// ============================================================================
// 内部辅助：创建并注册 timerfd
// ============================================================================

/**
 * @brief 通用 timerfd 创建并加入 epoll
 * @param sentinel  哨兵对象（内嵌于 session）
 * @param epoll_fd  BGP server 的 epoll fd
 * @param initial   首次超时秒数
 * @param interval  周期间隔秒数（0 = 单次）
 * @return timerfd（>=0）或 -1（失败）
 */
static int timer_arm(bgp_timer_sentinel_t *sentinel, int epoll_fd, uint16_t initial, uint16_t interval)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
    {
        LOG_PERROR("BGP: timerfd_create 失败");
        return -1;
    }

    struct itimerspec ts;
    ts.it_value.tv_sec = (time_t)initial;
    ts.it_value.tv_nsec = 0;
    ts.it_interval.tv_sec = (time_t)interval;
    ts.it_interval.tv_nsec = 0;

    if (timerfd_settime(tfd, 0, &ts, NULL) < 0)
    {
        LOG_PERROR("BGP: timerfd_settime 失败");
        close(tfd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)((uintptr_t)sentinel | 1UL);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD timerfd 失败");
        close(tfd);
        return -1;
    }

    return tfd;
}

/**
 * @brief 通用 timerfd 从 epoll 移除并关闭
 */
static void timer_cancel(int *tfd_ptr, int epoll_fd)
{
    if (*tfd_ptr < 0)
    {
        return;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, *tfd_ptr, NULL);
    close(*tfd_ptr);
    *tfd_ptr = -1;
}

// ============================================================================
// connect-retry 定时器
// ============================================================================

void bgp_session_arm_retry(bgp_session_t *sess, int epoll_fd, uint16_t retry_sec)
{
    if (sess->retry_timerfd >= 0)
    {
        return; /* 已调度，幂等 */
    }

    sess->retry_timerfd = timer_arm(&sess->retry_sentinel, epoll_fd, retry_sec, 0);
    if (sess->retry_timerfd >= 0)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: neighbor %s 调度 connect-retry，%u 秒后重试", addr_str, (unsigned)retry_sec);
    }
}

void bgp_session_cancel_retry(bgp_session_t *sess, int epoll_fd)
{
    timer_cancel(&sess->retry_timerfd, epoll_fd);
}

// ============================================================================
// keepalive 周期定时器
// ============================================================================

void bgp_session_arm_keepalive(bgp_session_t *sess, int epoll_fd, uint16_t ka_sec)
{
    if (ka_sec == 0 || sess->ka_timerfd >= 0)
    {
        return;
    }

    sess->ka_timerfd = timer_arm(&sess->ka_sentinel, epoll_fd, ka_sec, ka_sec);
    if (sess->ka_timerfd >= 0)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: neighbor %s 启动 keepalive 定时器，周期 %u 秒", addr_str, (unsigned)ka_sec);
    }
}

void bgp_session_cancel_keepalive(bgp_session_t *sess, int epoll_fd)
{
    timer_cancel(&sess->ka_timerfd, epoll_fd);
}

// ============================================================================
// hold time 超时定时器
// ============================================================================

void bgp_session_arm_hold(bgp_session_t *sess, int epoll_fd, uint16_t hold_sec)
{
    if (hold_sec == 0 || sess->hold_timerfd >= 0)
    {
        return;
    }

    sess->hold_timerfd = timer_arm(&sess->hold_sentinel, epoll_fd, hold_sec, 0);
    if (sess->hold_timerfd >= 0)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: neighbor %s 启动 hold 定时器，%u 秒", addr_str, (unsigned)hold_sec);
    }
}

void bgp_session_reset_hold(bgp_session_t *sess)
{
    if (sess->hold_timerfd < 0 || sess->negotiated_hold == 0)
    {
        return;
    }

    struct itimerspec ts;
    ts.it_value.tv_sec = (time_t)sess->negotiated_hold;
    ts.it_value.tv_nsec = 0;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;
    timerfd_settime(sess->hold_timerfd, 0, &ts, NULL);
}

void bgp_session_cancel_hold(bgp_session_t *sess, int epoll_fd)
{
    timer_cancel(&sess->hold_timerfd, epoll_fd);
}
