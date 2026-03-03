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

#include "bgp_cli.h"
#include "bgp_db.h"
#include "bgp_session.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

/** BGP server epoll 单次最大事件数 */
#define BGP_MAX_EPOLL_EVENTS 16

bgp_local_t *g_bgp_local = NULL;

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
// fd_entry 析构
// ============================================================================

/**
 * @brief GHashTable value 析构回调：清理 bgp_conn_t 并释放 bgp_fd_entry_t
 */
static void bgp_fd_entry_free(gpointer data)
{
    bgp_fd_entry_t *entry = (bgp_fd_entry_t *)data;
    if (!entry)
    {
        return;
    }
    /* conn 应已在 bgp_close_fd_entry 中清理，此处作为安全兜底 */
    if (entry->conn)
    {
        bgp_conn_cleanup(entry->conn);
        entry->conn = NULL;
    }
    g_free(entry);
}

// ============================================================================
// BGP server 辅助函数
// ============================================================================

/**
 * @brief 从 epoll 移除 fd，清理内嵌 conn，从 fd_entries 中删除 entry
 * @param fd 要关闭的 fd
 */
static void bgp_close_fd_entry(int fd)
{
    if (!g_bgp_local)
    {
        return;
    }

    /* 先从 epoll 移除，防止产生新事件 */
    epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_DEL, fd, NULL);

    pthread_mutex_lock(&g_bgp_local->fd_mutex);
    bgp_fd_entry_t *entry = g_hash_table_lookup(g_bgp_local->fd_entries, &fd);
    if (!entry)
    {
        pthread_mutex_unlock(&g_bgp_local->fd_mutex);
        return;
    }

    /* 取出 conn 指针，防止 bgp_fd_entry_free 重复清理 */
    bgp_conn_t *conn = entry->conn;
    entry->conn = NULL;
    g_hash_table_remove(g_bgp_local->fd_entries, &fd); /* 触发 bgp_fd_entry_free */
    pthread_mutex_unlock(&g_bgp_local->fd_mutex);

    /* conn 指向 session->pri_conn 或 sec_conn（内嵌），调用 cleanup 关闭 fd */
    if (conn)
    {
        bgp_conn_cleanup(conn);
    }
    else
    {
        close(fd); /* 兜底 */
    }
}

/**
 * @brief 碰撞检测：刚变为 ESTABLISHED 时，检查另一条连接是否已 ESTABLISHED
 *        若另一条已 ESTABLISHED → 本连接为次连接，关闭；否则本连接成为主连接，关闭另一条（若存在）
 * @param fd    刚变为 ESTABLISHED 的 fd
 * @param entry 该 fd 对应的 bgp_fd_entry_t
 */
static void bgp_handle_collision(int fd, bgp_fd_entry_t *entry)
{
    bgp_session_t *sess = entry->session;
    bgp_conn_t *this_conn = entry->conn;

    /* 根据 is_active 确定另一条连接 */
    bgp_conn_t *other_conn = this_conn->is_active ? &sess->sec_conn : &sess->pri_conn;

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    /* 另一条连接已 ESTABLISHED → 本连接为次连接，关闭 */
    if (other_conn->fd >= 0 && !other_conn->is_connecting && other_conn->state == BGP_CONN_STATE_ESTABLISHED)
    {
        LOG_INFO("BGP: 碰撞检测：fd=%d 为次连接，关闭 (neighbor=%s)", fd, addr_str);
        bgp_close_fd_entry(fd); /* entry 此后无效，this_conn->fd 已 -1 */
        return;
    }

    /* 本连接先 ESTABLISHED → 成为主连接 */
    LOG_INFO("BGP: 碰撞检测：fd=%d 成为主连接 (neighbor=%s)", fd, addr_str);

    /* 关闭另一条连接（若存在且活跃） */
    if (other_conn->fd >= 0)
    {
        int other_fd = other_conn->fd;
        LOG_INFO("BGP: 碰撞检测：关闭次连接 fd=%d (neighbor=%s)", other_fd, addr_str);
        bgp_close_fd_entry(other_fd); /* other_conn->fd 已在 cleanup 中置 -1 */
    }
}

// ============================================================================
// BGP server 线程 — 事件处理函数
// ============================================================================

/**
 * @brief 处理全局 listener 上的被动入站连接
 */
static void bgp_handle_passive_accept(void)
{
    bgp_protocol_t *proto = g_bgp_local->protocol;

    struct sockaddr_storage peer_sa;
    socklen_t addr_len = sizeof(peer_sa);
    int conn_fd = accept(g_bgp_local->listener->fd, (struct sockaddr *)&peer_sa, &addr_len);

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

    /* 查找对应 session */
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

    if (sess->sec_conn.fd >= 0)
    {
        LOG_WARN("BGP: 拒绝来自 %s 的重复被动连接（已有 sec_conn.fd=%d）", from_ip, sess->sec_conn.fd);
        close(conn_fd);
        return;
    }

    LOG_INFO("BGP: neighbor %s 被动 TCP 连接（fd=%d）", from_ip, conn_fd);

    /* 直接初始化 sec_conn（内嵌） */
    bgp_conn_init(&sess->sec_conn);
    sess->sec_conn.fd = conn_fd;
    sess->sec_conn.is_active = FALSE;
    sess->sec_conn.is_connecting = FALSE;
    memcpy(&sess->sec_conn.peer_addr, &from_addr, sizeof(from_addr));
    sess->sec_conn.state = BGP_CONN_STATE_OPEN_SENT;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = conn_fd;
    if (epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) < 0)
    {
        LOG_PERROR("BGP: epoll_ctl ADD 被动连接失败");
        bgp_conn_cleanup(&sess->sec_conn);
        return;
    }

    bgp_fd_entry_t *fde = g_malloc0(sizeof(bgp_fd_entry_t));
    fde->session = sess;
    fde->conn = &sess->sec_conn;

    pthread_mutex_lock(&g_bgp_local->fd_mutex);
    int *fd_key = g_malloc(sizeof(int));
    *fd_key = conn_fd;
    g_hash_table_insert(g_bgp_local->fd_entries, fd_key, fde);
    pthread_mutex_unlock(&g_bgp_local->fd_mutex);

    bgp_conn_send_open(&sess->sec_conn, proto->as_number, proto->router_id, sess->peers);
}

/**
 * @brief 处理主动连接完成事件（EPOLLOUT）
 * @param fd    主动连接的 socket fd
 * @param entry 对应的 fd entry
 */
static void bgp_handle_active_connect(int fd, bgp_fd_entry_t *entry)
{
    bgp_session_t *sess = entry->session;
    bgp_conn_t *conn = entry->conn; /* 指向 sess->pri_conn */
    bgp_protocol_t *proto = g_bgp_local->protocol;

    /* 检查 connect 结果 */
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);

    char addr_str[64];
    net_addr_to_str(&sess->neighbor_addr, addr_str, sizeof(addr_str));

    if (err != 0)
    {
        LOG_WARN("BGP: 主动连接到 %s 失败: %s (fd=%d)", addr_str, strerror(err), fd);
        bgp_close_fd_entry(fd); /* entry 此后无效，conn->fd 已 -1 */
        return;
    }

    LOG_INFO("BGP: 主动连接到 %s TCP 已建立 (fd=%d)", addr_str, fd);

    /* 将 epoll 事件改为 EPOLLIN，接收后续 BGP 数据 */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_MOD, fd, &ev);

    /* 更新 conn 状态 */
    pthread_mutex_lock(&g_bgp_local->fd_mutex);
    bgp_fd_entry_t *e = g_hash_table_lookup(g_bgp_local->fd_entries, &fd);
    if (e && e->conn)
    {
        e->conn->is_connecting = FALSE;
    }
    pthread_mutex_unlock(&g_bgp_local->fd_mutex);

    if (proto)
    {
        bgp_conn_send_open(conn, proto->as_number, proto->router_id, sess->peers);
    }
}

/**
 * @brief 处理已建立连接上的 BGP 数据（EPOLLIN）
 * @param fd    已建立连接的 socket fd
 * @param entry 对应的 fd entry
 */
static void bgp_handle_data(int fd, bgp_fd_entry_t *entry)
{
    bgp_conn_t *conn = entry->conn;
    if (!conn)
    {
        return;
    }

    bgp_conn_state_t prev_state = conn->state;
    int ret = bgp_conn_on_data(conn);

    if (ret < 0)
    {
        /* 连接关闭或出错 */
        char addr_str[64];
        net_addr_to_str(&conn->peer_addr, addr_str, sizeof(addr_str));
        LOG_INFO("BGP: 与 %s 的连接关闭 (fd=%d)", addr_str, fd);

        bgp_close_fd_entry(fd); /* entry 此后无效，conn->fd 已置 -1 */
    }
    else if (prev_state != BGP_CONN_STATE_ESTABLISHED && conn->state == BGP_CONN_STATE_ESTABLISHED)
    {
        /* 刚变为 ESTABLISHED → 碰撞检测 */
        bgp_handle_collision(fd, entry);
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
            int fd = events[i].data.fd;

            /* 判断是否为全局 listener */
            if (g_bgp_local->listener && fd == g_bgp_local->listener->fd)
            {
                bgp_handle_passive_accept();
                continue;
            }

            /* 查找 fd 对应的 entry */
            pthread_mutex_lock(&g_bgp_local->fd_mutex);
            bgp_fd_entry_t *entry = g_hash_table_lookup(g_bgp_local->fd_entries, &fd);
            pthread_mutex_unlock(&g_bgp_local->fd_mutex);

            if (!entry || !entry->conn)
            {
                continue; /* fd 已被移除 */
            }

            if (entry->conn->is_connecting)
            {
                /* 主动连接完成（EPOLLOUT） */
                bgp_handle_active_connect(fd, entry);
            }
            else
            {
                /* BGP 数据处理（EPOLLIN） */
                bgp_handle_data(fd, entry);
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

    /* pri_conn 必须空闲（fd=-1） */
    if (session->pri_conn.fd >= 0)
    {
        return;
    }

    int fd = bgp_conn_start_active(&session->pri_conn, &session->neighbor_addr, g_bgp_local->epoll_fd);
    if (fd < 0)
    {
        char addr_str[64];
        net_addr_to_str(&session->neighbor_addr, addr_str, sizeof(addr_str));
        LOG_WARN("BGP: 为 neighbor %s 发起主动连接失败", addr_str);
        return;
    }

    bgp_fd_entry_t *entry = g_malloc0(sizeof(bgp_fd_entry_t));
    entry->session = session;
    entry->conn = &session->pri_conn; /* 主动连接使用 pri_conn */

    pthread_mutex_lock(&g_bgp_local->fd_mutex);
    int *fd_key = g_malloc(sizeof(int));
    *fd_key = fd;
    g_hash_table_insert(g_bgp_local->fd_entries, fd_key, entry);
    pthread_mutex_unlock(&g_bgp_local->fd_mutex);
}

void bgp_server_stop_session_conns(bgp_session_t *session)
{
    if (!session || !g_bgp_local)
    {
        return;
    }

    /* 停止主动连接（fd_entries 中注册了 pri_conn.fd） */
    if (session->pri_conn.fd >= 0)
    {
        bgp_close_fd_entry(session->pri_conn.fd); /* cleanup 会将 fd 置 -1 */
    }

    /* 关闭被动连接 */
    if (session->sec_conn.fd >= 0)
    {
        bgp_close_fd_entry(session->sec_conn.fd);
    }
}

// ============================================================================
// Phase 1: MODULE_START — 建立 IPC 连接到 CFG
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
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void bgp_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 初始化数据库 + 启动 BGP server
// ============================================================================

static void bgp_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY — 初始化 BGP 数据库并启动 server");

    /* 建立 BGP 数据库表（IF NOT EXISTS，幂等操作） */
    if (bgp_db_init(ctx) != 0)
    {
        LOG_WARN("BGP 数据库初始化失败，继续启动");
    }

    /* 从数据库恢复 BGP 内存状态 */
    g_bgp_local->protocol = bgp_db_restore(ctx);

    /* 创建 BGP server epoll */
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("BGP: 创建 epoll 失败");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_bgp_local->epoll_fd = epoll_fd;

    /* 创建全局 listener（0.0.0.0:179）并加入 epoll */
    g_bgp_local->listener = bgp_listen_create();
    if (g_bgp_local->listener)
    {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = g_bgp_local->listener->fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_bgp_local->listener->fd, &ev) < 0)
        {
            LOG_PERROR("BGP: epoll_ctl ADD listener 失败");
            bgp_listen_destroy(g_bgp_local->listener);
            g_bgp_local->listener = NULL;
        }
    }
    else
    {
        LOG_WARN("BGP: 全局 listener 创建失败，被动连接不可用");
    }

    /* 启动 BGP server 线程 */
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

    /* 若已恢复 session 且有 AF peer，为其启动主动连接 */
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
                if (sess->peers != NULL)
                {
                    bgp_server_start_active_conn(sess);
                }
            }
        }
    }

    LOG_INFO("BGP server 线程已启动，监听 0.0.0.0:179");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void bgp_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("BGP module cleanup");

    /* 通知 server 线程退出，等待它结束 */
    g_bgp_local->running = 0;
    if (g_bgp_local->server_thread)
    {
        pthread_join(g_bgp_local->server_thread, NULL);
        g_bgp_local->server_thread = 0;
    }

    /* 关闭所有 session 的连接（在 epoll/fd_entries 销毁前） */
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

    /* 从 epoll 移除并销毁全局 listener */
    if (g_bgp_local->listener)
    {
        epoll_ctl(g_bgp_local->epoll_fd, EPOLL_CTL_DEL, g_bgp_local->listener->fd, NULL);
        bgp_listen_destroy(g_bgp_local->listener);
        g_bgp_local->listener = NULL;
    }

    /* 关闭 epoll */
    if (g_bgp_local->epoll_fd >= 0)
    {
        close(g_bgp_local->epoll_fd);
        g_bgp_local->epoll_fd = DEV_INVALID_FD;
    }

    /* 销毁协议结构 */
    if (g_bgp_local->protocol)
    {
        bgp_protocol_destroy(g_bgp_local->protocol);
        g_bgp_local->protocol = NULL;
    }

    /* 销毁 fd_entries（析构函数清理残留 entry） */
    if (g_bgp_local->fd_entries)
    {
        g_hash_table_destroy(g_bgp_local->fd_entries);
        g_bgp_local->fd_entries = NULL;
    }
    pthread_mutex_destroy(&g_bgp_local->fd_mutex);

    /* dev_ipc_ctx 由 DEV 管理 */
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
        /* ---- DEV 生命周期消息 ---- */
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

        /* ---- CLI 消息 ---- */
        case CFG_MSG_TYPE_CLI:
            LOG_DEBUG("Received CLI command message");
            bgp_cli_handle_message(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            bgp_cli_handle_continue(msg);
            break;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================
__attribute__((constructor)) static void bgp_so_init(void)
{
    LOG_INFO(".so 加载，自初始化");

    /* 创建 IPC 上下文 */
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_BGP, "bgp", DEV_MODULE_PORT_BGP, bgp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC 初始化失败");
        return;
    }

    /* 初始化本地状态 */
    g_bgp_local = g_malloc0(sizeof(bgp_local_t));
    g_bgp_local->dev_ipc_ctx = ctx;
    g_bgp_local->epoll_fd = DEV_INVALID_FD;
    g_bgp_local->fd_entries = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, bgp_fd_entry_free);
    pthread_mutex_init(&g_bgp_local->fd_mutex, NULL);
}
