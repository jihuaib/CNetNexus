/**
 * @file   bgp_bmp.c
 * @brief  BMP 实例运行态：生命周期管理、连接管理（BMP 线程内执行）
 * @author jhb
 * @date   2026/03/29
 */
#include "bgp_bmp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "bgp_bmp_pkt.h"
#include "bgp_bmp_thread.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 定时器辅助
// ============================================================================

/**
 * @brief 创建 timerfd 并注册到 epoll（sentinel bit0=1 标记）
 */
static int bmp_timer_arm(bgp_bmp_timer_sentinel_t *sentinel, int epoll_fd, uint16_t initial, uint16_t interval)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0)
    {
        LOG_PERROR("BMP: timerfd_create failed");
        return -1;
    }

    struct itimerspec ts;
    ts.it_value.tv_sec = (time_t)initial;
    ts.it_value.tv_nsec = 0;
    ts.it_interval.tv_sec = (time_t)interval;
    ts.it_interval.tv_nsec = 0;

    if (timerfd_settime(tfd, 0, &ts, NULL) < 0)
    {
        LOG_PERROR("BMP: timerfd_settime failed");
        close(tfd);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)((uintptr_t)sentinel | 1UL);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &ev) < 0)
    {
        LOG_PERROR("BMP: epoll_ctl ADD timerfd failed");
        close(tfd);
        return -1;
    }

    return tfd;
}

/**
 * @brief 取消 timerfd 并关闭
 */
static void bmp_timer_cancel(int *tfd_ptr, int epoll_fd)
{
    if (*tfd_ptr < 0)
    {
        return;
    }
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, *tfd_ptr, NULL);
    close(*tfd_ptr);
    *tfd_ptr = -1;
}

/**
 * @brief 读取 timerfd 计数（消耗事件）
 */
static void bmp_timer_consume(int tfd)
{
    uint64_t expirations;
    (void)read(tfd, &expirations, sizeof(expirations));
}

// ============================================================================
// 生命周期
// ============================================================================

bgp_bmp_instance_t *bgp_bmp_instance_create(const char *name)
{
    bgp_bmp_instance_t *inst = g_malloc0(sizeof(bgp_bmp_instance_t));
    if (!inst)
    {
        return NULL;
    }

    g_strlcpy(inst->name, name, sizeof(inst->name));
    inst->fd = -1;
    inst->reconnect_timerfd = -1;
    inst->stats_timerfd = -1;
    inst->conn_state = BGP_BMP_CONN_IDLE;
    inst->reconnect_interval = 30;
    inst->monitor_all = TRUE;

    /* 初始化 sentinel */
    inst->reconnect_sentinel.type = BMP_TIMER_RECONNECT;
    inst->stats_sentinel.type = BMP_TIMER_STATS;
    inst->conn_sentinel.type = BMP_TIMER_CONN;

    return inst;
}

void bgp_bmp_instance_destroy(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (!inst)
    {
        return;
    }

    if (inst->conn_state == BGP_BMP_CONN_UP)
    {
        bgp_bmp_send_termination(inst);
    }
    bgp_bmp_disconnect(inst, epoll_fd);

    if (inst->monitor_peers)
    {
        g_hash_table_destroy(inst->monitor_peers);
        inst->monitor_peers = NULL;
    }

    g_free(inst);
}

gboolean bgp_bmp_should_monitor(const bgp_bmp_instance_t *inst, const char *peer_ip)
{
    if (!inst || !peer_ip)
    {
        return FALSE;
    }
    if (inst->monitor_all)
    {
        return TRUE;
    }
    if (!inst->monitor_peers)
    {
        return FALSE;
    }
    return g_hash_table_contains(inst->monitor_peers, peer_ip);
}

// ============================================================================
// TCP 连接管理
// ============================================================================

/**
 * @brief 调度重连定时器
 */
static void bmp_schedule_reconnect(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (inst->reconnect_timerfd >= 0)
    {
        return;
    }
    if (inst->reconnect_interval == 0)
    {
        return;
    }

    inst->reconnect_timerfd = bmp_timer_arm(&inst->reconnect_sentinel, epoll_fd, inst->reconnect_interval, 0);
    if (inst->reconnect_timerfd >= 0)
    {
        inst->conn_state = BGP_BMP_CONN_WAIT;
        LOG_INFO("BMP '%s': scheduling reconnect in %u seconds", inst->name, inst->reconnect_interval);
    }
}

/**
 * @brief 连接成功后发送 Initiation + 请求初始 Peer Ups
 */
static void bmp_on_connect_up(bgp_bmp_instance_t *inst, int epoll_fd)
{
    bgp_bmp_send_initiation(inst);

    /* 异步请求 BGP worker 收集当前 ESTABLISHED 邻居 */
    bgp_bmp_request_initial_peers(inst->name);

    /* 如果配置了 stats interval，启动 stats 定时器 */
    if (inst->stats_interval > 0 && inst->stats_timerfd < 0)
    {
        inst->stats_timerfd =
            bmp_timer_arm(&inst->stats_sentinel, epoll_fd, inst->stats_interval, inst->stats_interval);
    }
}

void bgp_bmp_connect(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (!inst || inst->fd >= 0)
    {
        return;
    }

    if (inst->collector_port == 0 || net_addr_is_zero(&inst->collector_addr))
    {
        LOG_WARN("BMP '%s': no collector configured, skipping connect", inst->name);
        return;
    }

    bmp_timer_cancel(&inst->reconnect_timerfd, epoll_fd);

    int sock = socket(inst->collector_addr.family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0)
    {
        LOG_PERROR("BMP '%s': socket() failed", inst->name);
        bmp_schedule_reconnect(inst, epoll_fd);
        return;
    }

    int ret;
    if (inst->collector_addr.family == AF_INET)
    {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(inst->collector_port);
        memcpy(&sa.sin_addr, &inst->collector_addr.u.v4, sizeof(inst->collector_addr.u.v4));
        ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    }
    else if (inst->collector_addr.family == AF_INET6)
    {
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons(inst->collector_port);
        memcpy(&sa.sin6_addr, &inst->collector_addr.u.v6, sizeof(inst->collector_addr.u.v6));
        ret = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    }
    else
    {
        LOG_WARN("BMP '%s': unsupported address family %d", inst->name, inst->collector_addr.family);
        close(sock);
        bmp_schedule_reconnect(inst, epoll_fd);
        return;
    }

    if (ret == 0)
    {
        /* 立即连接成功 */
        inst->fd = sock;
        inst->conn_state = BGP_BMP_CONN_UP;
        inst->connected_at_usec = g_get_real_time();

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
        ev.data.ptr = (void *)((uintptr_t)&inst->conn_sentinel | 1UL);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) < 0)
        {
            LOG_PERROR("BMP '%s': epoll_ctl ADD fd failed", inst->name);
            close(sock);
            inst->fd = -1;
            inst->conn_state = BGP_BMP_CONN_IDLE;
            bmp_schedule_reconnect(inst, epoll_fd);
            return;
        }

        char addr_str[64];
        net_addr_to_str(&inst->collector_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BMP '%s': connected to %s:%u (fd=%d)", inst->name, addr_str, inst->collector_port, sock);

        bmp_on_connect_up(inst, epoll_fd);
        return;
    }

    if (errno != EINPROGRESS)
    {
        char addr_str[64];
        net_addr_to_str(&inst->collector_addr, addr_str, sizeof(addr_str));
        LOG_PERROR("BMP '%s': connect to %s:%u failed", inst->name, addr_str, inst->collector_port);
        close(sock);
        bmp_schedule_reconnect(inst, epoll_fd);
        return;
    }

    /* EINPROGRESS */
    inst->fd = sock;
    inst->conn_state = BGP_BMP_CONN_CONNECTING;

    struct epoll_event ev;
    ev.events = EPOLLOUT | EPOLLERR;
    ev.data.ptr = (void *)((uintptr_t)&inst->conn_sentinel | 1UL);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) < 0)
    {
        LOG_PERROR("BMP '%s': epoll_ctl ADD connecting fd failed", inst->name);
        close(sock);
        inst->fd = -1;
        inst->conn_state = BGP_BMP_CONN_IDLE;
        bmp_schedule_reconnect(inst, epoll_fd);
        return;
    }

    char addr_str[64];
    net_addr_to_str(&inst->collector_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BMP '%s': connecting to %s:%u (fd=%d, EINPROGRESS)", inst->name, addr_str, inst->collector_port, sock);
}

void bgp_bmp_handle_connect_result(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (!inst || inst->fd < 0)
    {
        return;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(inst->fd, SOL_SOCKET, SO_ERROR, &err, &len);

    if (err != 0)
    {
        char addr_str[64];
        net_addr_to_str(&inst->collector_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BMP '%s': connect to %s:%u failed: %s (errno=%d)", inst->name, addr_str, inst->collector_port,
                 strerror(err), err);

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, inst->fd, NULL);
        close(inst->fd);
        inst->fd = -1;
        inst->conn_state = BGP_BMP_CONN_IDLE;
        bmp_schedule_reconnect(inst, epoll_fd);
        return;
    }

    inst->conn_state = BGP_BMP_CONN_UP;
    inst->connected_at_usec = g_get_real_time();

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
    ev.data.ptr = (void *)((uintptr_t)&inst->conn_sentinel | 1UL);
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, inst->fd, &ev);

    char addr_str[64];
    net_addr_to_str(&inst->collector_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BMP '%s': connected to %s:%u (fd=%d)", inst->name, addr_str, inst->collector_port, inst->fd);

    bmp_on_connect_up(inst, epoll_fd);
}

void bgp_bmp_handle_read(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (!inst || inst->fd < 0)
    {
        return;
    }

    uint8_t buf[64];
    ssize_t n = read(inst->fd, buf, sizeof(buf));

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
    {
        char addr_str[64];
        net_addr_to_str(&inst->collector_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BMP '%s': connection to %s:%u lost (read=%zd, errno=%d)", inst->name, addr_str, inst->collector_port,
                 n, (n < 0) ? errno : 0);

        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, inst->fd, NULL);
        close(inst->fd);
        inst->fd = -1;

        bmp_timer_cancel(&inst->stats_timerfd, epoll_fd);

        inst->conn_state = BGP_BMP_CONN_IDLE;
        inst->connected_at_usec = 0;

        bmp_schedule_reconnect(inst, epoll_fd);
    }
}

void bgp_bmp_handle_reconnect(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (!inst)
    {
        return;
    }

    bmp_timer_consume(inst->reconnect_timerfd);
    bmp_timer_cancel(&inst->reconnect_timerfd, epoll_fd);

    LOG_INFO("BMP '%s': reconnect timer fired, attempting connection", inst->name);
    bgp_bmp_connect(inst, epoll_fd);
}

void bgp_bmp_disconnect(bgp_bmp_instance_t *inst, int epoll_fd)
{
    if (!inst)
    {
        return;
    }

    if (inst->fd >= 0)
    {
        if (epoll_fd >= 0)
        {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, inst->fd, NULL);
        }
        close(inst->fd);
        inst->fd = -1;
    }

    bmp_timer_cancel(&inst->reconnect_timerfd, epoll_fd);
    bmp_timer_cancel(&inst->stats_timerfd, epoll_fd);

    inst->conn_state = BGP_BMP_CONN_IDLE;
    inst->connected_at_usec = 0;
}

// ============================================================================
// 报文发送委托
// ============================================================================

void bgp_bmp_send_initiation(bgp_bmp_instance_t *inst)
{
    bgp_bmp_pkt_send_initiation(inst);
}

void bgp_bmp_send_termination(bgp_bmp_instance_t *inst)
{
    bgp_bmp_pkt_send_termination(inst, BGP_BMP_TERM_REASON_ADMIN);
}

void bgp_bmp_send_peer_up(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer)
{
    bgp_bmp_pkt_send_peer_up(inst, peer);
}

void bgp_bmp_send_peer_down(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer, uint8_t reason)
{
    bgp_bmp_pkt_send_peer_down(inst, peer, reason);
}

void bgp_bmp_send_route_monitoring(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer, const uint8_t *bgp_pdu,
                                   uint16_t pdu_len)
{
    bgp_bmp_pkt_send_route_monitoring(inst, peer, bgp_pdu, pdu_len);
}

void bgp_bmp_send_stats_report(bgp_bmp_instance_t *inst)
{
    bgp_bmp_pkt_send_stats_report(inst);
}

void bgp_bmp_handle_stats_timer(bgp_bmp_instance_t *inst)
{
    if (!inst)
    {
        return;
    }

    bmp_timer_consume(inst->stats_timerfd);

    if (inst->conn_state == BGP_BMP_CONN_UP)
    {
        bgp_bmp_send_stats_report(inst);
    }
}
