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

#include "bgp_bdr.h"
#include "bgp_cli.h"
#include "bgp_conn.h"
#include "bgp_db.h"
#include "bgp_session.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

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
        LOG_PERROR("BGP: 创建 listen socket 失败");
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
        LOG_PERROR("BGP: bind 0.0.0.0:179 失败");
        close(fd);
        return;
    }

    if (listen(fd, 32) < 0)
    {
        LOG_PERROR("BGP: listen 失败");
        close(fd);
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = &bgp_listen_tag;
    if (epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD listen fd 失败");
        close(fd);
        return;
    }

    g_bgp_local->listen_fd = fd;
    LOG_INFO("BGP: 开始监听 0.0.0.0:179 (fd=%d)", fd);
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
    LOG_INFO("BGP: 停止监听 0.0.0.0:179");
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
    LOG_INFO("BGP: 被动连接 fd=%d 提升为 pri_conn (neighbor=%s)", sess->sec_conn->fd, addr_str);
    sess->pri_conn = sess->sec_conn;
    sess->sec_conn = NULL;
}

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
            LOG_PERROR("BGP: accept 失败");
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
        LOG_WARN("BGP: 拒绝未知地址族的连接");
        close(conn_fd);
        return;
    }

    if (!proto)
    {
        LOG_WARN("BGP: 协议未初始化，拒绝来自 %s 的连接", from_ip);
        close(conn_fd);
        return;
    }

    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(proto, BGP_VRF_PUBLIC_ID);
    bgp_session_t *sess = vrf0 ? bgp_vrf_find_session(vrf0, &from_addr) : NULL;
    if (!sess || !sess->peers)
    {
        LOG_WARN("BGP: 拒绝来自 %s 的连接（未配置 AF 邻居）", from_ip);
        close(conn_fd);
        return;
    }

    /* sec_conn 已存在：防御性拒绝（正常不应出现） */
    if (sess->sec_conn)
    {
        LOG_WARN("BGP: 拒绝来自 %s 的连接（sec_conn 已存在 fd=%d）", from_ip, sess->sec_conn->fd);
        close(conn_fd);
        return;
    }

    /* pri_conn 已建立（!is_connecting）→ 连接已在协商中，拒绝新的被动连接 */
    if (sess->pri_conn && !sess->pri_conn->is_connecting)
    {
        LOG_INFO("BGP: neighbor %s pri_conn fd=%d 已建立，拒绝被动连接 fd=%d", from_ip, sess->pri_conn->fd, conn_fd);
        close(conn_fd);
        return;
    }

    LOG_INFO("BGP: neighbor %s 被动 TCP 连接（fd=%d）", from_ip, conn_fd);

    /* 创建被动连接对象 */
    bgp_conn_t *conn = bgp_conn_create(sess);
    conn->fd = conn_fd;
    conn->is_active = FALSE;
    conn->is_connecting = FALSE;
    memcpy(&conn->peer_addr, &from_addr, sizeof(from_addr));
    conn->state = BGP_CONN_STATE_OPEN_SENT;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    if (epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD 被动连接失败");
        bgp_conn_destroy(conn);
        return;
    }

    if (sess->pri_conn)
    {
        /* pri_conn 还在 connecting：被动 TCP 先建立，暂存 sec_conn，等 EPOLLOUT 解决 */
        LOG_INFO("BGP: neighbor %s 被动连接 fd=%d 先建立（主动连接 fd=%d 仍在握手中）", from_ip, conn_fd,
                 sess->pri_conn->fd);
        sess->sec_conn = conn;
    }
    else
    {
        /* 无主动连接，被动连接直接作为 pri_conn */
        sess->pri_conn = conn;
    }

    bgp_conn_send_open(conn, proto->as_number, proto->router_id, sess->peers);
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
        LOG_WARN("BGP: 主动连接到 %s 失败: %s (fd=%d)", addr_str, strerror(err), conn->fd);
        bgp_conn_close(&sess->pri_conn);
        if (sess->sec_conn)
        {
            /* 主动失败，被动连接（已发过 OPEN）顶上 */
            bgp_session_promote_sec(sess);
        }
        return;
    }

    if (sess->sec_conn)
    {
        /* 被动 TCP 先建立（sec_conn 已在协商中）→ 放弃主动连接，使用被动连接 */
        LOG_INFO("BGP: neighbor %s 被动连接 fd=%d 先建立，放弃主动连接 fd=%d", addr_str, sess->sec_conn->fd, conn->fd);
        bgp_conn_close(&sess->pri_conn);
        bgp_session_promote_sec(sess);
        return;
    }

    /* 主动连接胜出：转 EPOLLIN，发送 OPEN */
    LOG_INFO("BGP: 主动连接到 %s TCP 已建立 (fd=%d)", addr_str, conn->fd);
    conn->is_connecting = FALSE;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);

    if (proto)
    {
        bgp_conn_send_open(conn, proto->as_number, proto->router_id, sess->peers);
    }
}

/**
 * @brief 处理已建立连接上的 BGP 数据（EPOLLIN）
 *
 * 碰撞检测已在 TCP 层完成，此处只负责数据处理和连接关闭。
 *
 * @param conn 接收数据的连接结构
 */
static void bgp_handle_data(bgp_conn_t *conn)
{
    bgp_session_t *sess = conn->session;
    bgp_conn_t **slot = (sess->pri_conn == conn) ? &sess->pri_conn : &sess->sec_conn;

    int ret = bgp_conn_on_data(conn);
    if (ret < 0)
    {
        char addr_str[64];
        net_addr_to_str(&conn->peer_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: 与 %s 的连接关闭 (fd=%d)", addr_str, conn->fd);
        bgp_conn_close(slot);
    }
}

// ============================================================================
// BGP server 线程
// ============================================================================

static void *bgp_server_thread(void *arg)
{
    (void)arg;
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
            LOG_PERROR("BGP: epoll_wait 失败");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            void *ptr = events[i].data.ptr;

            if (ptr == (void *)&bgp_listen_tag)
            {
                bgp_handle_passive_accept();
                continue;
            }

            bgp_conn_t *conn = (bgp_conn_t *)ptr;
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
        LOG_WARN("BGP: 为 neighbor %s 发起主动连接失败", addr_str);
        bgp_conn_destroy(conn);
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
    bgp_conn_close(&session->pri_conn);
    bgp_conn_close(&session->sec_conn);
}

// ============================================================================
// Phase 1: MODULE_START
// ============================================================================

static void bgp_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START — 建立 IPC 连接");
    dev_ipc_connect(ctx, DEV_MODULE_ID_CFG, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CFG);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    LOG_INFO("已连接到 CFG 和 DB");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT
// ============================================================================

static void bgp_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY
// ============================================================================

static void bgp_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY — 初始化 BGP 数据库并启动 server");

    if (bgp_db_init(ctx) != 0)
    {
        LOG_WARN("BGP 数据库初始化失败，继续启动");
    }

    g_bgp_local->protocol = bgp_db_restore(ctx);

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("BGP: 创建 epoll 失败");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_bgp_local->epoll_fd = epoll_fd;

    g_bgp_local->running = 1;
    if (pthread_create(&g_bgp_local->server_thread, NULL, bgp_server_thread, NULL) != 0)
    {
        LOG_PERROR("BGP: 创建 server 线程失败");
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
                    if (sess->peers)
                    {
                        bgp_server_start_active_conn(sess);
                    }
                }
            }
        }
    }

    LOG_INFO("BGP server 线程已启动");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void bgp_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("BGP module cleanup");

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
        case CFG_MSG_TYPE_CLI:
            LOG_DEBUG("Received CLI command message");
            bgp_cli_handle_message(msg);
            break;
        case CFG_MSG_TYPE_CLI_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            bgp_cli_handle_continue(msg);
            break;
        case CFG_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            bgp_bdr_show_config(msg);
            return;
        default:
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// .so constructor
// ============================================================================

__attribute__((constructor)) static void bgp_so_init(void)
{
    LOG_INFO(".so 加载，自初始化");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_BGP, "bgp", DEV_MODULE_PORT_BGP, bgp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC 初始化失败");
        return;
    }

    g_bgp_local = g_malloc0(sizeof(bgp_local_t));
    g_bgp_local->dev_ipc_ctx = ctx;
    g_bgp_local->epoll_fd = DEV_INVALID_FD;
    g_bgp_local->listen_fd = -1;
}
