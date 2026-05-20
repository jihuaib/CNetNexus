/**
 * @file   bgp_conn.c
 * @brief  BGP TCP 连接处理器实现（负责 TCP 连接的建立、监听、接入、销毁）
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp_fsm.h"
#include "bgp_pkt.h"
#include "bgp_protocol.h"
#include "bgp_session.h"
#include "bgp_vrf.h"
#include "bgp_worker.h"
#include "log.h"
#include "vrf.h"

/** BGP 协议标准端口 */
#define BGP_PORT 179

// ============================================================================
// 生命周期
// ============================================================================

static void bgp_conn_init(bgp_conn_t *conn)
{
    if (!conn)
    {
        return;
    }
    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->is_active = FALSE;
    conn->is_connecting = FALSE;
    conn->has_ttl = FALSE;
    conn->last_socket_error = 0;
}

static void bgp_conn_cleanup(bgp_conn_t *conn)
{
    if (!conn)
    {
        return;
    }
    if (conn->fd >= 0)
    {
        close(conn->fd);
        conn->fd = -1;
    }
    conn->is_active = FALSE;
    conn->is_connecting = FALSE;
}

bgp_conn_t *bgp_conn_create(struct bgp_session *sess)
{
    bgp_conn_t *conn = g_malloc0(sizeof(bgp_conn_t));
    bgp_conn_init(conn);
    conn->session = sess;
    return conn;
}

void bgp_conn_destroy(bgp_conn_t *conn)
{
    if (!conn)
    {
        return;
    }
    bgp_conn_cleanup(conn);
    g_free(conn);
}

// ============================================================================
// 主动连接
// ============================================================================

static int bgp_bind_local_addr(int sock, const net_addr_t *local_addr)
{
    if (!local_addr || local_addr->family == 0)
    {
        return 0;
    }

    if (local_addr->family == AF_INET)
    {
        struct sockaddr_in local_sa;
        memset(&local_sa, 0, sizeof(local_sa));
        local_sa.sin_family = AF_INET;
        local_sa.sin_port = htons(0);
        memcpy(&local_sa.sin_addr, &local_addr->u.v4, sizeof(local_addr->u.v4));
        return bind(sock, (struct sockaddr *)&local_sa, sizeof(local_sa));
    }

    if (local_addr->family == AF_INET6)
    {
        struct sockaddr_in6 local_sa6;
        memset(&local_sa6, 0, sizeof(local_sa6));
        local_sa6.sin6_family = AF_INET6;
        local_sa6.sin6_port = htons(0);
        memcpy(&local_sa6.sin6_addr, &local_addr->u.v6, sizeof(local_addr->u.v6));
        return bind(sock, (struct sockaddr *)&local_sa6, sizeof(local_sa6));
    }

    errno = EAFNOSUPPORT;
    return -1;
}

static int bgp_set_socket_ttl(int sock, sa_family_t family, uint8_t ttl)
{
    if (ttl == 0)
    {
        return 0;
    }

    int ttl_opt = (int)ttl;
    if (family == AF_INET)
    {
        return setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl_opt, sizeof(ttl_opt));
    }
    if (family == AF_INET6)
    {
        return setsockopt(sock, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl_opt, sizeof(ttl_opt));
    }

    errno = EAFNOSUPPORT;
    return -1;
}

int bgp_conn_start_active(bgp_conn_t *conn, const net_addr_t *peer_addr, int epoll_fd)
{
    if (!conn || !peer_addr || conn->fd >= 0)
    {
        return -1;
    }

    int sock = socket(peer_addr->family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0)
    {
        LOG_PERROR("BGP: Failed to create active connection socket");
        return -1;
    }

    /* 非 public VRF 必须 SO_BINDTODEVICE 到 L3VRF 设备，否则 connect 走主表无法到达对端 */
    if (conn->session && conn->session->vrf && conn->session->vrf->vrf_id != BGP_VRF_PUBLIC_ID)
    {
        const vrf_api_cache_entry_t *vrf_entry = vrf_api_cache_lookup(conn->session->vrf->vrf_id);
        const char *vrf_name = vrf_entry ? vrf_entry->name : NULL;
        if (vrf_name && vrf_name[0] != '\0' &&
            setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, vrf_name, strlen(vrf_name) + 1) < 0)
        {
            LOG_PERROR("BGP: bind active socket to VRF device %s failed", vrf_name);
            close(sock);
            return -1;
        }
    }

    if (conn->has_ttl)
    {
        if (bgp_set_socket_ttl(sock, peer_addr->family, conn->ttl) < 0)
        {
            char addr_str[64];
            net_addr_to_str(peer_addr, addr_str, sizeof(addr_str));
            LOG_PERROR("BGP: set socket TTL=%u for %s failed", conn->ttl, addr_str);
            close(sock);
            return -1;
        }
    }

    if (conn->has_local_addr)
    {
        if (conn->local_addr.family != peer_addr->family)
        {
            char peer_str[64];
            char local_str[64];
            net_addr_to_str(peer_addr, peer_str, sizeof(peer_str));
            net_addr_to_str(&conn->local_addr, local_str, sizeof(local_str));
            LOG_WARN("BGP: local source-address family mismatch (local=%s peer=%s)", local_str, peer_str);
            close(sock);
            return -1;
        }

        if (bgp_bind_local_addr(sock, &conn->local_addr) < 0)
        {
            char local_str[64];
            net_addr_to_str(&conn->local_addr, local_str, sizeof(local_str));
            LOG_PERROR("BGP: bind source address %s failed", local_str);
            close(sock);
            return -1;
        }
    }

    int ret;
    if (peer_addr->family == AF_INET)
    {
        struct sockaddr_in peer_sa;
        memset(&peer_sa, 0, sizeof(peer_sa));
        peer_sa.sin_family = AF_INET;
        peer_sa.sin_port = htons(BGP_PORT);
        memcpy(&peer_sa.sin_addr, &peer_addr->u.v4, sizeof(peer_addr->u.v4));
        ret = connect(sock, (struct sockaddr *)&peer_sa, sizeof(peer_sa));
    }
    else if (peer_addr->family == AF_INET6)
    {
        struct sockaddr_in6 peer_sa;
        memset(&peer_sa, 0, sizeof(peer_sa));
        peer_sa.sin6_family = AF_INET6;
        peer_sa.sin6_port = htons(BGP_PORT);
        memcpy(&peer_sa.sin6_addr, &peer_addr->u.v6, sizeof(peer_addr->u.v6));
        ret = connect(sock, (struct sockaddr *)&peer_sa, sizeof(peer_sa));
    }
    else
    {
        char addr_str[64];
        net_addr_to_str(peer_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BGP: Active connection: unsupported address family %d (peer=%s)", peer_addr->family, addr_str);
        close(sock);
        return -1;
    }

    if (ret < 0 && errno != EINPROGRESS)
    {
        char addr_str[64];
        net_addr_to_str(peer_addr, addr_str, sizeof(addr_str));
        LOG_PERROR("BGP: connect to %s:%d failed", addr_str, BGP_PORT);
        close(sock);
        return -1;
    }

    /* 注册 EPOLLOUT（等待 connect 完成）和 EPOLLERR，data.ptr 指向 conn 本身 */
    struct epoll_event ev;
    ev.events = EPOLLOUT | EPOLLERR;
    ev.data.ptr = conn;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD active connection socket failed");
        close(sock);
        return -1;
    }

    memcpy(&conn->peer_addr, peer_addr, sizeof(*peer_addr));
    conn->fd = sock;
    conn->is_active = TRUE;
    conn->is_connecting = TRUE;

    char addr_str[64];
    net_addr_to_str(peer_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP: Initiating active connection to %s:%d (fd=%d)", addr_str, BGP_PORT, sock);
    return sock;
}

int bgp_conn_get_local_addr(const bgp_conn_t *conn, net_addr_t *out_addr)
{
    if (!conn || conn->fd < 0 || !out_addr)
    {
        return -1;
    }

    memset(out_addr, 0, sizeof(*out_addr));

    struct sockaddr_storage local_sa;
    socklen_t local_len = sizeof(local_sa);
    memset(&local_sa, 0, sizeof(local_sa));

    if (getsockname(conn->fd, (struct sockaddr *)&local_sa, &local_len) < 0)
    {
        /*
         * 回退到显式 source-interface 地址（仅主动建连场景可用）。
         * 对被动建连必须依赖 getsockname() 获取本地端点地址。
         */
        if (conn->has_local_addr && conn->local_addr.family != 0)
        {
            *out_addr = conn->local_addr;
            return 0;
        }
        return -1;
    }

    if (local_sa.ss_family == AF_INET)
    {
        const struct sockaddr_in *sa4 = (const struct sockaddr_in *)&local_sa;
        out_addr->family = AF_INET;
        memcpy(&out_addr->u.v4, &sa4->sin_addr, sizeof(sa4->sin_addr));
        return 0;
    }

    if (local_sa.ss_family == AF_INET6)
    {
        const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)&local_sa;
        out_addr->family = AF_INET6;
        memcpy(&out_addr->u.v6, &sa6->sin6_addr, sizeof(sa6->sin6_addr));
        return 0;
    }

    return -1;
}

// ============================================================================
// 连接关闭
// ============================================================================

void bgp_conn_close(struct bgp_session *sess, bgp_conn_t **slot, int epoll_fd)
{
    if (!slot || !*slot)
    {
        return;
    }
    bgp_conn_t *conn = *slot;
    if (sess)
    {
        if (slot == &sess->pri_conn)
        {
            sess->pri_last_socket_error = conn->last_socket_error;
        }
        else if (slot == &sess->sec_conn)
        {
            sess->sec_last_socket_error = conn->last_socket_error;
        }
    }
    if (conn->fd >= 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    }
    /*
     * 不立即 g_free：同批 epoll 事件可能仍持有此 conn 指针。
     * bgp_conn_cleanup 关闭 fd 并置 fd=-1，epoll 循环的
     * conn->fd < 0 检查会安全跳过。延迟到 epoll 循环结束后释放。
     */
    bgp_conn_cleanup(conn);
    conn->session = NULL;
    g_bgp_work_local->deferred_conns = g_slist_prepend(g_bgp_work_local->deferred_conns, conn);
    *slot = NULL;
}

void bgp_conn_flush_deferred(void)
{
    GSList *list = g_bgp_work_local->deferred_conns;
    g_bgp_work_local->deferred_conns = NULL;
    for (GSList *n = list; n; n = n->next)
    {
        g_free(n->data);
    }
    g_slist_free(list);
}

// ============================================================================
// 监听（per-VRF IPv4 + IPv6）
// ============================================================================

static const char *bgp_listen_vrf_name(const bgp_vrf_t *vrf)
{
    if (!vrf || vrf->vrf_id == BGP_VRF_PUBLIC_ID)
    {
        return VRF_PUBLIC_VRF_NAME;
    }

    const vrf_api_cache_entry_t *entry = vrf_api_cache_lookup(vrf->vrf_id);
    return entry ? entry->name : NULL;
}

static int bgp_listen_open_socket(bgp_vrf_t *vrf, int family, int *slot)
{
    if (!vrf || !slot || *slot >= 0 || !g_bgp_work_local || g_bgp_work_local->epoll_fd < 0)
    {
        return -1;
    }

    const char *vrf_name = bgp_listen_vrf_name(vrf);
    if (!vrf_name)
    {
        LOG_WARN("BGP: skip listen for VRF %u: VRF name not in cache", vrf->vrf_id);
        return -1;
    }

    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_PERROR("BGP: create listen socket failed");
        return -1;
    }

    int opt = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    if (family == AF_INET6)
    {
        int v6_only = 1;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only));
    }
    if (vrf->vrf_id != BGP_VRF_PUBLIC_ID &&
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, vrf_name, strlen(vrf_name) + 1) < 0)
    {
        LOG_PERROR("BGP: bind listen socket to VRF device %s failed", vrf_name);
        close(fd);
        return -1;
    }

    if (family == AF_INET)
    {
        struct sockaddr_in addr4;
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = INADDR_ANY;
        addr4.sin_port = htons(BGP_PORT);
        if (bind(fd, (struct sockaddr *)&addr4, sizeof(addr4)) < 0)
        {
            LOG_PERROR("BGP: bind %s 0.0.0.0:179 failed", vrf_name);
            close(fd);
            return -1;
        }
    }
    else
    {
        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr = in6addr_any;
        addr6.sin6_port = htons(BGP_PORT);
        if (bind(fd, (struct sockaddr *)&addr6, sizeof(addr6)) < 0)
        {
            LOG_PERROR("BGP: bind %s [::]:179 failed", vrf_name);
            close(fd);
            return -1;
        }
    }

    if (listen(fd, 32) < 0)
    {
        LOG_PERROR("BGP: listen %s %s failed", vrf_name, family == AF_INET ? "IPv4" : "IPv6");
        close(fd);
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = slot;
    if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD %s listen fd failed", family == AF_INET ? "IPv4" : "IPv6");
        close(fd);
        return -1;
    }

    *slot = fd;
    LOG_INFO("BGP: VRF %s listening on %s:179 (fd=%d)", vrf_name, family == AF_INET ? "0.0.0.0" : "[::]", fd);
    return 0;
}

void bgp_listen_start_vrf(bgp_vrf_t *vrf)
{
    if (!vrf)
    {
        return;
    }
    (void)bgp_listen_open_socket(vrf, AF_INET, &vrf->listen_fd);
    (void)bgp_listen_open_socket(vrf, AF_INET6, &vrf->listen_fd_v6);
}

void bgp_listen_stop_vrf(bgp_vrf_t *vrf)
{
    if (!vrf)
    {
        return;
    }

    if (vrf->listen_fd >= 0)
    {
        if (g_bgp_work_local && g_bgp_work_local->epoll_fd >= 0)
        {
            epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, vrf->listen_fd, NULL);
        }
        close(vrf->listen_fd);
        vrf->listen_fd = -1;
    }

    if (vrf->listen_fd_v6 >= 0)
    {
        if (g_bgp_work_local && g_bgp_work_local->epoll_fd >= 0)
        {
            epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_DEL, vrf->listen_fd_v6, NULL);
        }
        close(vrf->listen_fd_v6);
        vrf->listen_fd_v6 = -1;
    }
}

void bgp_listen_start(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol || !g_bgp_work_local->protocol->vrf_hash)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_bgp_work_local->protocol->vrf_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        bgp_listen_start_vrf((bgp_vrf_t *)value);
    }
}

void bgp_listen_stop(void)
{
    if (!g_bgp_work_local || !g_bgp_work_local->protocol || !g_bgp_work_local->protocol->vrf_hash)
    {
        return;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_bgp_work_local->protocol->vrf_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        bgp_listen_stop_vrf((bgp_vrf_t *)value);
    }
}

gboolean bgp_listen_handle_event_ptr(void *ptr)
{
    if (!ptr || !g_bgp_work_local || !g_bgp_work_local->protocol || !g_bgp_work_local->protocol->vrf_hash)
    {
        return FALSE;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_bgp_work_local->protocol->vrf_hash);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        bgp_vrf_t *vrf = (bgp_vrf_t *)value;
        if (ptr == &vrf->listen_fd)
        {
            if (vrf->listen_fd >= 0)
            {
                bgp_conn_handle_passive_accept(vrf->listen_fd, vrf->vrf_id);
            }
            return TRUE;
        }
        if (ptr == &vrf->listen_fd_v6)
        {
            if (vrf->listen_fd_v6 >= 0)
            {
                bgp_conn_handle_passive_accept(vrf->listen_fd_v6, vrf->vrf_id);
            }
            return TRUE;
        }
    }
    return FALSE;
}

// ============================================================================
// 被动入站 / 主动完成
// ============================================================================

void bgp_conn_handle_passive_accept(int listen_fd, uint32_t listen_vrf_id)
{
    bgp_protocol_t *proto = g_bgp_work_local->protocol;

    struct sockaddr_storage peer_sa;
    socklen_t addr_len = sizeof(peer_sa);
    int conn_fd = accept(listen_fd, (struct sockaddr *)&peer_sa, &addr_len);

    if (conn_fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            LOG_PERROR("BGP: accept failed");
        }
        return;
    }

    /* 被动连接必须设为非阻塞，否则 recv() 会阻塞 worker 线程 */
    int flags = fcntl(conn_fd, F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK);
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

    bgp_vrf_t *vrf = bgp_protocol_get_vrf(proto, listen_vrf_id);
    bgp_session_t *sess = vrf ? bgp_vrf_find_session(vrf, &from_addr) : NULL;
    if (!sess || !bgp_vrf_neighbor_has_any_af(vrf, &from_addr))
    {
        LOG_WARN("BGP: Rejecting connection from %s in VRF %u (no AF neighbor configured)", from_ip, listen_vrf_id);
        close(conn_fd);
        return;
    }

    /* router-id 未配置：本端 BGP Identifier 无效，不允许建立被动连接 */
    if (vrf->router_id == 0)
    {
        LOG_WARN("BGP: Rejecting connection from %s in VRF %u (router-id not configured)", from_ip, listen_vrf_id);
        close(conn_fd);
        return;
    }

    if (sess->sec_conn)
    {
        LOG_WARN("BGP: Rejecting connection from %s (sec_conn already exists fd=%d)", from_ip, sess->sec_conn->fd);
        close(conn_fd);
        return;
    }

    if (sess->pri_conn && !sess->pri_conn->is_connecting && sess->fsm_state == BGP_FSM_STATE_ESTABLISHED)
    {
        LOG_INFO("BGP: neighbor %s session established (fd=%d), rejecting new passive connection fd=%d", from_ip,
                 sess->pri_conn->fd, conn_fd);
        close(conn_fd);
        return;
    }

    LOG_INFO("BGP: neighbor %s passive TCP connection (fd=%d)", from_ip, conn_fd);

    bgp_conn_t *conn = bgp_conn_create(sess);
    conn->fd = conn_fd;
    conn->is_active = FALSE;
    conn->is_connecting = FALSE;
    memcpy(&conn->peer_addr, &from_addr, sizeof(from_addr));

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    if (epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD passive connection failed");
        bgp_conn_destroy(conn);
        return;
    }

    if (sess->pri_conn)
    {
        /* 碰撞场景：pri_conn 已存在，新连接暂挂为 sec_conn，直接发 OPEN（不经 FSM） */
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
        sess->sec_last_socket_error = 0;

        /* 向 sec_conn 发送 OPEN，碰撞将在收到对端 OPEN 时解决 */
        GList *af_peers = bgp_vrf_get_session_peers(vrf, &sess->neighbor_addr);
        bgp_pkt_send_open(conn, proto->as_number, vrf->router_id, af_peers);
        g_list_free(af_peers);
        /* FSM 状态不变（跟踪 pri_conn） */
    }
    else
    {
        sess->pri_conn = conn;
        sess->pri_last_socket_error = 0;
        /* 触发 FSM 事件：发送 OPEN 并迁移状态 */
        bgp_fsm_event(sess, BGP_EVT_TCP_CONNECTION_CONFIRMED);
    }
}

void bgp_conn_handle_active_connect(bgp_conn_t *conn)
{
    bgp_session_t *sess = conn->session;

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len);
    conn->last_socket_error = err;

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    if (err != 0)
    {
        LOG_WARN("BGP: Active connection to %s failed: %s (errno=%d, fd=%d)", addr_str, strerror(err), err, conn->fd);
        bgp_fsm_event(sess, BGP_EVT_TCP_CONNECTION_FAILS);
        return;
    }

    conn->last_socket_error = 0;

    if (sess->sec_conn)
    {
        LOG_INFO("BGP: Active TCP to %s established (fd=%d), sec_conn fd=%d also present, §6.8 collision pending",
                 addr_str, conn->fd, sess->sec_conn->fd);
    }
    else
    {
        LOG_INFO("BGP: Active TCP connection to %s established (fd=%d)", addr_str, conn->fd);
    }

    /* 将 epoll 改为 EPOLLIN（接收 BGP 报文），清除连接中标志 */
    conn->is_connecting = FALSE;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    epoll_ctl(g_bgp_work_local->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);

    bgp_fsm_event(sess, BGP_EVT_TCP_CR_ACKED);
}
