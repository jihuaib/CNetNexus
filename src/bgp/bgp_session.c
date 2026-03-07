/**
 * @file   bgp_session.c
 * @brief  BGP 会话结构生命周期实现
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
    /* 默认使能 AS4 和 Route Refresh 能力 */
    BIT_SET(sess->flags, BGP_SESS_CAP_DEFAULT);
    sess->pri_conn = NULL;
    sess->sec_conn = NULL;
    snprintf(sess->remote_id, sizeof(sess->remote_id), "0.0.0.0");
    sess->recv_len = 0;
    sess->negotiated_afs = NULL;
    sess->retry_timerfd = -1;
    sess->retry_sentinel.session = sess;

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

    /* peer_list 只存借用引用，仅释放链表节点，不销毁 peer 本身 */
    if (session->peer_list)
    {
        g_list_free(session->peer_list);
        session->peer_list = NULL;
    }

    /* timerfd 正常由 bgp_session_cancel_retry 提前关闭；此处作兜底处理 */
    if (session->retry_timerfd >= 0)
    {
        close(session->retry_timerfd);
        session->retry_timerfd = -1;
    }

    g_free(session);
}

// ============================================================================
// connect-retry timerfd 管理
// ============================================================================

void bgp_session_arm_retry(bgp_session_t *sess, int epoll_fd, uint16_t retry_sec)
{
    if (sess->retry_timerfd >= 0)
    {
        return; /* 已调度，幂等 */
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
    {
        LOG_PERROR("BGP: timerfd_create 失败");
        return;
    }

    struct itimerspec ts;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec = (time_t)retry_sec;
    ts.it_value.tv_nsec = 0;
    if (timerfd_settime(tfd, 0, &ts, NULL) < 0)
    {
        LOG_PERROR("BGP: timerfd_settime 失败");
        close(tfd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)((uintptr_t)&sess->retry_sentinel | 1UL);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD timerfd 失败");
        close(tfd);
        return;
    }

    sess->retry_timerfd = tfd;

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: neighbor %s 调度 connect-retry，%u 秒后重试", addr_str, (unsigned)retry_sec);
}

void bgp_session_cancel_retry(bgp_session_t *sess, int epoll_fd)
{
    if (sess->retry_timerfd < 0)
    {
        return;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, sess->retry_timerfd, NULL);
    close(sess->retry_timerfd);
    sess->retry_timerfd = -1;
}
