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

#include "bgp.h"
#include "bgp_bmp_thread.h"
#include "bgp_conn.h"
#include "bgp_fsm.h"
#include "bgp_pkt.h"
#include "bgp_vrf.h"
#include "bgp_work.h"
#include "bgp_worker.h"
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
    sess->pri_last_socket_error = 0;
    sess->sec_last_socket_error = 0;
    /* remote_id / local_router_id 初始值为 0（g_malloc0 已置零） */
    sess->negotiated_afs = NULL;

    /* FSM 初始状态 */
    sess->fsm_state = BGP_FSM_STATE_IDLE;

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
    LOG_INFO("BGP session created: neighbor=%s AS=%u", addr_str, remote_as);
    return sess;
}

void bgp_session_update_type(bgp_session_t *sess, uint32_t local_as)
{
    if (!sess || local_as == 0u || sess->remote_as == 0u)
    {
        sess->sess_type = BGP_SESS_TYPE_UNKNOWN;
    }
    else if (sess->remote_as == local_as)
    {
        sess->sess_type = BGP_SESS_TYPE_IBGP;
    }
    else
    {
        sess->sess_type = BGP_SESS_TYPE_EBGP;
    }
}

void bgp_session_destroy(bgp_session_t *session)
{
    if (!session)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&session->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP session destroyed: neighbor=%s", addr_str);

    bgp_conn_destroy(session->pri_conn);
    session->pri_conn = NULL;
    bgp_conn_destroy(session->sec_conn);
    session->sec_conn = NULL;

    if (session->negotiated_afs)
    {
        g_array_free(session->negotiated_afs, TRUE);
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
// 协商参数重置
// ============================================================================

void bgp_session_reset_negotiated(bgp_session_t *sess)
{
    if (!sess)
    {
        return;
    }

    /* 清除 OPEN 协商填入的对端信息 */
    sess->remote_id = 0;
    sess->remote_caps = 0;
    sess->remote_hold = 0;
    sess->established_at_usec = 0;

    /* 清除本次 OPEN 发出时记录的本地快照 */
    sess->local_router_id = 0;
    sess->local_caps = 0;

    /* 清除协商结果 */
    sess->negotiated_caps = 0;
    sess->negotiated_hold = 0;

    /* 清空地址族协商列表（保留 GArray 对象，仅清除内容） */
    if (sess->negotiated_afs)
    {
        g_array_set_size(sess->negotiated_afs, 0);
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_DEBUG("BGP: neighbor %s negotiated params reset", addr_str);
}

// ============================================================================
// 断邻居
// ============================================================================

/**
 * @brief 关闭单条连接槽位：从 epoll 移除、销毁对象、槽位置 NULL
 *
 * 与 bgp_main.c 中同名静态函数逻辑一致，此处供 session 层使用。
 */
static void session_conn_close(bgp_conn_t **slot, int epoll_fd)
{
    if (!slot || !*slot)
    {
        return;
    }
    bgp_conn_t *conn = *slot;
    if (conn->fd >= 0 && epoll_fd >= 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    }
    bgp_conn_destroy(conn);
    *slot = NULL;
}

void bgp_neighbor_down(bgp_session_t *sess, int epoll_fd)
{
    if (!sess)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: neighbor %s admin down — sending NOTIFICATION and resetting session", addr_str);

    /* BMP Peer Down 通知（在连接关闭前发送，确保 per-peer header 信息完整） */
    if (sess->fsm_state == BGP_FSM_STATE_ESTABLISHED)
    {
        bgp_bmp_thread_notify_peer_down(sess, BGP_BMP_PEER_DOWN_LOCAL_NO_NOTIFY);
    }

    /* 步骤 1：向已完成 TCP 握手的主连接发送 NOTIFICATION Cease/Admin-Reset
     *         TCP 握手中（is_connecting）的连接尚未进入 BGP 协议层，跳过 */
    if (sess->pri_conn && sess->pri_conn->fd >= 0 && !sess->pri_conn->is_connecting)
    {
        bgp_pkt_send_notification(sess->pri_conn, BGP_ERR_CEASE, BGP_CEASE_ADMIN_RESET);
    }

    /* 步骤 2：取消所有定时器 */
    bgp_session_cancel_keepalive(sess, epoll_fd);
    bgp_session_cancel_hold(sess, epoll_fd);
    bgp_session_cancel_retry(sess, epoll_fd);

    /* 步骤 3：关闭所有 TCP 连接，清除该邻居的 RIB 路由 */
    if (sess->pri_conn)
    {
        sess->pri_last_socket_error = sess->pri_conn->last_socket_error;
    }
    if (sess->sec_conn)
    {
        sess->sec_last_socket_error = sess->sec_conn->last_socket_error;
    }
    session_conn_close(&sess->pri_conn, epoll_fd);
    session_conn_close(&sess->sec_conn, epoll_fd);

    /* 离开所属 subgroup（对每个 AF 实例） */
    for (GList *l = sess->peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        if (peer && peer->inst)
        {
            bgp_subgroup_peer_leave(peer, sess);
        }
    }

    bgp_worker_flush_peer_routes(sess->vrf ? sess->vrf->vrf_id : BGP_VRF_PUBLIC_ID, &sess->neighbor_addr);
    (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);

    /* 步骤 4：重置 OPEN 协商产生的所有参数，确保重连时完整重新协商 */
    bgp_session_reset_negotiated(sess);

    /* 步骤 5：会话已被本地 reset 并等待 retry，FSM 必须离开旧状态。
     * 若保留 Established/OpenSent 等旧状态，后续 ConnectRetryTimer_Expires /
     * TcpConnectionConfirmed 会走到错误分支，导致本可重建的会话卡死。 */
    sess->fsm_state = BGP_FSM_STATE_ACTIVE;

    /* 步骤 6：按 VRF 配置的 connect-retry 间隔调度重连定时器
     *         到期后触发 bgp_server_start_active_conn → bgp_pkt_send_open */
    uint16_t retry_sec =
        (sess->vrf && sess->vrf->connect_retry > 0) ? sess->vrf->connect_retry : BGP_TIMER_DEFAULT_CONNECT_RETRY;
    bgp_session_arm_retry(sess, epoll_fd, retry_sec);
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
        LOG_PERROR("BGP: timerfd_create failed");
        return -1;
    }

    struct itimerspec ts;
    ts.it_value.tv_sec = (time_t)initial;
    ts.it_value.tv_nsec = 0;
    ts.it_interval.tv_sec = (time_t)interval;
    ts.it_interval.tv_nsec = 0;

    if (timerfd_settime(tfd, 0, &ts, NULL) < 0)
    {
        LOG_PERROR("BGP: timerfd_settime failed");
        close(tfd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)((uintptr_t)sentinel | 1UL);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD timerfd failed");
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
        LOG_INFO("BGP: neighbor %s scheduling connect-retry, retrying in %u seconds", addr_str, (unsigned)retry_sec);
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
        LOG_INFO("BGP: neighbor %s starting keepalive timer, interval %u seconds", addr_str, (unsigned)ka_sec);
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
        LOG_INFO("BGP: neighbor %s starting hold timer, %u seconds", addr_str, (unsigned)hold_sec);
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
