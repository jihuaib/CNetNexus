/**
 * @file   ipc_context.c
 * @brief  IPC 上下文实现，包含 IO 线程、连接管理和公共 API
 * @author jhb
 * @date   2026/02/02
 */

#define LOG_TAG "ipc"

#include "ipc_context.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errcode.h"
#include "ipc_frame.h"
#include "log.h"

/** 将模块 ID 格式化为可读字符串（用于日志），结果写入 buf */
static const char *fmt_module_id(uint32_t module_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "0x%08X", module_id);
    return buf;
}

#define IPC_MAX_EPOLL_EVENTS 32

// 全局 IPC 上下文实例，用于其他模块方便获取
ipc_context_t *g_ipc_context = NULL;

// ============================================================================
// 内部函数前向声明
// ============================================================================

static ipc_connection_t *find_connection(ipc_context_t *ctx, uint32_t module_id);
static ipc_connection_t *find_connection_by_fd(ipc_context_t *ctx, int fd);
static int send_handshake(ipc_context_t *ctx, ipc_connection_t *conn);
static int send_heartbeat(ipc_context_t *ctx, ipc_connection_t *conn);
static void process_received_data(ipc_context_t *ctx, ipc_connection_t *conn);
static void handle_frame(ipc_context_t *ctx, ipc_connection_t *conn, ipc_message_t *header, const uint8_t *payload);
static void check_heartbeats(ipc_context_t *ctx);
static void attempt_reconnects(ipc_context_t *ctx);
static void accept_new_connection(ipc_context_t *ctx);

// ============================================================================
// 连接查找
// ============================================================================

static ipc_connection_t *find_connection(ipc_context_t *ctx, uint32_t module_id)
{
    ipc_connection_t *fallback = NULL;
    for (int i = 0; i < ctx->num_connections; i++)
    {
        if (ctx->connections[i] && ctx->connections[i]->remote_module_id == module_id)
        {
            if (ctx->connections[i]->state == IPC_COCONNECTED)
            {
                return ctx->connections[i]; // 优先返回已连接的
            }
            if (!fallback)
            {
                fallback = ctx->connections[i];
            }
        }
    }
    return fallback;
}

static ipc_connection_t *find_connection_by_fd(ipc_context_t *ctx, int fd)
{
    for (int i = 0; i < ctx->num_connections; i++)
    {
        if (ctx->connections[i] && ctx->connections[i]->fd == fd)
        {
            return ctx->connections[i];
        }
    }
    return NULL;
}

// ============================================================================
// 握手和心跳
// ============================================================================

static int send_handshake(ipc_context_t *ctx, ipc_connection_t *conn)
{
    /* 握手消息: payload 为本模块 ID (4 字节，网络字节序) */
    uint32_t id_be = htonl(ctx->module_id);
    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = IPC_MSG_TYPE_HANDSHAKE;
    msg.src_module_id = ctx->module_id;
    msg.dst_module_id = conn->remote_module_id;
    msg.request_id = 0;
    msg.payload = &id_be;
    msg.payload_len = sizeof(uint32_t);
    msg.free_fn = NULL;

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (ipc_frame_serialize(&msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int ret = ipc_connection_send(conn, buf, buf_len);
    g_free(buf);

    if (ret == ERRCODE_SUCCESS)
    {
        conn->state = IPC_COHANDSHAKING;
    }

    return ret;
}

static int send_heartbeat(ipc_context_t *ctx, ipc_connection_t *conn)
{
    ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = IPC_MSG_TYPE_HEARTBEAT;
    msg.src_module_id = ctx->module_id;
    msg.dst_module_id = conn->remote_module_id;
    msg.request_id = 0;
    msg.payload = NULL;
    msg.payload_len = 0;
    msg.free_fn = NULL;

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (ipc_frame_serialize(&msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int ret = ipc_connection_send(conn, buf, buf_len);
    g_free(buf);

    if (ret == ERRCODE_SUCCESS)
    {
        conn->last_heartbeat_sent = time(NULL);
    }

    return ret;
}

// ============================================================================
// 帧处理
// ============================================================================

static void handle_frame(ipc_context_t *ctx, ipc_connection_t *conn, ipc_message_t *header, const uint8_t *payload)
{
    switch (header->msg_type)
    {
        case IPC_MSG_TYPE_HANDSHAKE:
        {
            /* 收到握手：提取对端模块 ID */
            if (header->payload_len >= sizeof(uint32_t) && payload)
            {
                uint32_t remote_id_be;
                memcpy(&remote_id_be, payload, sizeof(uint32_t));
                uint32_t remote_id = ntohl(remote_id_be);
                conn->remote_module_id = remote_id;
                char _buf[16];
                LOG_INFO("<%s> 收到 %s 的握手", ctx->name, fmt_module_id(remote_id, _buf, sizeof(_buf)));
            }

            /* 发送握手响应 */
            uint32_t id_be = htonl(ctx->module_id);
            ipc_message_t ack;
            memset(&ack, 0, sizeof(ack));
            ack.msg_type = IPC_MSG_TYPE_HANDSHAKE_ACK;
            ack.src_module_id = ctx->module_id;
            ack.dst_module_id = conn->remote_module_id;
            ack.payload = &id_be;
            ack.payload_len = sizeof(uint32_t);

            uint8_t *buf = NULL;
            uint32_t buf_len = 0;
            if (ipc_frame_serialize(&ack, &buf, &buf_len) == ERRCODE_SUCCESS)
            {
                pthread_mutex_lock(&ctx->comutex);
                ipc_connection_send(conn, buf, buf_len);
                pthread_mutex_unlock(&ctx->comutex);
                g_free(buf);
            }

            conn->state = IPC_COCONNECTED;
            conn->last_heartbeat_recv = time(NULL);
            ipc_connection_reset_reconnect(conn);
            {
                char _buf[16];
                LOG_INFO("<%s> 与 %s 连接建立", ctx->name, fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
            }
            break;
        }

        case IPC_MSG_TYPE_HANDSHAKE_ACK:
        {
            if (header->payload_len >= sizeof(uint32_t) && payload)
            {
                uint32_t remote_id_be;
                memcpy(&remote_id_be, payload, sizeof(uint32_t));
                conn->remote_module_id = ntohl(remote_id_be);
            }
            conn->state = IPC_COCONNECTED;
            conn->last_heartbeat_recv = time(NULL);
            ipc_connection_reset_reconnect(conn);
            {
                char _buf[16];
                LOG_INFO("<%s> 与 %s 握手完成", ctx->name, fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
            }
            break;
        }

        case IPC_MSG_TYPE_HEARTBEAT:
        {
            conn->last_heartbeat_recv = time(NULL);
            /* 发送心跳响应 */
            ipc_message_t ack;
            memset(&ack, 0, sizeof(ack));
            ack.msg_type = IPC_MSG_TYPE_HEARTBEAT_ACK;
            ack.src_module_id = ctx->module_id;
            ack.dst_module_id = conn->remote_module_id;

            uint8_t *buf = NULL;
            uint32_t buf_len = 0;
            if (ipc_frame_serialize(&ack, &buf, &buf_len) == ERRCODE_SUCCESS)
            {
                pthread_mutex_lock(&ctx->comutex);
                ipc_connection_send(conn, buf, buf_len);
                pthread_mutex_unlock(&ctx->comutex);
                g_free(buf);
            }
            break;
        }

        case IPC_MSG_TYPE_HEARTBEAT_ACK:
        {
            conn->last_heartbeat_recv = time(NULL);
            break;
        }

        case IPC_MSG_TYPE_SHUTDOWN:
        {
            LOG_INFO("<%s> 收到关闭通知", ctx->name);
            ctx->shutdown_requested = 1;
            break;
        }

        default:
        {
            /* 应用消息 */
            ipc_message_t *app_msg = ipc_frame_to_message(header, payload);
            if (!app_msg)
            {
                break;
            }

            /* 检查是否是对同步查询的响应 */
            if (app_msg->request_id != 0 && ctx->query_mgr)
            {
                int completed = ipc_query_mgr_complete(ctx->query_mgr, app_msg->request_id, app_msg);
                if (completed == ERRCODE_SUCCESS)
                {
                    break; /* 已交付给等待者，不调用 msg_handler */
                }
            }

            /* 推入 worker 队列，IO 线程立即返回继续 epoll */
            g_async_queue_push(ctx->msg_queue, app_msg);
            break;
        }
    }
}

static void process_received_data(ipc_context_t *ctx, ipc_connection_t *conn)
{
    while (conn->recv_len >= IPC_FRAME_HEADER_SIZE)
    {
        /* 尝试解析帧头部 */
        ipc_message_t header;
        if (ipc_frame_parse_header(conn->recv_buf, &header) != ERRCODE_SUCCESS)
        {
            /* 无效帧，断开连接 */
            LOG_WARN("<%s> 收到无效帧，断开连接", ctx->name);
            ipc_connection_close(conn);
            return;
        }

        /* 检查是否有完整帧 */
        uint32_t frame_total = IPC_FRAME_HEADER_SIZE + header.payload_len;
        if (conn->recv_len < frame_total)
        {
            break; /* 等待更多数据 */
        }

        /* 提取负载 */
        const uint8_t *payload = (header.payload_len > 0) ? (conn->recv_buf + IPC_FRAME_HEADER_SIZE) : NULL;

        /* 处理帧 */
        handle_frame(ctx, conn, &header, payload);

        /* 移除已处理数据 */
        uint32_t remaining = conn->recv_len - frame_total;
        if (remaining > 0)
        {
            memmove(conn->recv_buf, conn->recv_buf + frame_total, remaining);
        }
        conn->recv_len = remaining;
    }
}

// ============================================================================
// 心跳检查和重连
// ============================================================================

static void check_heartbeats(ipc_context_t *ctx)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        ipc_connection_t *conn = ctx->connections[i];
        if (!conn || conn->state != IPC_COCONNECTED)
        {
            continue;
        }

        /* 发送心跳 */
        if (now - conn->last_heartbeat_sent >= IPC_HEARTBEAT_INTERVAL)
        {
            send_heartbeat(ctx, conn);
        }

        /* 检查心跳超时 */
        if (now - conn->last_heartbeat_recv > IPC_HEARTBEAT_TIMEOUT)
        {
            {
                char _buf[16];
                LOG_WARN("<%s> 心跳超时，断开 %s", ctx->name,
                         fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
            }
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
            ipc_connection_close(conn);
            if (conn->is_initiator)
            {
                ipc_connection_backoff_reconnect(conn);
            }
        }
    }
    pthread_mutex_unlock(&ctx->comutex);
}

static void attempt_reconnects(ipc_context_t *ctx)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        ipc_connection_t *conn = ctx->connections[i];
        if (!conn || !conn->is_initiator)
        {
            continue;
        }
        if (conn->state != IPC_CODISCONNECTED)
        {
            continue;
        }
        if (now < conn->next_reconnect_time)
        {
            continue;
        }

        LOG_INFO("<%s> 重连 module(0x%08X) (%s:%u)...", ctx->name, conn->remote_module_id, conn->remote_host,
                 conn->remote_port);

        if (ipc_connection_initiate(conn, conn->remote_host, conn->remote_port) == ERRCODE_SUCCESS)
        {
            /* 添加到 epoll */
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLOUT;
            ev.data.fd = conn->fd;
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, conn->fd, &ev);
        }
        else
        {
            ipc_connection_backoff_reconnect(conn);
        }
    }
    pthread_mutex_unlock(&ctx->comutex);
}

// ============================================================================
// 接受新连接
// ============================================================================

static void accept_new_connection(ipc_context_t *ctx)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(ctx->listen_fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0)
    {
        return;
    }

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    /* 创建连接对象（被接受方，module_id 在握手后确定） */
    pthread_mutex_lock(&ctx->comutex);
    if (ctx->num_connections >= IPC_MAX_CONNECTIONS)
    {
        pthread_mutex_unlock(&ctx->comutex);
        close(fd);
        return;
    }

    ipc_connection_t *conn = ipc_connection_create(0, 0);
    conn->fd = fd;
    conn->state = IPC_COHANDSHAKING;
    conn->last_heartbeat_recv = time(NULL);

    ctx->connections[ctx->num_connections++] = conn;
    pthread_mutex_unlock(&ctx->comutex);

    /* 添加到 epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    LOG_DEBUG("<%s> 接受新连接 (fd=%d)", ctx->name, fd);
}

// ============================================================================
// Worker 线程：执行业务 msg_handler，可安全调用 ipc_query（嵌套 RPC）
// ============================================================================

static void *ipc_worker_thread(void *arg)
{
    ipc_context_t *ctx = (ipc_context_t *)arg;

    LOG_INFO("<%s> Worker 线程启动", ctx->name);

    while (1)
    {
        /* 阻塞等待业务消息；NULL 为退出哨兵 */
        ipc_message_t *msg = g_async_queue_pop(ctx->msg_queue);
        if (!msg)
        {
            break;
        }

        if (ctx->msg_handler)
        {
            ctx->msg_handler(ctx, msg);
        }
        else
        {
            ipc_message_free(msg);
        }
    }

    LOG_INFO("<%s> Worker 线程退出", ctx->name);
    return NULL;
}

// ============================================================================
// IO 线程
// ============================================================================

static void *ipc_io_thread(void *arg)
{
    ipc_context_t *ctx = (ipc_context_t *)arg;
    struct epoll_event events[IPC_MAX_EPOLL_EVENTS];

    LOG_INFO("<%s> IO 线程启动", ctx->name);

    while (ctx->running)
    {
        int nfds = epoll_wait(ctx->epoll_fd, events, IPC_MAX_EPOLL_EVENTS, 1000);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            if (fd == ctx->listen_fd)
            {
                accept_new_connection(ctx);
                continue;
            }

            pthread_mutex_lock(&ctx->comutex);
            ipc_connection_t *conn = find_connection_by_fd(ctx, fd);
            pthread_mutex_unlock(&ctx->comutex);

            if (!conn)
            {
                continue;
            }

            /* 处理可写（连接建立完成） */
            if (events[i].events & EPOLLOUT)
            {
                if (conn->state == IPC_COCONNECTING)
                {
                    /* 检查连接是否成功 */
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);

                    if (err == 0)
                    {
                        /* 连接成功，发送握手 */
                        struct epoll_event ev;
                        ev.events = EPOLLIN;
                        ev.data.fd = fd;
                        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, fd, &ev);

                        pthread_mutex_lock(&ctx->comutex);
                        send_handshake(ctx, conn);
                        pthread_mutex_unlock(&ctx->comutex);
                    }
                    else
                    {
                        /* 连接失败 */
                        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        ipc_connection_close(conn);
                        ipc_connection_backoff_reconnect(conn);
                    }
                }
            }

            /* 处理可读 */
            if (events[i].events & EPOLLIN)
            {
                ssize_t n = read(fd, conn->recv_buf + conn->recv_len, IPC_RECV_BUF_SIZE - conn->recv_len);

                if (n <= 0)
                {
                    if (n == 0 || (errno != EAGAIN && errno != EINTR))
                    {
                        {
                            char _buf[16];
                            LOG_WARN("<%s> 连接断开 (module=%s)", ctx->name,
                                     fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
                        }
                        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        ipc_connection_close(conn);
                        if (conn->is_initiator)
                        {
                            ipc_connection_backoff_reconnect(conn);
                        }
                    }
                    continue;
                }

                conn->recv_len += (uint32_t)n;
                process_received_data(ctx, conn);
            }

            /* 处理错误 */
            if (events[i].events & (EPOLLERR | EPOLLHUP))
            {
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                ipc_connection_close(conn);
                if (conn->is_initiator)
                {
                    ipc_connection_backoff_reconnect(conn);
                }
            }
        }

        /* 定时任务 */
        check_heartbeats(ctx);
        attempt_reconnects(ctx);
    }

    LOG_INFO("<%s> IO 线程退出", ctx->name);
    return NULL;
}

// ============================================================================
// 创建监听 socket
// ============================================================================

static int create_listen_socket(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0)
    {
        close(fd);
        return -1;
    }

    /* 设置非阻塞 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

// ============================================================================
// 公共 API 实现
// ============================================================================

ipc_context_t *ipc_init(uint32_t module_id, const char *name, uint16_t listen_port, ipc_msg_handler_fn msg_handler)
{
    ipc_context_t *ctx = g_malloc0(sizeof(ipc_context_t));
    if (!ctx)
    {
        return NULL;
    }

    /* 设置全局上下文（若尚未设置） */
    if (!g_ipc_context)
    {
        g_ipc_context = ctx;
    }

    ctx->module_id = module_id;
    snprintf(ctx->name, sizeof(ctx->name), "%s", name ? name : "unknown");
    ctx->msg_handler = msg_handler;
    ctx->listen_fd = -1;
    ctx->epoll_fd = -1;
    ctx->running = 0;
    ctx->shutdown_requested = 0;
    ctx->num_connections = 0;
    pthread_mutex_init(&ctx->comutex, NULL);

    /* 创建查询管理器 */
    ctx->query_mgr = ipc_query_mgr_create();

    /* 创建 epoll */
    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0)
    {
        ipc_query_mgr_destroy(ctx->query_mgr);
        g_free(ctx);
        return NULL;
    }

    /* 绑定本模块 IPC 监听端口（由调用方传入） */
    if (listen_port > 0)
    {
        ctx->listen_fd = create_listen_socket("127.0.0.1", listen_port);
        if (ctx->listen_fd >= 0)
        {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = ctx->listen_fd;
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev);
            LOG_INFO("<%s> IPC 监听端口 %u", ctx->name, listen_port);
        }
        else
        {
            LOG_ERROR("<%s> 无法绑定 IPC 端口 %u", ctx->name, listen_port);
        }
    }

    /* 创建业务消息队列 */
    ctx->msg_queue = g_async_queue_new();

    /* 启动 IO 线程 */
    ctx->running = 1;
    if (pthread_create(&ctx->io_thread, NULL, ipc_io_thread, ctx) != 0)
    {
        LOG_PERROR("pthread_create (io)");
        ctx->running = 0;
        ipc_destroy(ctx);
        return NULL;
    }

    /* 启动 Worker 线程 */
    if (pthread_create(&ctx->worker_thread, NULL, ipc_worker_thread, ctx) != 0)
    {
        LOG_PERROR("pthread_create (worker)");
        ctx->running = 0;
        ipc_destroy(ctx);
        return NULL;
    }

    LOG_INFO("<%s> IPC 初始化完成 (module_id=0x%08X)", ctx->name, module_id);
    return ctx;
}

void ipc_destroy(ipc_context_t *ctx)
{
    if (!ctx)
    {
        return;
    }

    ctx->running = 0;

    if (ctx->io_thread != 0)
    {
        pthread_join(ctx->io_thread, NULL);
    }

    /* 发送 NULL 哨兵唤醒 worker 线程使其退出 */
    if (ctx->msg_queue)
    {
        g_async_queue_push(ctx->msg_queue, NULL);
    }
    if (ctx->worker_thread != 0)
    {
        pthread_join(ctx->worker_thread, NULL);
    }
    if (ctx->msg_queue)
    {
        g_async_queue_unref(ctx->msg_queue);
        ctx->msg_queue = NULL;
    }

    /* 关闭所有连接 */
    for (int i = 0; i < ctx->num_connections; i++)
    {
        ipc_connection_destroy(ctx->connections[i]);
    }

    if (ctx->listen_fd >= 0)
    {
        close(ctx->listen_fd);
    }

    if (ctx->epoll_fd >= 0)
    {
        close(ctx->epoll_fd);
    }

    if (ctx->query_mgr)
    {
        ipc_query_mgr_destroy(ctx->query_mgr);
    }

    g_free(ctx->module_table);

    pthread_mutex_destroy(&ctx->comutex);
    g_free(ctx);
}

int ipc_connect(ipc_context_t *ctx, uint32_t target_module_id, const char *host, uint16_t port)
{
    if (!ctx || !host || port == 0)
    {
        return ERRCODE_FAIL;
    }

    pthread_mutex_lock(&ctx->comutex);

    /* 检查是否已有连接 */
    if (find_connection(ctx, target_module_id))
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_SUCCESS;
    }

    if (ctx->num_connections >= IPC_MAX_CONNECTIONS)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    ipc_connection_t *conn = ipc_connection_create(target_module_id, 1);
    /* 存储目标地址，供断连后重连使用 */
    snprintf(conn->remote_host, sizeof(conn->remote_host), "%s", host);
    conn->remote_port = port;
    ctx->connections[ctx->num_connections++] = conn;
    pthread_mutex_unlock(&ctx->comutex);

    LOG_INFO("<%s> 连接 module(0x%08X) (%s:%u)...", ctx->name, target_module_id, host, port);

    if (ipc_connection_initiate(conn, host, port) == ERRCODE_SUCCESS)
    {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = conn->fd;
        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, conn->fd, &ev);
        return ERRCODE_SUCCESS;
    }
    else
    {
        /* 连接失败，稍后重试 */
        ipc_connection_backoff_reconnect(conn);
        return ERRCODE_SUCCESS; /* 不报错，IO 线程会重试 */
    }
}

int ipc_send(ipc_context_t *ctx, uint32_t target_module_id, ipc_message_t *msg)
{
    if (!ctx || !msg)
    {
        return ERRCODE_FAIL;
    }

    msg->src_module_id = ctx->module_id;
    msg->dst_module_id = target_module_id;

    pthread_mutex_lock(&ctx->comutex);
    ipc_connection_t *conn = find_connection(ctx, target_module_id);
    if (!conn || conn->state != IPC_COCONNECTED)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (ipc_frame_serialize(msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    int ret = ipc_connection_send(conn, buf, buf_len);
    pthread_mutex_unlock(&ctx->comutex);

    g_free(buf);
    return ret;
}

ipc_message_t *ipc_query(ipc_context_t *ctx, uint32_t target_module_id, ipc_message_t *msg, uint32_t timeout_ms)
{
    if (!ctx || !msg)
    {
        return NULL;
    }

    if (timeout_ms == 0)
    {
        timeout_ms = IPC_QUERY_TIMEOUT_DEFAULT;
    }

    /* 分配请求 ID */
    uint32_t request_id = ipc_query_mgr_register(ctx->query_mgr);
    msg->request_id = request_id;
    msg->src_module_id = ctx->module_id;

    /* 发送消息 */
    if (ipc_send(ctx, target_module_id, msg) != ERRCODE_SUCCESS)
    {
        return NULL;
    }

    /* 等待响应 */
    return ipc_query_mgr_wait(ctx->query_mgr, request_id, timeout_ms);
}

int ipc_send_response(ipc_context_t *ctx, ipc_message_t *msg)
{
    if (!ctx || !msg)
    {
        return ERRCODE_FAIL;
    }

    /* 根据 dst_module_id 路由到目标模块 */
    uint32_t target_id = msg->dst_module_id;
    msg->src_module_id = ctx->module_id;

    /* 查找连接 */
    pthread_mutex_lock(&ctx->comutex);
    ipc_connection_t *conn = find_connection(ctx, target_id);
    if (!conn || conn->state != IPC_COCONNECTED)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (ipc_frame_serialize(msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    int ret = ipc_connection_send(conn, buf, buf_len);
    pthread_mutex_unlock(&ctx->comutex);

    g_free(buf);
    return ret;
}

int ipc_shutdown_requested(ipc_context_t *ctx)
{
    return ctx ? ctx->shutdown_requested : 1;
}

void ipc_request_shutdown(ipc_context_t *ctx)
{
    if (ctx)
    {
        ctx->shutdown_requested = 1;
    }
}

uint32_t ipc_get_module_id(ipc_context_t *ctx)
{
    return ctx ? ctx->module_id : 0;
}

int ipc_is_connected(ipc_context_t *ctx, uint32_t target_module_id)
{
    if (!ctx)
    {
        return 0;
    }

    pthread_mutex_lock(&ctx->comutex);
    ipc_connection_t *conn = find_connection(ctx, target_module_id);
    int connected = (conn && conn->state == IPC_COCONNECTED) ? 1 : 0;
    pthread_mutex_unlock(&ctx->comutex);

    return connected;
}

void ipc_set_module_table(ipc_context_t *ctx, const ipc_module_table_t *table)
{
    if (!ctx || !table)
    {
        return;
    }

    g_free(ctx->module_table);

    size_t total_size = sizeof(ipc_module_table_t) + table->count * sizeof(ipc_module_info_t);
    ctx->module_table = g_malloc(total_size);
    memcpy(ctx->module_table, table, total_size);
}

const ipc_module_table_t *ipc_get_module_table(ipc_context_t *ctx)
{
    return ctx ? ctx->module_table : NULL;
}
