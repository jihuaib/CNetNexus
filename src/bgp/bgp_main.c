/**
 * @file   bgp_main.c
 * @brief  BGP 模块主入口，三阶段初始化和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_main.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp.h"
#include "bgp_bdr.h"
#include "bgp_cli.h"
#include "bgp_conn.h"
#include "bgp_db.h"
#include "bgp_instance.h"
#include "bgp_pkt.h"
#include "bgp_rib.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

/** BGP server epoll 单次最大事件数 */
#define BGP_MAX_EPOLL_EVENTS 16

/** BGP 协议标准端口 */
#define BGP_LISTEN_PORT 179

/** epoll data.ptr sentinel：区分 listen fd 事件与连接 fd 事件 */
static char bgp_listen_tag;

bgp_local_t *g_bgp_local = NULL;

// ============================================================================
// BGP listen socket 管理
// ============================================================================

void bgp_listen_start(void)
{
    if (!g_bgp_local || g_bgp_local->epoll_fd < 0)
    {
        return;
    }
    if (g_bgp_local->listen_fd >= 0)
    {
        return; /* 已在监听，幂等 */
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_PERROR("BGP: Failed to create listen socket");
        return;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(BGP_LISTEN_PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_PERROR("BGP: bind 0.0.0.0:179 failed");
        close(fd);
        return;
    }

    if (listen(fd, 32) < 0)
    {
        LOG_PERROR("BGP: listen failed");
        close(fd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = &bgp_listen_tag;
    if (epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD listen fd failed");
        close(fd);
        return;
    }

    g_bgp_local->listen_fd = fd;
    LOG_INFO("BGP: Listening on 0.0.0.0:179 (fd=%d)", fd);
}

void bgp_listen_stop(void)
{
    if (!g_bgp_local || g_bgp_local->listen_fd < 0)
    {
        return;
    }
    if (g_bgp_local->epoll_fd >= 0)
    {
        epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_local->listen_fd, NULL);
    }
    close(g_bgp_local->listen_fd);
    g_bgp_local->listen_fd = -1;
    LOG_INFO("BGP: Stopped listening on 0.0.0.0:179");
}

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_BGP,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// BGP server 辅助函数
// ============================================================================

/**
 * @brief 从 epoll 移除、销毁连接对象，并将 session 槽位置 NULL
 * @param slot &sess->pri_conn 或 &sess->sec_conn
 */
static void bgp_conn_close(bgp_conn_t **slot)
{
    if (!slot || !*slot)
    {
        return;
    }
    bgp_conn_t *conn = *slot;
    if (conn->fd >= 0)
    {
        epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    }
    bgp_conn_destroy(conn);
    *slot = NULL;
}

/**
 * @brief 将 sec_conn 提升为 pri_conn（pri_conn 必须已为 NULL）
 *
 * bgp_conn_t 对象地址不变，epoll data.ptr 无需更新。
 */
static void bgp_session_promote_sec(bgp_session_t *sess)
{
    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: Passive connection fd=%d promoted to pri_conn (neighbor=%s)", sess->sec_conn->fd, addr_str);
    sess->pri_conn = sess->sec_conn;
    sess->sec_conn = NULL;
}

/* 前向声明：connect-retry 调度辅助（定义在 bgp_handle_retry_timer 之前） */
static void bgp_arm_retry(bgp_session_t *sess);

// ============================================================================
// BGP server 线程 — 事件处理函数
// ============================================================================

/**
 * @brief 处理全局 listener 上的被动入站连接
 *
 * 碰撞策略（TCP 层，立即决策）：
 *   - pri_conn 已建立（!is_connecting）→ 拒绝本次被动连接
 *   - pri_conn 仍在 connecting   → 被动 TCP 先建立，暂存于 sec_conn，等 EPOLLOUT 触发后统一处理
 *   - 无 pri_conn               → 被动连接直接作为 pri_conn
 */
static void bgp_handle_passive_accept(void)
{
    bgp_protocol_t *proto = g_bgp_local->protocol;

    struct sockaddr_storage peer_sa;
    socklen_t addr_len = sizeof(peer_sa);
    int conn_fd = accept(g_bgp_local->listen_fd, (struct sockaddr *)&peer_sa, &addr_len);

    if (conn_fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("BGP: accept failed");
        }
        return;
    }

    /* 解析来源地址 */
    net_addr_t from_addr;
    memset(&from_addr, 0, sizeof(from_addr));
    char from_ip[64] = "";

    if (peer_sa.ss_family == AF_INET)
    {
        struct sockaddr_in *sa4 = (struct sockaddr_in *)&peer_sa;
        from_addr.family = AF_INET;
        memcpy(&from_addr.u.v4, &sa4->sin_addr, sizeof(sa4->sin_addr));
        inet_ntop(AF_INET, &sa4->sin_addr, from_ip, sizeof(from_ip));
    }
    else if (peer_sa.ss_family == AF_INET6)
    {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&peer_sa;
        from_addr.family = AF_INET6;
        memcpy(&from_addr.u.v6, &sa6->sin6_addr, sizeof(sa6->sin6_addr));
        inet_ntop(AF_INET6, &sa6->sin6_addr, from_ip, sizeof(from_ip));
    }
    else
    {
        LOG_WARN("BGP: Rejecting connection with unknown address family");
        close(conn_fd);
        return;
    }

    if (!proto)
    {
        LOG_WARN("BGP: Protocol not initialized, rejecting connection from %s", from_ip);
        close(conn_fd);
        return;
    }

    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    bgp_session_t *sess = vrf0 ? bgp_vrf_find_session(vrf0, &from_addr) : NULL;
    if (!sess || !bgp_vrf_neighbor_has_any_af(vrf0, &from_addr))
    {
        LOG_WARN("BGP: Rejecting connection from %s (no AF neighbor configured)", from_ip);
        close(conn_fd);
        return;
    }

    /* sec_conn 已存在：防御性拒绝（正常不应出现，碰撞检测期间最多两条连接并存） */
    if (sess->sec_conn)
    {
        LOG_WARN("BGP: Rejecting connection from %s (sec_conn already exists fd=%d)", from_ip, sess->sec_conn->fd);
        close(conn_fd);
        return;
    }

    /* session 已完全 ESTABLISHED → 拒绝新被动连接
     * RFC 4271 §6.8 碰撞检测仅发生在 OPEN_SENT / OPEN_CONFIRM 状态 */
    if (sess->pri_conn && !sess->pri_conn->is_connecting && sess->state == BGP_CONN_STATE_ESTABLISHED)
    {
        LOG_INFO("BGP: neighbor %s session established (fd=%d), rejecting new passive connection fd=%d", from_ip,
                 sess->pri_conn->fd, conn_fd);
        close(conn_fd);
        return;
    }

    LOG_INFO("BGP: neighbor %s passive TCP connection (fd=%d)", from_ip, conn_fd);

    /* 创建被动连接对象 */
    bgp_conn_t *conn = bgp_conn_create(sess);
    conn->fd = conn_fd;
    conn->is_active = FALSE;
    conn->is_connecting = FALSE;
    memcpy(&conn->peer_addr, &from_addr, sizeof(from_addr));
    sess->state = BGP_CONN_STATE_OPEN_SENT;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    if (epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD passive connection failed");
        bgp_conn_destroy(conn);
        return;
    }

    if (sess->pri_conn)
    {
        /* pri_conn 正在进行中（TCP 握手中或 BGP OPEN 协商中）：
         * 暂存为 sec_conn，等待 RFC 4271 §6.8 碰撞检测在 OPEN 层面决策 */
        if (sess->pri_conn->is_connecting)
        {
            LOG_INFO("BGP: neighbor %s passive connection fd=%d (active fd=%d still TCP handshaking, §6.8 pending)",
                     from_ip, conn_fd, sess->pri_conn->fd);
        }
        else
        {
            LOG_INFO("BGP: neighbor %s passive connection fd=%d (active fd=%d in OPEN negotiation, §6.8 pending)",
                     from_ip, conn_fd, sess->pri_conn->fd);
        }
        sess->sec_conn = conn;
    }
    else
    {
        /* 无已存在连接，被动连接直接作为 pri_conn */
        sess->pri_conn = conn;
    }

    GList *af_peers = bgp_vrf_get_session_peers(vrf0, &from_addr);
    bgp_pkt_send_open(conn, proto->as_number, vrf0->router_id, af_peers);
    g_list_free(af_peers);
}

/**
 * @brief 处理主动连接完成事件（EPOLLOUT）
 *
 * 碰撞策略（TCP 层，立即决策）：
 *   - 连接失败                          → 关闭 pri_conn；若 sec_conn 存在则提升
 *   - 连接成功且 sec_conn 存在（被动先建立）→ 关闭主动连接，提升 sec_conn
 *   - 连接成功且无 sec_conn              → pri_conn 胜出，转 EPOLLIN，发送 OPEN
 *
 * @param conn session->pri_conn（主动连接）
 */
static void bgp_handle_active_connect(bgp_conn_t *conn)
{
    bgp_session_t *sess = conn->session;
    bgp_protocol_t *proto = g_bgp_local->protocol;

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len);

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    if (err != 0)
    {
        LOG_WARN("BGP: Active connection to %s failed: %s (fd=%d)", addr_str, strerror(err), conn->fd);
        bgp_conn_close(&sess->pri_conn);
        if (sess->sec_conn)
        {
            /* 主动失败，被动连接（已发过 OPEN）顶上 */
            bgp_session_promote_sec(sess);
        }
        else
        {
            /* 无后备连接，按 connect-retry 定时器调度重连 */
            bgp_arm_retry(sess);
        }
        return;
    }

    /* 主动连接成功：转 EPOLLIN，发送 OPEN
     * 若 sec_conn 同时存在（被动 TCP 也已建立），不在 TCP 层做决策，
     * 让两条连接都进入 BGP OPEN 协商，由 RFC 4271 §6.8 在 OPEN 层面基于 BGP-ID 决策 */
    if (sess->sec_conn)
    {
        LOG_INFO("BGP: Active TCP to %s established (fd=%d), sec_conn fd=%d also present, §6.8 collision pending",
                 addr_str, conn->fd, sess->sec_conn->fd);
    }
    else
    {
        LOG_INFO("BGP: Active TCP connection to %s established (fd=%d)", addr_str, conn->fd);
    }
    conn->is_connecting = FALSE;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);

    if (proto)
    {
        bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
        GList *af_peers = vrf0 ? bgp_vrf_get_session_peers(vrf0, &sess->neighbor_addr) : NULL;
        bgp_pkt_send_open(conn, proto->as_number, vrf0 ? vrf0->router_id : 0, af_peers);
        g_list_free(af_peers);
    }
}

/**
 * @brief 处理已建立连接上的 BGP 数据（EPOLLIN）
 *
 * 碰撞检测已在 TCP 层完成，此处负责数据处理、定时器管理和连接关闭。
 *
 * @param conn 接收数据的连接结构
 */
static void bgp_handle_data(bgp_conn_t *conn)
{
    bgp_session_t *sess = conn->session;
    bgp_conn_t **slot = (sess->pri_conn == conn) ? &sess->pri_conn : &sess->sec_conn;
    gboolean was_active = conn->is_active;
    bgp_conn_state_t old_state = sess->state;

    int ret = bgp_pkt_on_data(conn);

    if (ret == BGP_PKT_ON_DATA_COLLISION_CLOSE_ME)
    {
        /* RFC 4271 §6.8：当前 conn 因碰撞检测应关闭，另一条连接继续协商 */
        char addr_str[64];
        net_addr_to_str(&conn->peer_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: §6.8 collision: closing %s connection (fd=%d) with %s", conn->is_active ? "active" : "passive",
                 conn->fd, addr_str);
        bgp_conn_close(slot);
        /* 若 pri_conn 被关闭，将 sec_conn 提升为 pri_conn */
        if (!sess->pri_conn && sess->sec_conn)
        {
            bgp_session_promote_sec(sess);
        }
        return;
    }

    if (ret == BGP_PKT_ON_DATA_COLLISION_CLOSE_OTHER)
    {
        /* RFC 4271 §6.8：当前 conn 保留（已进入 OPEN_CONFIRM），关闭另一条连接 */
        bgp_conn_t **other_slot = (slot == &sess->pri_conn) ? &sess->sec_conn : &sess->pri_conn;
        if (*other_slot)
        {
            char addr_str[64];
            net_addr_to_str(&(*other_slot)->peer_addr, addr_str, sizeof(addr_str));
            LOG_INFO("BGP: §6.8 collision: closing %s connection (fd=%d) with %s",
                     (*other_slot)->is_active ? "active" : "passive", (*other_slot)->fd, addr_str);
            bgp_conn_close(other_slot);
        }
        /* 若 pri_conn 被关闭（当前 conn 在 sec_conn 槽），将其提升 */
        if (!sess->pri_conn && sess->sec_conn)
        {
            bgp_session_promote_sec(sess);
        }
        return;
    }

    if (ret < 0)
    {
        /* 连接关闭：取消 KA 和 Hold 定时器 */
        bgp_session_cancel_keepalive(sess, g_bgp_local->epoll_fd);
        bgp_session_cancel_hold(sess, g_bgp_local->epoll_fd);

        char addr_str[64];
        net_addr_to_str(&conn->peer_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: Connection with %s closed (fd=%d)", addr_str, conn->fd);
        bgp_conn_close(slot);

        if (!sess->pri_conn && !sess->sec_conn)
        {
            (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);
        }

        if (was_active && !sess->pri_conn)
        {
            bgp_arm_retry(sess);
        }
        return;
    }

    /* 状态变为 ESTABLISHED：启动 KA 和 Hold 定时器 */
    if (old_state != BGP_CONN_STATE_ESTABLISHED && sess->state == BGP_CONN_STATE_ESTABLISHED)
    {
        bgp_protocol_t *proto = g_bgp_local->protocol;
        bgp_vrf_t *vrf0 = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
        uint16_t ka_sec = vrf0 ? vrf0->keepalive : BGP_TIMER_DEFAULT_KEEPALIVE;
        bgp_session_arm_keepalive(sess, g_bgp_local->epoll_fd, ka_sec);
        if (sess->negotiated_hold > 0)
        {
            bgp_session_arm_hold(sess, g_bgp_local->epoll_fd, sess->negotiated_hold);
        }
    }

    /* 收到 KA 或 UPDATE：重置 Hold 定时器 */
    if (sess->hold_reset_pending)
    {
        sess->hold_reset_pending = FALSE;
        bgp_session_reset_hold(sess);
    }
}

// ============================================================================
// connect-retry 定时器调度辅助（获取 VRF 配置并委托给 bgp_session）
// ============================================================================

/**
 * @brief 从 VRF 取出 connect_retry 间隔，调用 bgp_session_arm_retry
 */
static void bgp_arm_retry(bgp_session_t *sess)
{
    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(g_bgp_local->protocol, BGP_VRF_PUBLIC_ID);
    uint16_t retry_sec = vrf0 ? vrf0->connect_retry : BGP_TIMER_DEFAULT_CONNECT_RETRY;
    bgp_session_arm_retry(sess, g_bgp_local->epoll_fd, retry_sec);
}

/**
 * @brief 处理 keepalive 周期 timerfd 到期（向对端发送 KEEPALIVE）
 */
static void bgp_handle_ka_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->ka_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read ka timerfd");
    }

    /* 仅在 ESTABLISHED 状态发送 KA；connection does not exist时定时器应已被取消 */
    bgp_conn_t *conn = sess->pri_conn;
    if (!conn || conn->fd < 0 || sess->state != BGP_CONN_STATE_ESTABLISHED)
    {
        return;
    }

    if (bgp_pkt_send_keepalive(conn) < 0)
    {
        char addr_str[64];
        net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BGP: Failed to send KEEPALIVE to %s, closing connection", addr_str);
        bgp_session_cancel_keepalive(sess, g_bgp_local->epoll_fd);
        bgp_session_cancel_hold(sess, g_bgp_local->epoll_fd);
        bgp_conn_close(&sess->pri_conn);
        (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);
        bgp_arm_retry(sess);
    }
}

/**
 * @brief 处理 hold time 超时 timerfd 到期（session 超时，关闭连接）
 */
static void bgp_handle_hold_timer(bgp_session_t *sess)
{
    uint64_t expirations;
    if (read(sess->hold_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read hold timerfd");
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_WARN("BGP: neighbor %s hold time expired, closing session", addr_str);

    bgp_session_cancel_keepalive(sess, g_bgp_local->epoll_fd);
    bgp_session_cancel_hold(sess, g_bgp_local->epoll_fd);
    bgp_conn_close(&sess->pri_conn);
    bgp_conn_close(&sess->sec_conn);
    (void)bgp_vrf_purge_session_routes(sess->vrf, &sess->neighbor_addr);
    bgp_arm_retry(sess);
}

/**
 * @brief 处理 connect-retry timerfd 到期事件
 *
 * @param sentinel epoll data.ptr 去掉 bit0 后得到的 bgp_timer_sentinel_t*
 */
static void bgp_handle_retry_timer(bgp_session_t *sess)
{
    /* 读取 timerfd，清除 EPOLLIN（不读会持续触发） */
    uint64_t expirations;
    if (read(sess->retry_timerfd, &expirations, sizeof(expirations)) < 0 && errno != EAGAIN)
    {
        LOG_PERROR("BGP: Failed to read timerfd");
    }

    /* 关闭 timerfd，从 epoll 移除 */
    bgp_session_cancel_retry(sess, g_bgp_local->epoll_fd);

    /* 检查 session 当前状态 */
    if (sess->pri_conn)
    {
        return; /* 已有连接，无需重试 */
    }

    bgp_protocol_t *proto = g_bgp_local->protocol;
    bgp_vrf_t *vrf0 = proto ? bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID) : NULL;
    if (!vrf0 || !bgp_vrf_neighbor_has_any_af(vrf0, &sess->neighbor_addr))
    {
        return; /* AF 已禁用，放弃重试 */
    }

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: connect-retry expired, reconnecting neighbor %s", addr_str);
    bgp_server_start_active_conn(sess);
}

// ============================================================================
// BGP server 线程
// ============================================================================

static void *bgp_server_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "bgp-server");
    log_set_tag("bgp");

    struct epoll_event events[BGP_MAX_EPOLL_EVENTS];

    while (g_bgp_local && g_bgp_local->running)
    {
        int nfds = epoll_wait(g_bgp_local->epoll_fd, events, BGP_MAX_EPOLL_EVENTS, 1000);

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

            if (events[i].data.ptr == (void *)&bgp_listen_tag)
            {
                bgp_handle_passive_accept();
                continue;
            }

            /* bit0=1 表示 timerfd 事件（retry / keepalive / hold） */
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
                bgp_handle_active_connect(conn);
            }
            else
            {
                bgp_handle_data(conn);
            }
        }
    }

    return NULL;
}

// ============================================================================
// session 连接管理 API
// ============================================================================

void bgp_server_start_active_conn(bgp_session_t *session)
{
    if (!session || !g_bgp_local || g_bgp_local->epoll_fd < 0)
    {
        return;
    }

    if (session->pri_conn)
    {
        return;
    }

    bgp_conn_t *conn = bgp_conn_create(session);
    int fd = bgp_conn_start_active(conn, &session->neighbor_addr, g_bgp_local->epoll_fd);
    if (fd < 0)
    {
        char addr_str[64];
        net_addr_to_str(&session->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BGP: Failed to initiate active connection for neighbor %s, scheduling connect-retry", addr_str);
        bgp_conn_destroy(conn);
        bgp_arm_retry(session);
        return;
    }

    session->pri_conn = conn;
    (void)fd;
}

void bgp_server_stop_session_conns(bgp_session_t *session)
{
    if (!session || !g_bgp_local)
    {
        return;
    }
    bgp_session_cancel_retry(session, g_bgp_local->epoll_fd);
    bgp_session_cancel_keepalive(session, g_bgp_local->epoll_fd);
    bgp_session_cancel_hold(session, g_bgp_local->epoll_fd);
    bgp_conn_close(&session->pri_conn);
    bgp_conn_close(&session->sec_conn);
    (void)bgp_vrf_purge_session_routes(session->vrf, &session->neighbor_addr);
}

// ============================================================================
// Phase 1: MODULE_START
// ============================================================================

static void bgp_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");
    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    dev_ipc_connect(ctx, DEV_MODULE_ID_ROUTE, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_ROUTE);
    LOG_INFO("Connected to CFG, DB and ROUTE");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT
// ============================================================================

static void bgp_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY
// ============================================================================

static void bgp_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY - Initializing database tables and restoring BGP state");

    /* 建表（幂等，首次启动时创建，后续启动时跳过） */
    if (bgp_db_init(ctx) != 0)
    {
        LOG_ERROR("BGP: Database table initialization failed");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* 仅恢复：表不存在（BGP 未曾配置）时静默返回 NULL，不建表也不写默认值 */
    uint32_t ret = bgp_db_restore(ctx);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_ERROR("BGP: Failed to restore state from database");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("BGP: Failed to create epoll");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_bgp_local->epoll_fd = epoll_fd;

    g_bgp_local->running = 1;
    if (pthread_create(&g_bgp_local->server_thread, NULL, bgp_server_thread, NULL) != 0)
    {
        LOG_PERROR("BGP: Failed to create server thread");
        close(epoll_fd);
        g_bgp_local->epoll_fd = DEV_INVALID_FD;
        g_bgp_local->running = 0;
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    if (g_bgp_local->protocol)
    {
        /* 从数据库恢复了配置，立即开始监听 179 端口 */
        bgp_listen_start();

        if (g_bgp_local->protocol->vrf_hash)
        {
            GHashTableIter vrf_iter;
            gpointer vrf_key, vrf_val;
            g_hash_table_iter_init(&vrf_iter, g_bgp_local->protocol->vrf_hash);
            while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
            {
                bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
                GHashTableIter sess_iter;
                gpointer sess_key, sess_val;
                g_hash_table_iter_init(&sess_iter, vrf->sess_hash);
                while (g_hash_table_iter_next(&sess_iter, &sess_key, &sess_val))
                {
                    bgp_session_t *sess = (bgp_session_t *)sess_val;
                    if (bgp_vrf_neighbor_has_any_af(vrf, &sess->neighbor_addr))
                    {
                        bgp_server_start_active_conn(sess);
                    }
                }
            }
        }
    }

    LOG_INFO("BGP server thread started");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void bgp_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("BGP module cleanup");

    bgp_cli_cleanup_state();

    g_bgp_local->running = 0;
    if (g_bgp_local->server_thread)
    {
        pthread_join(g_bgp_local->server_thread, NULL);
        g_bgp_local->server_thread = 0;
    }

    if (g_bgp_local->protocol && g_bgp_local->protocol->vrf_hash)
    {
        GHashTableIter vrf_iter;
        gpointer vrf_key, vrf_val;
        g_hash_table_iter_init(&vrf_iter, g_bgp_local->protocol->vrf_hash);
        while (g_hash_table_iter_next(&vrf_iter, &vrf_key, &vrf_val))
        {
            bgp_vrf_t *vrf = (bgp_vrf_t *)vrf_val;
            GHashTableIter sess_iter;
            gpointer sess_key, sess_val;
            g_hash_table_iter_init(&sess_iter, vrf->sess_hash);
            while (g_hash_table_iter_next(&sess_iter, &sess_key, &sess_val))
            {
                bgp_session_t *sess = (bgp_session_t *)sess_val;
                bgp_server_stop_session_conns(sess);
            }
        }
    }

    bgp_listen_stop();

    if (g_bgp_local->epoll_fd >= 0)
    {
        close(g_bgp_local->epoll_fd);
        g_bgp_local->epoll_fd = DEV_INVALID_FD;
    }

    if (g_bgp_local->protocol)
    {
        bgp_protocol_destroy(g_bgp_local->protocol);
        g_bgp_local->protocol = NULL;
    }

    g_bgp_local->dev_ipc_ctx = NULL;
    g_free(g_bgp_local);
    g_bgp_local = NULL;

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// ROUTE 模块推送的路由更新处理
// ============================================================================

/**
 * @brief 将一条 ROUTE 路由条目导入（或撤销）到 BGP RIB
 * @return 1=已处理，0=被过滤/忽略
 */
static int bgp_import_route_entry(const route_msg_entry_t *entry)
{
    if (!entry)
    {
        return 0;
    }

    if (!g_bgp_local || !g_bgp_local->protocol)
    {
        return 0;
    }

    /* 当前仅处理默认公网 VRF */
    if (entry->vrf_id != ROUTE_VRF_DEFAULT)
    {
        return 0;
    }

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(g_bgp_local->protocol, entry->vrf_id);
    if (!vrf)
    {
        return 0;
    }

    /* 查找对应 AFI/SAFI 实例（不自动创建：未配置 AF 则忽略） */
    bgp_afi_t afi = (bgp_afi_t)entry->afi;
    bgp_safi_t safi = (entry->safi == 0) ? BGP_SAFI_UNICAST : (bgp_safi_t)entry->safi;
    bgp_instance_t *inst = (bgp_instance_t *)g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));

    if (!inst || !(inst->import_protos & (1u << entry->protocol)))
    {
        /* 该 AF 未配置 import-route 对应协议，丢弃 */
        return 0;
    }

    /* 构建 NLRI entry（直接使用二进制地址，无字符串转换） */
    bgp_nlri_entry_t nlri;
    memset(&nlri, 0, sizeof(nlri));
    nlri.afi = (uint16_t)afi;
    nlri.safi = (uint8_t)safi;
    nlri.type = BGP_NLRI_PREFIX;
    nlri.prefix.prefix.prefix_len = entry->prefix_len;
    nlri.prefix.prefix.addr = entry->prefix_addr;
    nlri.prefix.has_rd = false;
    nlri.prefix.has_label = false;

    /* key 字段仅用于显示，从二进制地址格式化 */
    char prefix_str[64];
    net_addr_to_str(&entry->prefix_addr, prefix_str, sizeof(prefix_str));
    snprintf(nlri.key, sizeof(nlri.key), "%s/%u", prefix_str, (unsigned)entry->prefix_len);

    net_addr_t src = entry->source_addr;

    if (entry->is_withdraw)
    {
        bgp_rib_unreach_one(inst->rib, &nlri, &src);
        LOG_DEBUG("BGP: Import route withdraw %s", nlri.key);
    }
    else
    {
        /* 构建合成 BGP 属性（ORIGIN=INCOMPLETE，AS_PATH 为空） */
        bgp_attr_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.origin = BGP_ORIGIN_INCOMPLETE;
        attr.local_pref = 100;
        attr.has_local_pref = true;

        bgp_nexthop_t nexthop;
        memset(&nexthop, 0, sizeof(nexthop));
        nexthop.has_link_local = false;
        nexthop.global = entry->nexthop_addr;

        bgp_rib_reach_one(inst->rib, &nlri, &src, &attr, &nexthop);

        char nh_str[64], src_str[64];
        net_addr_to_str(&entry->nexthop_addr, nh_str, sizeof(nh_str));
        net_addr_to_str(&entry->source_addr, src_str, sizeof(src_str));
        LOG_DEBUG("BGP: Import route add %s nh=%s src=%s", nlri.key, nh_str, src_str);
    }

    return 1;
}

/**
 * @brief 处理 ROUTE 模块推送的增量路由更新（ROUTE_MSG_TYPE_UPDATE）
 */
static void bgp_handle_route_update(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_msg_entry_t))
    {
        LOG_WARN("BGP: ROUTE_UPDATE payload too short: %u bytes", msg->payload_len);
        dev_ipc_message_free(msg);
        return;
    }

    const route_msg_entry_t *entry = (const route_msg_entry_t *)msg->payload;
    (void)bgp_import_route_entry(entry);

    dev_ipc_message_free(msg);
    (void)ctx;
}

/**
 * @brief 处理 ROUTE 模块全量路由快照（ROUTE_MSG_TYPE_REPORT）
 */
static void bgp_handle_route_report(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(route_msg_report_t))
    {
        LOG_WARN("BGP: ROUTE_REPORT payload too short: %u bytes", msg->payload_len);
        dev_ipc_message_free(msg);
        return;
    }

    const route_msg_report_t *report = (const route_msg_report_t *)msg->payload;
    size_t expected_len = sizeof(route_msg_report_t) + (size_t)report->route_count * sizeof(route_msg_entry_t);
    if (msg->payload_len < expected_len)
    {
        LOG_WARN("BGP: ROUTE_REPORT malformed: payload=%u, expected>=%zu", msg->payload_len, expected_len);
        dev_ipc_message_free(msg);
        return;
    }

    uint32_t imported = 0;
    for (uint32_t i = 0; i < report->route_count; ++i)
    {
        imported += (uint32_t)bgp_import_route_entry(&report->routes[i]);
    }

    LOG_INFO("BGP: ROUTE_REPORT received %u entries, imported %u", report->route_count, imported);
    dev_ipc_message_free(msg);
    (void)ctx;
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            bgp_on_start(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            bgp_on_connect(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            bgp_on_ready(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            bgp_on_shutdown(ctx, msg);
            return;
        case CLI_MSG_TYPE:
            LOG_DEBUG("Received CLI command message");
            bgp_cli_handle_message(msg);
            break;
        case CLI_MSG_TYPE_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            bgp_cli_handle_continue(msg);
            break;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            bgp_bdr_show_config(msg);
            return;
        case ROUTE_MSG_TYPE_UPDATE:
            bgp_handle_route_update(ctx, msg);
            return;
        case ROUTE_MSG_TYPE_REPORT:
            bgp_handle_route_report(ctx, msg);
            return;
        default:
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// Module initialization
// ============================================================================

int bgp_module_init(void)
{
    LOG_INFO("Module initialization");

    /* 注册所有内置 AFI/SAFI 解析器 */
    bgp_parse_init();

    /* 注册所有内置 UPDATE 编码器 */
    bgp_pkt_build_init();

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_BGP, "bgp", DEV_MODULE_PORT_BGP, bgp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_bgp_local = g_malloc0(sizeof(bgp_local_t));
    g_bgp_local->dev_ipc_ctx = ctx;
    g_bgp_local->epoll_fd = DEV_INVALID_FD;
    g_bgp_local->listen_fd = -1;
    return 0;
}
