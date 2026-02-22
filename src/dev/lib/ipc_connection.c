/**
 * @file   ipc_connection.c
 * @brief  IPC TCP 连接管理实现
 * @author jhb
 * @date   2026/02/02
 */

#define LOG_TAG "ipc"

#include "ipc_connection.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errcode.h"
#include "log.h"

ipc_connection_t *ipc_connection_create(uint32_t remote_module_id, int is_initiator)
{
    ipc_connection_t *conn = g_malloc0(sizeof(ipc_connection_t));
    conn->remote_module_id = remote_module_id;
    conn->fd = -1;
    conn->state = IPC_CODISCONNECTED;
    conn->recv_len = 0;
    conn->last_heartbeat_sent = 0;
    conn->last_heartbeat_recv = 0;
    conn->reconnect_delay_ms = IPC_RECONNECT_DELAY_MIN;
    conn->next_reconnect_time = 0;
    conn->is_initiator = is_initiator;
    return conn;
}

void ipc_connection_destroy(ipc_connection_t *conn)
{
    if (!conn)
    {
        return;
    }
    ipc_connection_close(conn);
    g_free(conn);
}

int ipc_connection_initiate(ipc_connection_t *conn, const char *host, uint16_t port)
{
    if (!conn || !host)
    {
        return ERRCODE_FAIL;
    }

    /* 创建 socket */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_PERROR("socket");
        return ERRCODE_FAIL;
    }

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    /* 设置 TCP_NODELAY */
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    /* 设置 SO_KEEPALIVE */
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        close(fd);
        return ERRCODE_FAIL;
    }

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS)
    {
        close(fd);
        return ERRCODE_FAIL;
    }

    conn->fd = fd;
    conn->state = (ret == 0) ? IPC_COHANDSHAKING : IPC_COCONNECTING;
    conn->recv_len = 0;
    conn->last_heartbeat_recv = time(NULL);

    return ERRCODE_SUCCESS;
}

void ipc_connection_close(ipc_connection_t *conn)
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
    conn->state = IPC_CODISCONNECTED;
    conn->recv_len = 0;
}

int ipc_connection_send(ipc_connection_t *conn, const uint8_t *data, uint32_t len)
{
    if (!conn || conn->fd < 0 || !data || len == 0)
    {
        return ERRCODE_FAIL;
    }

    uint32_t total_sent = 0;
    while (total_sent < len)
    {
        ssize_t n = write(conn->fd, data + total_sent, len - total_sent);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                /* 等待可写 */
                usleep(1000);
                continue;
            }
            return ERRCODE_FAIL;
        }
        total_sent += (uint32_t)n;
    }

    return ERRCODE_SUCCESS;
}

void ipc_connection_reset_reconnect(ipc_connection_t *conn)
{
    if (conn)
    {
        conn->reconnect_delay_ms = IPC_RECONNECT_DELAY_MIN;
    }
}

void ipc_connection_backoff_reconnect(ipc_connection_t *conn)
{
    if (!conn)
    {
        return;
    }
    conn->reconnect_delay_ms *= 2;
    if (conn->reconnect_delay_ms > IPC_RECONNECT_DELAY_MAX)
    {
        conn->reconnect_delay_ms = IPC_RECONNECT_DELAY_MAX;
    }
    conn->next_reconnect_time = time(NULL) + (conn->reconnect_delay_ms / 1000);
}
