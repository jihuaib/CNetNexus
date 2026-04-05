/**
 * @file   bgp_conn.c
 * @brief  BGP TCP 连接处理器实现（负责 TCP 连接的建立与销毁）
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_conn.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bgp_session.h"
#include "bgp_worker.h"
#include "log.h"

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
