/**
 * @file   dev_ipc_context.c
 * @brief  IPC 上下文实现，包含 IO 线程、连接管理和公共 API
 * @author jhb
 * @date   2026/02/02
 */

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

#include "dev.h"
#include "errcode.h"
#include "log.h"

/** 将模块 ID 格式化为可读字符串（用于日志），结果写入 buf */
static const char *fmt_module_id(uint32_t module_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "0x%08X", module_id);
    return buf;
}

#define DEV_IPC_MAX_EPOLL_EVENTS 32

/* 与 DEV 握手完成时,若 dev_ipc_notify_ready 之前因 DEV 未连而置了延迟标志,
 * 这里把 NOTIFY_READY 帧补发给 DEV。仅在 IO 线程上下文中调用。 */
static void flush_pending_notify_ready(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn)
{
    if (!ctx || !conn || !ctx->pending_notify_ready)
    {
        return;
    }
    if (conn->remote_module_id != DEV_MODULE_ID_DEV)
    {
        return;
    }
    if (conn->state != DEV_IPC_COCONNECTED)
    {
        return;
    }

    dev_ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = DEV_IPC_MSG_TYPE_DEV_NOTIFY_READY;
    msg.src_module_id = ctx->module_id;
    msg.dst_module_id = DEV_MODULE_ID_DEV;

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (dev_ipc_frame_serialize(&msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        return;
    }

    pthread_mutex_lock(&ctx->comutex);
    int rc = dev_ipc_connection_send(conn, buf, buf_len);
    pthread_mutex_unlock(&ctx->comutex);
    g_free(buf);

    if (rc == ERRCODE_SUCCESS)
    {
        ctx->pending_notify_ready = 0;
        LOG_INFO("<%s> Deferred notify_ready flushed to DEV after handshake", ctx->name);
    }
    else
    {
        LOG_WARN("<%s> Deferred notify_ready send failed; will retry on next handshake", ctx->name);
    }
}

// 全局 IPC 上下文实例，用于其他模块方便获取
dev_ipc_context_t *g_ipc_context = NULL;
/* Worker 退出哨兵（GAsyncQueue 不能推送 NULL）。 */
static dev_ipc_message_t g_worker_exit_sentinel;

/* 判定消息类型是否为“响应语义”，用于过滤超时后晚到响应。 */
static int is_response_like_msg_type(uint32_t msg_type)
{
    if (msg_type == DEV_IPC_MSG_TYPE_DEV_MODULE_RESP || msg_type == DEV_IPC_MSG_TYPE_DEV_QUERY_IPC_CONNS_RESP ||
        msg_type == DEV_IPC_MSG_TYPE_DEV_QUERY_SUBS_RESP || msg_type == DEV_IPC_MSG_TYPE_DB_RESP)
    {
        return 1;
    }

    return DEV_IPC_MSG_SUBTYPE(msg_type) == 0x00FF;
}

// ============================================================================
// 内部函数前向声明
// ============================================================================

static dev_ipc_connection_t *find_connection(dev_ipc_context_t *ctx, uint32_t module_id);
static dev_ipc_connection_t *find_connection_by_fd(dev_ipc_context_t *ctx, int fd);
static int send_handshake(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn);
static int send_heartbeat(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn);
static void process_received_data(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn);
static void handle_frame(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn, dev_ipc_message_t *header,
                         const uint8_t *payload);
static void check_heartbeats(dev_ipc_context_t *ctx);
static void check_pending_connects(dev_ipc_context_t *ctx);
static void attempt_reconnects(dev_ipc_context_t *ctx);
static void accept_new_connection(dev_ipc_context_t *ctx);
static int arm_initiator_connection(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn, int epoll_op);

// ============================================================================
// 连接查找
// ============================================================================

static dev_ipc_connection_t *find_connection(dev_ipc_context_t *ctx, uint32_t module_id)
{
    dev_ipc_connection_t *fallback = NULL;
    for (int i = 0; i < ctx->num_connections; i++)
    {
        if (ctx->connections[i] && ctx->connections[i]->remote_module_id == module_id)
        {
            if (ctx->connections[i]->state == DEV_IPC_COCONNECTED)
            {
                return ctx->connections[i]; // 优先返回connected的
            }
            if (!fallback)
            {
                fallback = ctx->connections[i];
            }
        }
    }
    return fallback;
}

static dev_ipc_connection_t *find_connection_by_fd(dev_ipc_context_t *ctx, int fd)
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

static void notify_connection_down(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn)
{
    if (!ctx || !conn || conn->remote_module_id == 0 || conn->state != DEV_IPC_COCONNECTED)
    {
        return;
    }

    /* 立即唤醒所有打到该目标的挂起 query，避免等满 60s 超时 */
    if (ctx->query_mgr)
    {
        dev_ipc_query_mgr_cancel_by_target(ctx->query_mgr, conn->remote_module_id);
    }

    if (ctx->disconnect_handler)
    {
        ctx->disconnect_handler(ctx, conn->remote_module_id, ctx->disconnect_user);
    }
}

/* IO 线程关闭并回收连接。
 * 主动方：close + backoff_reconnect，保留 slot 让 attempt_reconnects 复用。
 * 被动方：从 ctx->connections[] 摘除并 destroy，避免 slot 永久泄漏。
 * lock_held=1 表示调用者已持有 ctx->comutex；为 0 时本函数自行加解锁。
 * 返回后被动方 conn 指针失效，调用者不得再访问。
 * 对被动方返回 1（slot 已摘除），调用者若在数组迭代中需 i-- 重检当前位。 */
static int io_close_connection(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn, int lock_held)
{
    if (!conn)
    {
        return 0;
    }

    if (conn->is_initiator)
    {
        dev_ipc_connection_close(conn);
        dev_ipc_connection_backoff_reconnect(conn);
        return 0;
    }

    if (!lock_held)
    {
        pthread_mutex_lock(&ctx->comutex);
    }

    for (int i = 0; i < ctx->num_connections; i++)
    {
        if (ctx->connections[i] == conn)
        {
            ctx->connections[i] = ctx->connections[ctx->num_connections - 1];
            ctx->connections[ctx->num_connections - 1] = NULL;
            ctx->num_connections--;
            break;
        }
    }

    if (!lock_held)
    {
        pthread_mutex_unlock(&ctx->comutex);
    }

    dev_ipc_connection_destroy(conn);
    return 1;
}

// ============================================================================
// 连接状态序列化（供 QUERY_IPC_CONNS 响应和自查询使用）
// ============================================================================

#define IPC_QCONNS_ENTRY_SIZE 100

uint8_t *dev_ipc_build_conns_payload(dev_ipc_context_t *ctx, uint32_t *out_len)
{
    pthread_mutex_lock(&ctx->comutex);

    int n = ctx->num_connections;
    uint32_t pl_len = 4 + (uint32_t)n * IPC_QCONNS_ENTRY_SIZE;
    uint8_t *pl = g_malloc0(pl_len);
    uint8_t *p = pl;

    uint32_t n_be = htonl((uint32_t)n);
    memcpy(p, &n_be, 4);
    p += 4;

    for (int i = 0; i < n; i++)
    {
        dev_ipc_connection_t *c = ctx->connections[i];
        if (!c)
        {
            p += IPC_QCONNS_ENTRY_SIZE;
            continue;
        }

        uint32_t v;
        uint16_t v16;

        v = htonl(c->remote_module_id);
        memcpy(p, &v, 4);
        p += 4;
        v = htonl((uint32_t)c->state);
        memcpy(p, &v, 4);
        p += 4;
        v = htonl((uint32_t)c->is_initiator);
        memcpy(p, &v, 4);
        p += 4;
        strlcpy((char *)p, c->remote_host, 64);
        p += 64;
        v16 = htons(c->remote_port);
        memcpy(p, &v16, 2);
        p += 2;
        p += 2; /* pad */

        uint64_t ts = (uint64_t)c->last_heartbeat_sent;
        v = htonl((uint32_t)(ts >> 32));
        memcpy(p, &v, 4);
        p += 4;
        v = htonl((uint32_t)(ts & 0xFFFFFFFF));
        memcpy(p, &v, 4);
        p += 4;
        ts = (uint64_t)c->last_heartbeat_recv;
        v = htonl((uint32_t)(ts >> 32));
        memcpy(p, &v, 4);
        p += 4;
        v = htonl((uint32_t)(ts & 0xFFFFFFFF));
        memcpy(p, &v, 4);
        p += 4;
        v = htonl(c->reconnect_delay_ms);
        memcpy(p, &v, 4);
        p += 4;
    }

    pthread_mutex_unlock(&ctx->comutex);

    *out_len = pl_len;
    return pl;
}

// ============================================================================
// 握手和心跳
// ============================================================================

static int send_handshake(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn)
{
    /* 握手消息: payload 为本模块 ID (4 字节，网络字节序) */
    uint32_t id_be = htonl(ctx->module_id);
    dev_ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = DEV_IPC_MSG_TYPE_HANDSHAKE;
    msg.src_module_id = ctx->module_id;
    msg.dst_module_id = conn->remote_module_id;
    msg.request_id = 0;
    msg.payload = &id_be;
    msg.payload_len = sizeof(uint32_t);
    msg.free_fn = NULL;

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (dev_ipc_frame_serialize(&msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_connection_send(conn, buf, buf_len);
    g_free(buf);

    if (ret == ERRCODE_SUCCESS)
    {
        conn->state = DEV_IPC_COHANDSHAKING;
    }

    return ret;
}

static int send_heartbeat(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn)
{
    dev_ipc_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_type = DEV_IPC_MSG_TYPE_HEARTBEAT;
    msg.src_module_id = ctx->module_id;
    msg.dst_module_id = conn->remote_module_id;
    msg.request_id = 0;
    msg.payload = NULL;
    msg.payload_len = 0;
    msg.free_fn = NULL;

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (dev_ipc_frame_serialize(&msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_connection_send(conn, buf, buf_len);
    g_free(buf);

    if (ret == ERRCODE_SUCCESS)
    {
        conn->last_heartbeat_sent = time(NULL);
    }

    return ret;
}

static int arm_initiator_connection(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn, int epoll_op)
{
    if (!ctx || !conn || conn->fd < 0)
    {
        return ERRCODE_FAIL;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = conn->fd;

    if (conn->state == DEV_IPC_COCONNECTING)
    {
        ev.events = EPOLLIN | EPOLLOUT;
        if (epoll_ctl(ctx->epoll_fd, epoll_op, conn->fd, &ev) != 0)
        {
            LOG_PERROR("epoll_ctl (arm connect)");
            return ERRCODE_FAIL;
        }
        return ERRCODE_SUCCESS;
    }

    if (conn->state == DEV_IPC_COHANDSHAKING)
    {
        ev.events = EPOLLIN;
        if (epoll_ctl(ctx->epoll_fd, epoll_op, conn->fd, &ev) != 0)
        {
            LOG_PERROR("epoll_ctl (arm handshake)");
            return ERRCODE_FAIL;
        }

        int ret = send_handshake(ctx, conn);
        if (ret != ERRCODE_SUCCESS)
        {
            char _buf[16];
            LOG_WARN("<%s> Failed to send handshake to %s", ctx->name,
                     fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
            return ERRCODE_FAIL;
        }

        return ERRCODE_SUCCESS;
    }

    return ERRCODE_FAIL;
}

// ============================================================================
// 帧处理
// ============================================================================

static void handle_frame(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn, dev_ipc_message_t *header,
                         const uint8_t *payload)
{
    switch (header->msg_type)
    {
        case DEV_IPC_MSG_TYPE_HANDSHAKE:
        {
            /* 收到握手：提取对端模块 ID */
            if (header->payload_len >= sizeof(uint32_t) && payload)
            {
                uint32_t remote_id_be;
                memcpy(&remote_id_be, payload, sizeof(uint32_t));
                uint32_t remote_id = ntohl(remote_id_be);
                conn->remote_module_id = remote_id;
                char _buf[16];
                LOG_INFO("<%s> Received handshake from %s", ctx->name, fmt_module_id(remote_id, _buf, sizeof(_buf)));
            }

            /* 发送握手响应 */
            uint32_t id_be = htonl(ctx->module_id);
            dev_ipc_message_t ack;
            memset(&ack, 0, sizeof(ack));
            ack.msg_type = DEV_IPC_MSG_TYPE_HANDSHAKE_ACK;
            ack.src_module_id = ctx->module_id;
            ack.dst_module_id = conn->remote_module_id;
            ack.payload = &id_be;
            ack.payload_len = sizeof(uint32_t);

            uint8_t *buf = NULL;
            uint32_t buf_len = 0;
            if (dev_ipc_frame_serialize(&ack, &buf, &buf_len) == ERRCODE_SUCCESS)
            {
                pthread_mutex_lock(&ctx->comutex);
                dev_ipc_connection_send(conn, buf, buf_len);
                pthread_mutex_unlock(&ctx->comutex);
                g_free(buf);
            }

            conn->state = DEV_IPC_COCONNECTED;
            conn->last_heartbeat_recv = time(NULL);
            dev_ipc_connection_reset_reconnect(conn);
            {
                char _buf[16];
                LOG_INFO("<%s> Connection established with %s", ctx->name,
                         fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
            }
            flush_pending_notify_ready(ctx, conn);
            break;
        }

        case DEV_IPC_MSG_TYPE_HANDSHAKE_ACK:
        {
            if (header->payload_len >= sizeof(uint32_t) && payload)
            {
                uint32_t remote_id_be;
                memcpy(&remote_id_be, payload, sizeof(uint32_t));
                conn->remote_module_id = ntohl(remote_id_be);
            }
            conn->state = DEV_IPC_COCONNECTED;
            conn->last_heartbeat_recv = time(NULL);
            dev_ipc_connection_reset_reconnect(conn);
            {
                char _buf[16];
                LOG_INFO("<%s> Handshake completed with %s", ctx->name,
                         fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
            }
            flush_pending_notify_ready(ctx, conn);
            break;
        }

        case DEV_IPC_MSG_TYPE_HEARTBEAT:
        {
            conn->last_heartbeat_recv = time(NULL);
            /* 发送心跳响应 */
            dev_ipc_message_t ack;
            memset(&ack, 0, sizeof(ack));
            ack.msg_type = DEV_IPC_MSG_TYPE_HEARTBEAT_ACK;
            ack.src_module_id = ctx->module_id;
            ack.dst_module_id = conn->remote_module_id;

            uint8_t *buf = NULL;
            uint32_t buf_len = 0;
            if (dev_ipc_frame_serialize(&ack, &buf, &buf_len) == ERRCODE_SUCCESS)
            {
                pthread_mutex_lock(&ctx->comutex);
                dev_ipc_connection_send(conn, buf, buf_len);
                pthread_mutex_unlock(&ctx->comutex);
                g_free(buf);
            }
            break;
        }

        case DEV_IPC_MSG_TYPE_HEARTBEAT_ACK:
        {
            conn->last_heartbeat_recv = time(NULL);
            break;
        }

        case DEV_IPC_MSG_TYPE_SHUTDOWN:
        {
            LOG_INFO("<%s> Received shutdown notification", ctx->name);
            ctx->shutdown_requested = 1;
            break;
        }

        case DEV_IPC_MSG_TYPE_DEV_SET_LOG_LEVEL:
        {
            /* 设置日志级别：payload = 4 字节 uint32 网络字节序，IPC 库层透明处理 */
            if (header->payload_len >= sizeof(uint32_t) && payload)
            {
                uint32_t level_be;
                memcpy(&level_be, payload, sizeof(level_be));
                uint32_t level = ntohl(level_be);
                if (level <= LOG_LEVEL_ERROR)
                {
                    log_set_level((log_level_t)level);
                    LOG_INFO("<%s> Log level set to %u via IPC", ctx->name, level);
                }
                else
                {
                    LOG_WARN("<%s> Ignore SET_LOG_LEVEL: invalid level %u", ctx->name, level);
                }
            }
            break;
        }

        case DEV_IPC_MSG_TYPE_DEV_MODULE_EVENT:
        {
            /* MODULE_EVENT 在 IO 线程直接派发：自动建联 + 触发回调（包括 wait_module_ready 的 condvar）
             * 这样即使业务 worker 线程被阻塞，wait 仍能被唤醒。 */
            if (header->payload_len >= sizeof(dev_module_event_payload_t) && payload)
            {
                dev_ipc_dispatch_module_event(ctx, (const dev_module_event_payload_t *)payload);
            }
            break;
        }

        case DEV_IPC_MSG_TYPE_DEV_QUERY_IPC_CONNS:
        {
            /* IPC 状态查询：序列化本模块所有连接并直接回复，无需经过应用层 */
            uint32_t pl_len;
            uint8_t *pl = dev_ipc_build_conns_payload(ctx, &pl_len);

            /* 使用 _RESP 子类型，确保请求方的 query_mgr 能正确路由（不触发递归处理） */
            dev_ipc_message_t resp_hdr;
            memset(&resp_hdr, 0, sizeof(resp_hdr));
            resp_hdr.msg_type = DEV_IPC_MSG_TYPE_DEV_QUERY_IPC_CONNS_RESP;
            resp_hdr.src_module_id = ctx->module_id;
            resp_hdr.dst_module_id = header->src_module_id;
            resp_hdr.request_id = header->request_id;
            resp_hdr.payload = pl;
            resp_hdr.payload_len = pl_len;

            uint8_t *frame_buf = NULL;
            uint32_t frame_len = 0;
            if (dev_ipc_frame_serialize(&resp_hdr, &frame_buf, &frame_len) == ERRCODE_SUCCESS)
            {
                pthread_mutex_lock(&ctx->comutex);
                dev_ipc_connection_send(conn, frame_buf, frame_len);
                pthread_mutex_unlock(&ctx->comutex);
                g_free(frame_buf);
            }
            g_free(pl);
            break;
        }

        case DEV_IPC_MSG_TYPE_DEV_QUERY_SUBS:
        {
            /* sub_mgr 本地视图查询：把本模块订阅了哪些 peer 序列化成文本回去 */
            uint32_t pl_len = 0;
            char *pl = dev_ipc_format_local_subs(ctx, &pl_len);
            if (!pl)
            {
                pl = g_strdup("(no sub_mgr)");
                pl_len = (uint32_t)strlen(pl) + 1;
            }

            dev_ipc_message_t resp_hdr;
            memset(&resp_hdr, 0, sizeof(resp_hdr));
            resp_hdr.msg_type = DEV_IPC_MSG_TYPE_DEV_QUERY_SUBS_RESP;
            resp_hdr.src_module_id = ctx->module_id;
            resp_hdr.dst_module_id = header->src_module_id;
            resp_hdr.request_id = header->request_id;
            resp_hdr.payload = (uint8_t *)pl;
            resp_hdr.payload_len = pl_len;

            uint8_t *frame_buf = NULL;
            uint32_t frame_len = 0;
            if (dev_ipc_frame_serialize(&resp_hdr, &frame_buf, &frame_len) == ERRCODE_SUCCESS)
            {
                pthread_mutex_lock(&ctx->comutex);
                dev_ipc_connection_send(conn, frame_buf, frame_len);
                pthread_mutex_unlock(&ctx->comutex);
                g_free(frame_buf);
            }
            g_free(pl);
            break;
        }

        default:
        {
            /* 应用消息 */
            dev_ipc_message_t *app_msg = dev_ipc_frame_to_message(header, payload);
            if (!app_msg)
            {
                break;
            }

            /* 检查是否是对同步查询的响应 */
            if (app_msg->request_id != 0 && ctx->query_mgr)
            {
                int completed = dev_ipc_query_mgr_complete(ctx->query_mgr, app_msg->request_id, app_msg);
                if (completed == ERRCODE_SUCCESS)
                {
                    break; /* 已交付给等待者，不调用 msg_handler */
                }

                if (is_response_like_msg_type(app_msg->msg_type))
                {
                    LOG_WARN("<%s> Drop unmatched response: src=0x%08X type=0x%08X request_id=%u", ctx->name,
                             app_msg->src_module_id, app_msg->msg_type, app_msg->request_id);
                    dev_ipc_message_free(app_msg);
                    break;
                }
            }

            /* 推入 worker 队列，IO 线程立即返回继续 epoll */
            g_async_queue_push(ctx->msg_queue, app_msg);
            break;
        }
    }
}

static void process_received_data(dev_ipc_context_t *ctx, dev_ipc_connection_t *conn)
{
    while (conn->recv_len >= DEV_IPC_FRAME_HEADER_SIZE)
    {
        /* 尝试解析帧头部 */
        dev_ipc_message_t header;
        if (dev_ipc_frame_parse_header(conn->recv_buf, &header) != ERRCODE_SUCCESS)
        {
            /* 无效帧，断开连接 */
            LOG_WARN("<%s> Received invalid frame, disconnecting", ctx->name);
            notify_connection_down(ctx, conn);
            if (conn->fd >= 0)
            {
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
            }
            io_close_connection(ctx, conn, 0);
            return;
        }

        uint32_t max_payload = DEV_IPC_RECV_BUF_SIZE - DEV_IPC_FRAME_HEADER_SIZE;
        if (header.payload_len > max_payload)
        {
            LOG_WARN("<%s> Received oversized frame payload_len=%u max=%u from module=0x%08X, disconnecting", ctx->name,
                     header.payload_len, max_payload, conn->remote_module_id);
            notify_connection_down(ctx, conn);
            if (conn->fd >= 0)
            {
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
            }
            io_close_connection(ctx, conn, 0);
            return;
        }

        /* 检查是否有完整帧 */
        uint32_t frame_total = DEV_IPC_FRAME_HEADER_SIZE + header.payload_len;
        if (conn->recv_len < frame_total)
        {
            break; /* 等待更多数据 */
        }

        /* 提取负载 */
        const uint8_t *payload = (header.payload_len > 0) ? (conn->recv_buf + DEV_IPC_FRAME_HEADER_SIZE) : NULL;

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

static void check_heartbeats(dev_ipc_context_t *ctx)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_t *conn = ctx->connections[i];
        if (!conn || conn->state != DEV_IPC_COCONNECTED)
        {
            continue;
        }

        /* 发送心跳 */
        if (now - conn->last_heartbeat_sent >= DEV_IPC_HEARTBEAT_INTERVAL)
        {
            send_heartbeat(ctx, conn);
        }

        /* 检查心跳超时 */
        if (now - conn->last_heartbeat_recv > DEV_IPC_HEARTBEAT_TIMEOUT)
        {
            {
                char _buf[16];
                LOG_WARN("<%s> Heartbeat timeout, disconnecting %s", ctx->name,
                         fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
            }
            notify_connection_down(ctx, conn);
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
            if (io_close_connection(ctx, conn, 1))
            {
                /* 被动连接已从数组摘除，swap-with-last 把 last 移到当前位 i，
                 * 退一格让下次 i++ 重新检查该位 */
                i--;
            }
        }
    }
    pthread_mutex_unlock(&ctx->comutex);
}

static void check_pending_connects(dev_ipc_context_t *ctx)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_t *conn = ctx->connections[i];
        if (!conn || !conn->is_initiator)
        {
            continue;
        }
        if (conn->state != DEV_IPC_COCONNECTING && conn->state != DEV_IPC_COHANDSHAKING)
        {
            continue;
        }
        if (now - conn->last_heartbeat_recv <= DEV_IPC_CONNECT_TIMEOUT)
        {
            continue;
        }

        {
            char _buf[16];
            LOG_WARN("<%s> Connection setup timeout, resetting %s (state=%d)", ctx->name,
                     fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)), (int)conn->state);
        }
        if (conn->fd >= 0)
        {
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
        }
        dev_ipc_connection_close(conn);
        dev_ipc_connection_backoff_reconnect(conn);
    }
    pthread_mutex_unlock(&ctx->comutex);
}

static void attempt_reconnects(dev_ipc_context_t *ctx)
{
    time_t now = time(NULL);

    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_t *conn = ctx->connections[i];
        if (!conn || !conn->is_initiator)
        {
            continue;
        }
        if (conn->state != DEV_IPC_CODISCONNECTED)
        {
            continue;
        }
        if (now < conn->next_reconnect_time)
        {
            continue;
        }

        LOG_INFO("<%s> Reconnecting module(0x%08X) (%s:%u)...", ctx->name, conn->remote_module_id, conn->remote_host,
                 conn->remote_port);

        if (dev_ipc_connection_initiate(conn, conn->remote_host, conn->remote_port) == ERRCODE_SUCCESS)
        {
            if (arm_initiator_connection(ctx, conn, EPOLL_CTL_ADD) != ERRCODE_SUCCESS)
            {
                if (conn->fd >= 0)
                {
                    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                }
                dev_ipc_connection_close(conn);
                dev_ipc_connection_backoff_reconnect(conn);
            }
        }
        else
        {
            dev_ipc_connection_backoff_reconnect(conn);
        }
    }
    pthread_mutex_unlock(&ctx->comutex);
}

// ============================================================================
// 接受新连接
// ============================================================================

static void accept_new_connection(dev_ipc_context_t *ctx)
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
    if (ctx->num_connections >= DEV_IPC_MAX_CONNECTIONS)
    {
        int cur = ctx->num_connections;
        pthread_mutex_unlock(&ctx->comutex);
        close(fd);
        LOG_WARN("<%s> Reject accept: connections table full (%d/%d)", ctx->name, cur, DEV_IPC_MAX_CONNECTIONS);
        return;
    }

    dev_ipc_connection_t *conn = dev_ipc_connection_create(0, 0);
    conn->fd = fd;
    conn->state = DEV_IPC_COHANDSHAKING;
    conn->last_heartbeat_recv = time(NULL);

    ctx->connections[ctx->num_connections++] = conn;
    pthread_mutex_unlock(&ctx->comutex);

    /* 添加到 epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    LOG_DEBUG("<%s> Accepted new connection (fd=%d)", ctx->name, fd);
}

// ============================================================================
// Worker 线程：执行业务 msg_handler，可安全调用 dev_ipc_query（嵌套 RPC）
// ============================================================================

static void *dev_ipc_worker_thread(void *arg)
{
    dev_ipc_context_t *ctx = (dev_ipc_context_t *)arg;
    char tname[16];
    snprintf(tname, sizeof(tname), "ipc-wk-%.8s", ctx->name);
    pthread_setname_np(pthread_self(), tname);
    log_set_tag(ctx->name);

    LOG_INFO("<%s> Worker thread started", ctx->name);

    while (1)
    {
        /* 阻塞等待业务消息；使用固定地址作为退出哨兵 */
        dev_ipc_message_t *msg = g_async_queue_pop(ctx->msg_queue);
        if (msg == &g_worker_exit_sentinel)
        {
            break;
        }

        if (ctx->msg_handler)
        {
            ctx->msg_handler(ctx, msg);
        }
        else
        {
            dev_ipc_message_free(msg);
        }
    }

    LOG_INFO("<%s> Worker thread exited", ctx->name);
    return NULL;
}

// ============================================================================
// IO 线程
// ============================================================================

static void *dev_ipc_io_thread(void *arg)
{
    dev_ipc_context_t *ctx = (dev_ipc_context_t *)arg;
    char tname[16];
    snprintf(tname, sizeof(tname), "ipc-io-%.8s", ctx->name);
    pthread_setname_np(pthread_self(), tname);
    log_set_tag(ctx->name);
    struct epoll_event events[DEV_IPC_MAX_EPOLL_EVENTS];

    LOG_INFO("<%s> IO thread started", ctx->name);

    while (ctx->running)
    {
        int nfds = epoll_wait(ctx->epoll_fd, events, DEV_IPC_MAX_EPOLL_EVENTS, 1000);

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
            dev_ipc_connection_t *conn = find_connection_by_fd(ctx, fd);
            pthread_mutex_unlock(&ctx->comutex);

            if (!conn)
            {
                continue;
            }

            /* 处理可写（连接建立完成） */
            if (events[i].events & EPOLLOUT)
            {
                if (conn->state == DEV_IPC_COCONNECTING)
                {
                    /* 检查连接是否成功 */
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);

                    if (err == 0)
                    {
                        if (conn->state != DEV_IPC_COHANDSHAKING)
                        {
                            conn->state = DEV_IPC_COHANDSHAKING;
                        }
                        if (arm_initiator_connection(ctx, conn, EPOLL_CTL_MOD) != ERRCODE_SUCCESS)
                        {
                            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                            dev_ipc_connection_close(conn);
                            dev_ipc_connection_backoff_reconnect(conn);
                        }
                    }
                    else
                    {
                        /* 连接失败 */
                        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        dev_ipc_connection_close(conn);
                        dev_ipc_connection_backoff_reconnect(conn);
                    }
                }
            }

            /* 处理可读 */
            if (events[i].events & EPOLLIN)
            {
                ssize_t n = read(fd, conn->recv_buf + conn->recv_len, DEV_IPC_RECV_BUF_SIZE - conn->recv_len);

                if (n <= 0)
                {
                    if (n == 0 || (errno != EAGAIN && errno != EINTR))
                    {
                        {
                            char _buf[16];
                            LOG_WARN("<%s> Connection lost (module=%s)", ctx->name,
                                     fmt_module_id(conn->remote_module_id, _buf, sizeof(_buf)));
                        }
                        notify_connection_down(ctx, conn);
                        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        io_close_connection(ctx, conn, 0);
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
                notify_connection_down(ctx, conn);
                io_close_connection(ctx, conn, 0);
            }
        }

        /* 定时任务 */
        check_heartbeats(ctx);
        check_pending_connects(ctx);
        attempt_reconnects(ctx);
    }

    LOG_INFO("<%s> IO thread exited", ctx->name);
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
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    /* `dev swap-image` 触发 execv 时,旧进程 close_range 关闭监听 socket 后
     * 内核需要短暂时间(通常 <1s)才把端口完全归还。新进程同 PID 立即 bind
     * 容易撞 EADDRINUSE。这里对 EADDRINUSE 做有限重试(总等待 ≤ 2s),
     * 其它错误立即失败。SO_REUSEADDR 已设但解决不了这种"刚 close 完"的窗口。 */
    int rc;
    int attempts = 0;
    const int max_attempts = 10;
    while ((rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr))) < 0)
    {
        if (errno != EADDRINUSE || ++attempts >= max_attempts)
        {
            break;
        }
        if (attempts == 1)
        {
            LOG_WARN("port %u busy, retrying bind (likely post-exec port drain)", port);
        }
        usleep(200 * 1000);
    }
    if (rc < 0)
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

dev_ipc_context_t *dev_ipc_init(uint32_t module_id, const char *name, uint16_t listen_port,
                                dev_ipc_msg_handler_fn msg_handler)
{
    dev_ipc_context_t *ctx = g_malloc0(sizeof(dev_ipc_context_t));
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
    /* 为调用方线程（constructor/初始化线程）设置日志标签，使初始化期间的日志也能显示模块名 */
    log_set_tag(ctx->name);
    /* 注册模块专属日志文件（$NN_WORK_DIR/log/{name}.log），未设置 NN_WORK_DIR 时无操作 */
    log_register_module_auto(ctx->name);
    ctx->msg_handler = msg_handler;
    ctx->listen_fd = -1;
    ctx->epoll_fd = -1;
    ctx->running = 0;
    ctx->shutdown_requested = 0;
    ctx->num_connections = 0;
    pthread_mutex_init(&ctx->comutex, NULL);

    /* 创建查询管理器 */
    ctx->query_mgr = dev_ipc_query_mgr_create();

    /* 创建订阅管理器 */
    ctx->sub_mgr = dev_ipc_subscribe_mgr_create();

    /* 创建 epoll */
    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0)
    {
        dev_ipc_query_mgr_destroy(ctx->query_mgr);
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
            LOG_INFO("<%s> IPC listening on port %u", ctx->name, listen_port);
        }
        else
        {
            LOG_ERROR("<%s> Failed to bind IPC port %u", ctx->name, listen_port);
        }
    }

    /* 创建业务消息队列 */
    ctx->msg_queue = g_async_queue_new();

    /* 启动 IO 线程 */
    ctx->running = 1;
    if (pthread_create(&ctx->io_thread, NULL, dev_ipc_io_thread, ctx) != 0)
    {
        LOG_PERROR("pthread_create (io)");
        ctx->running = 0;
        dev_ipc_destroy(ctx);
        return NULL;
    }

    /* 启动 Worker 线程 */
    if (pthread_create(&ctx->worker_thread, NULL, dev_ipc_worker_thread, ctx) != 0)
    {
        LOG_PERROR("pthread_create (worker)");
        ctx->running = 0;
        dev_ipc_destroy(ctx);
        return NULL;
    }

    LOG_INFO("<%s> IPC initialization complete (module_id=0x%08X)", ctx->name, module_id);
    return ctx;
}

void dev_ipc_set_disconnect_handler(dev_ipc_context_t *ctx, dev_ipc_disconnect_handler_fn handler, void *user)
{
    if (!ctx)
    {
        return;
    }

    ctx->disconnect_handler = handler;
    ctx->disconnect_user = user;
}

void dev_ipc_destroy(dev_ipc_context_t *ctx)
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

    /* 发送哨兵唤醒 worker 线程使其退出 */
    if (ctx->msg_queue)
    {
        g_async_queue_push(ctx->msg_queue, &g_worker_exit_sentinel);
    }
    if (ctx->worker_thread != 0)
    {
        pthread_join(ctx->worker_thread, NULL);
    }
    if (ctx->msg_queue)
    {
        /* 排空残留的应用消息（IO 线程退出前最后一批推入的消息可能未被 worker 处理） */
        dev_ipc_message_t *residual;
        while ((residual = g_async_queue_try_pop(ctx->msg_queue)) != NULL)
        {
            if (residual != &g_worker_exit_sentinel)
            {
                dev_ipc_message_free(residual);
            }
        }
        g_async_queue_unref(ctx->msg_queue);
        ctx->msg_queue = NULL;
    }

    /* 关闭所有连接 */
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_destroy(ctx->connections[i]);
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
        dev_ipc_query_mgr_destroy(ctx->query_mgr);
    }

    if (ctx->sub_mgr)
    {
        dev_ipc_subscribe_mgr_destroy(ctx->sub_mgr);
        ctx->sub_mgr = NULL;
    }

    pthread_mutex_destroy(&ctx->comutex);
    g_free(ctx);
}

void dev_ipc_clear_connections(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return;
    }

    int restart_io = 0;
    if (ctx->running && ctx->io_thread != 0 && !pthread_equal(pthread_self(), ctx->io_thread))
    {
        /* 先停 IO 线程，避免并发访问 conn 导致 UAF。 */
        ctx->running = 0;
        pthread_join(ctx->io_thread, NULL);
        ctx->io_thread = 0;
        restart_io = 1;
    }

    pthread_mutex_lock(&ctx->comutex);
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_t *conn = ctx->connections[i];
        if (!conn)
        {
            continue;
        }

        if (conn->fd >= 0)
        {
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
        }
        dev_ipc_connection_destroy(conn);
        ctx->connections[i] = NULL;
    }
    ctx->num_connections = 0;
    pthread_mutex_unlock(&ctx->comutex);

    if (restart_io)
    {
        ctx->running = 1;
        if (pthread_create(&ctx->io_thread, NULL, dev_ipc_io_thread, ctx) != 0)
        {
            LOG_PERROR("pthread_create (io restart)");
            ctx->running = 0;
        }
    }
}

void dev_ipc_drop_connection(dev_ipc_context_t *ctx, uint32_t target_module_id)
{
    if (!ctx)
    {
        return;
    }

    int restart_io = 0;
    if (ctx->running && ctx->io_thread != 0 && !pthread_equal(pthread_self(), ctx->io_thread))
    {
        /* 先停 IO 线程，避免并发访问 conn 导致 UAF（同 dev_ipc_clear_connections） */
        ctx->running = 0;
        pthread_join(ctx->io_thread, NULL);
        ctx->io_thread = 0;
        restart_io = 1;
    }

    int dropped = 0;
    pthread_mutex_lock(&ctx->comutex);
    int new_count = 0;
    for (int i = 0; i < ctx->num_connections; i++)
    {
        dev_ipc_connection_t *conn = ctx->connections[i];
        if (!conn)
        {
            continue;
        }
        if (conn->remote_module_id == target_module_id)
        {
            if (conn->fd >= 0)
            {
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
            }
            dev_ipc_connection_destroy(conn);
            dropped++;
        }
        else
        {
            ctx->connections[new_count++] = conn;
        }
    }
    /* 紧凑后清空末尾遗留指针 */
    for (int i = new_count; i < ctx->num_connections; i++)
    {
        ctx->connections[i] = NULL;
    }
    ctx->num_connections = new_count;
    pthread_mutex_unlock(&ctx->comutex);

    if (dropped > 0)
    {
        char _buf[16];
        LOG_INFO("<%s> Dropped %d connection(s) to module %s", ctx->name, dropped,
                 fmt_module_id(target_module_id, _buf, sizeof(_buf)));
    }

    if (restart_io)
    {
        ctx->running = 1;
        if (pthread_create(&ctx->io_thread, NULL, dev_ipc_io_thread, ctx) != 0)
        {
            LOG_PERROR("pthread_create (io restart)");
            ctx->running = 0;
        }
    }
}

int dev_ipc_connect(dev_ipc_context_t *ctx, uint32_t target_module_id, const char *host, uint16_t port)
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

    if (ctx->num_connections >= DEV_IPC_MAX_CONNECTIONS)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    dev_ipc_connection_t *conn = dev_ipc_connection_create(target_module_id, 1);
    /* 存储目标地址，供断连后重连使用 */
    snprintf(conn->remote_host, sizeof(conn->remote_host), "%s", host);
    conn->remote_port = port;
    ctx->connections[ctx->num_connections++] = conn;

    /* 在持锁期间发起连接，使 conn->state 立即进入 COCONNECTING，
     * 避免 IO 线程的 attempt_reconnects() 看到 DISCONNECTED 状态后重复发起连接 */
    LOG_INFO("<%s> Connecting to module(0x%08X) (%s:%u)...", ctx->name, target_module_id, host, port);
    int init_ok = (dev_ipc_connection_initiate(conn, host, port) == ERRCODE_SUCCESS);
    pthread_mutex_unlock(&ctx->comutex);

    if (init_ok)
    {
        if (arm_initiator_connection(ctx, conn, EPOLL_CTL_ADD) != ERRCODE_SUCCESS)
        {
            if (conn->fd >= 0)
            {
                epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
            }
            dev_ipc_connection_close(conn);
            dev_ipc_connection_backoff_reconnect(conn);
        }
        return ERRCODE_SUCCESS;
    }
    else
    {
        /* 连接失败，稍后重试 */
        LOG_WARN("<%s> Initial connection to module(0x%08X) (%s:%u) failed, IO thread will retry", ctx->name,
                 target_module_id, host, port);
        dev_ipc_connection_backoff_reconnect(conn);
        return ERRCODE_SUCCESS; /* 不报错，IO 线程会重试 */
    }
}

int dev_ipc_send(dev_ipc_context_t *ctx, uint32_t target_module_id, dev_ipc_message_t *msg)
{
    if (!ctx || !msg)
    {
        return ERRCODE_FAIL;
    }

    msg->src_module_id = ctx->module_id;
    msg->dst_module_id = target_module_id;

    pthread_mutex_lock(&ctx->comutex);
    dev_ipc_connection_t *conn = find_connection(ctx, target_module_id);
    if (!conn || conn->state != DEV_IPC_COCONNECTED)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (dev_ipc_frame_serialize(msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_connection_send(conn, buf, buf_len);
    pthread_mutex_unlock(&ctx->comutex);

    g_free(buf);
    return ret;
}

dev_ipc_message_t *dev_ipc_query(dev_ipc_context_t *ctx, uint32_t target_module_id, dev_ipc_message_t *msg,
                                 uint32_t timeout_ms)
{
    if (!ctx || !msg || !ctx->query_mgr)
    {
        return NULL;
    }

    if (timeout_ms == 0)
    {
        timeout_ms = DEV_IPC_QUERY_TIMEOUT_DEFAULT;
    }

    /* 分配请求 ID（带目标，便于连接断开时按目标取消） */
    uint32_t request_id = dev_ipc_query_mgr_register(ctx->query_mgr, target_module_id);
    if (request_id == 0)
    {
        return NULL;
    }
    msg->request_id = request_id;
    msg->src_module_id = ctx->module_id;

    /* 发送消息 */
    if (dev_ipc_send(ctx, target_module_id, msg) != ERRCODE_SUCCESS)
    {
        dev_ipc_query_mgr_cancel(ctx->query_mgr, request_id);
        return NULL;
    }

    /* 等待响应 */
    return dev_ipc_query_mgr_wait(ctx->query_mgr, request_id, timeout_ms);
}

int dev_ipc_send_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
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
    dev_ipc_connection_t *conn = find_connection(ctx, target_id);
    if (!conn || conn->state != DEV_IPC_COCONNECTED)
    {
        char _buf[16];
        LOG_WARN("<%s> dev_ipc_send_response: Cannot route to %s (num_connections=%d, %s)", ctx->name,
                 fmt_module_id(target_id, _buf, sizeof(_buf)), ctx->num_connections,
                 conn ? "connection exists but not ready" : "connection does not exist");
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    uint8_t *buf = NULL;
    uint32_t buf_len = 0;
    if (dev_ipc_frame_serialize(msg, &buf, &buf_len) != ERRCODE_SUCCESS)
    {
        pthread_mutex_unlock(&ctx->comutex);
        return ERRCODE_FAIL;
    }

    int ret = dev_ipc_connection_send(conn, buf, buf_len);
    pthread_mutex_unlock(&ctx->comutex);

    g_free(buf);
    return ret;
}

int dev_ipc_shutdown_requested(dev_ipc_context_t *ctx)
{
    return ctx ? ctx->shutdown_requested : 1;
}

void dev_ipc_request_shutdown(dev_ipc_context_t *ctx)
{
    if (ctx)
    {
        ctx->shutdown_requested = 1;
    }
}

uint32_t dev_ipc_get_module_id(dev_ipc_context_t *ctx)
{
    return ctx ? ctx->module_id : 0;
}

int dev_ipc_is_connected(dev_ipc_context_t *ctx, uint32_t target_module_id)
{
    if (!ctx)
    {
        return 0;
    }

    pthread_mutex_lock(&ctx->comutex);
    dev_ipc_connection_t *conn = find_connection(ctx, target_module_id);
    int connected = (conn && conn->state == DEV_IPC_COCONNECTED) ? 1 : 0;
    pthread_mutex_unlock(&ctx->comutex);

    return connected;
}

const char *dev_ipc_get_self_name(dev_ipc_context_t *ctx)
{
    return ctx ? ctx->name : NULL;
}
