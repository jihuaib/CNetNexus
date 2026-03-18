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
    conn->state = BGP_CONN_STATE_OPEN_SENT;
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
