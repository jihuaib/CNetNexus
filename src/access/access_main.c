/**
 * @file   access_main.c
 * @brief  ACCESS 接入层（line 层）模块主入口
 * @author jhb
 * @date   2026/05/30
 *
 * 职责：按 line 模型管理接入通道（telnet→vty，后续 con/tty/ssh），持有终端态；
 *       一整行命令就绪后向 CLI 发 RPC 执行，本地负责回显/行编辑/分页。
 *       本文件负责模块生命周期、IPC 上下文与 telnet epoll 服务线程；
 *       线池与终端逻辑见 access_line.c，传输层见 access_telnet.c。
 */
#include "access_main.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "access.h"
#include "access_console.h"
#include "access_db.h"
#include "access_line.h"
#include "access_telnet.h"
#include "cli.h"
#include "errcode.h"
#include "log.h"

#define ACCESS_MAX_EPOLL_EVENTS 16

/** 监听 socket。ep_kind 必须是首字段，与 access_line_t 一样供 epoll 判别。 */
typedef struct access_listener
{
    access_ep_kind_t ep_kind; /**< 恒为 ACCESS_EP_LISTENER */
    int fd;                   /**< 监听 fd */
    uint16_t line_type;       /**< accept 出的线类型：CON / VTY */
} access_listener_t;

/** ACCESS 模块本地状态 */
typedef struct access_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    volatile int running;
    int epoll_fd;
    access_listener_t telnet_lis;  /**< telnet/vty 监听（TCP 3788） */
    access_listener_t console_lis; /**< console 监听（AF_UNIX，永远在线） */
    char console_path[256];        /**< console unix socket 路径 */
    pthread_t server_thread;
    cli_chunk_stream_t show_stream; /**< show running-config 分片输出状态 */
} access_local_t;

static access_local_t g_access = {0};

dev_ipc_context_t *access_ipc_ctx(void)
{
    return g_access.dev_ipc_ctx;
}

// ============================================================================
// bash line 命令：在本线 fd 与 PTY 之间桥接（line 层职责，CLI 不参与）
// ============================================================================

typedef struct
{
    access_line_t *line;
    int epoll_fd;
} access_bash_ctx_t;

/** 把 client fd 重新挂回 epoll，使 server 线程恢复接管本线 */
static void access_bash_rearm(int epoll_fd, access_line_t *line)
{
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = line;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, line->fd, &ev);
}

/** bash 桥接线程：fork PTY 跑 bash，在 client fd 与 PTY 间双向转发，退出后恢复 CLI */
static void *access_bash_bridge(void *arg)
{
    pthread_setname_np(pthread_self(), "access-bash");
    access_bash_ctx_t *c = (access_bash_ctx_t *)arg;
    access_line_t *line = c->line;
    int client_fd = line->fd;
    int epoll_fd = c->epoll_fd;
    g_free(c);

    int pty_master = -1;
    struct winsize ws = {.ws_row = 24, .ws_col = 80, .ws_xpixel = 0, .ws_ypixel = 0};
    pid_t pid = forkpty(&pty_master, NULL, NULL, &ws);
    if (pid < 0)
    {
        access_line_send(line, "Error: Failed to start bash.\r\n");
        access_line_send_prompt(line);
        access_bash_rearm(epoll_fd, line);
        return NULL;
    }
    if (pid == 0)
    {
        setenv("TERM", "xterm", 1);
        execlp("/bin/bash", "bash", "--login", NULL);
        _exit(1);
    }

    char buf[4096];
    struct pollfd fds[2];
    while (1)
    {
        fds[0].fd = client_fd;
        fds[0].events = POLLIN;
        fds[1].fd = pty_master;
        fds[1].events = POLLIN;

        int r = poll(fds, 2, 500);
        if (r < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        int status;
        if (waitpid(pid, &status, WNOHANG) == pid)
        {
            pid = -1;
            break;
        }

        if (fds[0].revents & POLLIN)
        {
            ssize_t nn = read(client_fd, buf, sizeof(buf));
            if (nn <= 0 || write(pty_master, buf, (size_t)nn) < 0)
            {
                break;
            }
        }
        if (fds[1].revents & POLLIN)
        {
            ssize_t nn = read(pty_master, buf, sizeof(buf));
            if (nn <= 0 || write(client_fd, buf, (size_t)nn) < 0)
            {
                break;
            }
        }
    }

    if (pid > 0)
    {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
    close(pty_master);

    access_line_send(line, "\r\nBash session ended, returning to CLI...\r\n");
    access_line_send_prompt(line);
    access_bash_rearm(epoll_fd, line);
    return NULL;
}

/** 进入 bash：从 epoll 摘除本线 fd，交给分离的桥接线程，避免冻结其它 vty 线 */
static void access_bash_enter(access_line_t *line)
{
    epoll_ctl(g_access.epoll_fd, EPOLL_CTL_DEL, line->fd, NULL);
    access_line_send(line, "\r\nEntering bash shell, type 'exit' to return to CLI.\r\n\r\n");

    access_bash_ctx_t *c = g_malloc(sizeof(*c));
    c->line = line;
    c->epoll_fd = g_access.epoll_fd;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, access_bash_bridge, c) != 0)
    {
        g_free(c);
        access_line_send(line, "Error: Failed to create bash thread.\r\n");
        access_line_send_prompt(line);
        access_bash_rearm(g_access.epoll_fd, line);
    }
    pthread_attr_destroy(&attr);
}

// ============================================================================
// telnet epoll 服务线程
// ============================================================================

/** 线类型名（日志用） */
static const char *line_type_name(uint16_t t)
{
    return t == ACCESS_LINE_TYPE_CON ? "con" : "vty";
}

/** 在某个监听 socket 上 accept 一条新线 */
static void access_accept_on_listener(access_listener_t *lis)
{
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int conn_fd = accept(lis->fd, (struct sockaddr *)&ss, &slen);
    if (conn_fd < 0)
    {
        if (g_access.running)
        {
            LOG_PERROR("Accept failed");
        }
        return;
    }

    int flags = fcntl(conn_fd, F_GETFL, 0);
    fcntl(conn_fd, F_SETFL, flags | O_NONBLOCK);

    char ip[ACCESS_MAX_CLIENT_IP_LEN];
    uint16_t port = 0;
    if (lis->line_type == ACCESS_LINE_TYPE_VTY && ss.ss_family == AF_INET)
    {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        port = ntohs(sin->sin_port);
    }
    else
    {
        g_strlcpy(ip, "console", sizeof(ip));
    }

    access_line_t *line = access_line_alloc(conn_fd, ip, port, lis->line_type);
    if (!line)
    {
        const char *busy = "\r\nAll lines are busy. Try again later.\r\n";
        (void)write(conn_fd, busy, strlen(busy));
        close(conn_fd);
        LOG_WARN("%s line pool exhausted, rejected connection from %s", line_type_name(lis->line_type), ip);
        return;
    }

    struct epoll_event client_ev;
    client_ev.events = EPOLLIN;
    client_ev.data.ptr = line;
    if (epoll_ctl(g_access.epoll_fd, EPOLL_CTL_ADD, conn_fd, &client_ev) < 0)
    {
        LOG_PERROR("Failed to add client to epoll");
        access_line_free(line);
        return;
    }
    LOG_INFO("Line %s%u connected (fd=%d, peer=%s)", line_type_name(line->line_type), line->line_id, conn_fd, ip);
    access_line_greet(line);
}

static void *access_server_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "access-srv");
    log_set_tag(dev_ipc_get_self_name(g_access.dev_ipc_ctx));

    while (g_access.running)
    {
        struct epoll_event events[ACCESS_MAX_EPOLL_EVENTS];
        int nfds = epoll_wait(g_access.epoll_fd, events, ACCESS_MAX_EPOLL_EVENTS, 1000);
        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            /* data.ptr 首字段 ep_kind 区分监听 socket 与客户端线 */
            access_ep_kind_t kind = *(access_ep_kind_t *)events[i].data.ptr;
            if (kind == ACCESS_EP_LISTENER)
            {
                access_accept_on_listener((access_listener_t *)events[i].data.ptr);
                continue;
            }

            access_line_t *line = (access_line_t *)events[i].data.ptr;
            int fd = line->fd;
            if (access_line_process_input(line) < 0)
            {
                LOG_INFO("Line %s%u disconnected (fd=%d)", line_type_name(line->line_type), line->line_id, fd);
                epoll_ctl(g_access.epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                access_line_close_on_cli(line->line_id);
                access_line_free(line);
            }
            else if (line->enter_bash)
            {
                /* bash line 命令：摘除 fd 交桥接线程，本线 PTY 期间不再走 epoll */
                line->enter_bash = 0;
                LOG_INFO("Line %s%u entering bash", line_type_name(line->line_type), line->line_id);
                access_bash_enter(line);
            }
        }
    }

    return NULL;
}

/** 注册一个监听 socket 到 epoll */
static int access_register_listener(access_listener_t *lis, int fd, uint16_t line_type)
{
    lis->ep_kind = ACCESS_EP_LISTENER;
    lis->fd = fd;
    lis->line_type = line_type;
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = lis;
    if (epoll_ctl(g_access.epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        LOG_PERROR("Failed to add listener to epoll");
        return -1;
    }
    return 0;
}

void access_telnet_apply_gating(void)
{
    /* 监听只由全局 telnet server 开关决定；某条线能否接入由 per-line transport input 控制。 */
    int want = access_telnet_server_enabled();
    int have = (g_access.telnet_lis.fd >= 0);

    if (want && !have)
    {
        int tfd = access_telnet_create_listen_sock(ACCESS_TELNET_PORT);
        if (tfd < 0 || access_register_listener(&g_access.telnet_lis, tfd, ACCESS_LINE_TYPE_VTY) != 0)
        {
            if (tfd >= 0)
            {
                close(tfd);
            }
            g_access.telnet_lis.fd = -1;
            LOG_ERROR("ACCESS: failed to start telnet listener on port %d", ACCESS_TELNET_PORT);
            return;
        }
        LOG_INFO("Telnet enabled, listening on port %d", ACCESS_TELNET_PORT);
    }
    else if (!want && have)
    {
        epoll_ctl(g_access.epoll_fd, EPOLL_CTL_DEL, g_access.telnet_lis.fd, NULL);
        close(g_access.telnet_lis.fd);
        g_access.telnet_lis.fd = -1;
        LOG_INFO("Telnet disabled (no vty has transport input telnet)");
    }
}

static int access_start_listeners(void)
{
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("Failed to create epoll");
        return -1;
    }
    g_access.epoll_fd = epoll_fd;

    /* console（串口）通道：unix socket，永远在线——兜底入口，必须成功 */
    g_strlcpy(g_access.console_path, access_console_sock_path(), sizeof(g_access.console_path));
    int cfd = access_console_create_listen_sock(g_access.console_path);
    if (cfd < 0 || access_register_listener(&g_access.console_lis, cfd, ACCESS_LINE_TYPE_CON) != 0)
    {
        LOG_ERROR("ACCESS: console channel start failed (path=%s)", g_access.console_path);
        return -1;
    }
    LOG_INFO("Console channel listening on %s", g_access.console_path);

    /* telnet/vty：默认不监听——出厂只 console 能登录。需在 console 上 `line vty 0 4`
     * + `transport input telnet` 配置后，由 access_telnet_apply_gating 起 23 监听。 */

    g_access.running = 1;
    if (pthread_create(&g_access.server_thread, NULL, access_server_thread, NULL) != 0)
    {
        LOG_PERROR("Failed to create server thread");
        g_access.running = 0;
        return -1;
    }
    return 0;
}

// ============================================================================
// IPC 消息处理
// ============================================================================

static int g_db_restored = 0;

/* DB READY：建表 + 从 DB 恢复配置 + 据此刷新 telnet 监听（重启后保留使能状态）。
 * 只在 IPC worker 线程执行（由 ACCESS_MSG_INTERNAL_DB_READY 触发），单线程串行，无需加锁。
 * 触发来源：init 投递一条（DB 已 connected 后，保证可服务）+ DB 重启事件回调投递。 */
static void access_handle_db_ready(void)
{
    if (g_db_restored)
    {
        return;
    }
    dev_ipc_context_t *ctx = g_access.dev_ipc_ctx;
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ACCESS: DB not connected in time; db restore deferred");
        return;
    }
    if (access_db_init() != 0)
    {
        LOG_WARN("ACCESS: db init failed; db restore deferred");
        return;
    }
    access_db_restore();
    access_telnet_apply_gating(); /* 据恢复的 telnet server 开关起停 23 监听 */
    g_db_restored = 1;
    LOG_INFO("ACCESS: config restored from DB");
}

/* DB 模块事件回调（IO 线程，禁止阻塞）：READY 时投递内部消息到 worker 线程做 DB 恢复 */
static void access_on_db_event_cb(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                                  void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event != DEV_MODULE_EVENT_READY || !g_access.dev_ipc_ctx)
    {
        return;
    }
    dev_ipc_message_t *m = dev_ipc_message_create(ACCESS_MSG_INTERNAL_DB_READY, DEV_MODULE_ID_ACCESS,
                                                  DEV_MODULE_ID_ACCESS, 0, NULL, 0, NULL);
    if (m)
    {
        g_async_queue_push(g_access.dev_ipc_ctx->msg_queue, m);
    }
}

/* show current-configuration：输出 ACCESS 的 line 配置块（telnet server / line vty transport） */
static void access_bdr_show_config(dev_ipc_message_t *msg)
{
    GString *out = g_string_new("");
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) == 0 &&
        scope.mode == CLI_SHOW_SCOPE_MODE_THIS)
    {
        access_db_build_running_config_scoped(out, &scope);
    }
    else
    {
        access_db_build_running_config(out); /* 从 DB 读取（与其它模块 BDR 一致） */
    }
    /* cli_chunk_stream_start 接管 out 所有权（含 NULL/空），按需分片回 RESP/RESP_MORE。 */
    (void)cli_chunk_stream_start(&g_access.show_stream, g_access.dev_ipc_ctx, DEV_MODULE_ID_ACCESS, msg, out);
}

static void access_handle_line_progress(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(access_line_progress_t) + 1)
    {
        return;
    }

    access_line_progress_t *p = (access_line_progress_t *)msg->payload;
    size_t text_len = msg->payload_len - sizeof(access_line_progress_t);
    if (!memchr(p->text, '\0', text_len))
    {
        return;
    }
    access_line_send_to(p->line_id, p->text);
}

void access_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    /* ACCESS→CLI 的命令/会话 RPC 走 query/response，响应由 dev_ipc_query 直接返回，不经此回调。
     * 此处处理：内部异步消息（DB_READY）+ CLI 发来的 show current-configuration 汇聚请求。 */
    switch (msg->msg_type)
    {
        case ACCESS_MSG_INTERNAL_DB_READY:
            access_handle_db_ready();
            break;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            access_bdr_show_config(msg);
            break;
        case CLI_MSG_TYPE_CONTINUE:
            (void)cli_chunk_stream_continue(&g_access.show_stream, ctx, DEV_MODULE_ID_ACCESS, msg);
            break;
        case ACCESS_MSG_LINE_PROGRESS:
            access_handle_line_progress(msg);
            break;
        default:
            break;
    }
    dev_ipc_message_free(msg);
}

// ============================================================================
// 模块生命周期
// ============================================================================

int access_module_init(void)
{
    log_set_tag("access");
    LOG_INFO("Module initialization");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_ACCESS, "access", DEV_MODULE_PORT_ACCESS, access_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }
    g_access.dev_ipc_ctx = ctx;
    g_access.epoll_fd = -1;
    g_access.telnet_lis.fd = -1;
    g_access.console_lis.fd = -1;

    access_line_pool_init();

    /* 等 DEV 控制连接 → 启动 telnet → notify_ready。
     * 与 CLI 的 IPC 连接在首次需要时（P3 发 SESSION_OPEN）按需建立。 */
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ACCESS: timed out waiting for DEV connection");
    }

    /* 主动连接 CLI 命令引擎：每行命令/会话开关都要向 CLI 发 RPC。 */
    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_CLI, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("ACCESS: timed out waiting for CLI connection");
    }

    /* 订阅 DB：READY 后建表 + 恢复配置（telnet server 开关 / per-vty transport）。 */
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, access_on_db_event_cb, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ACCESS: subscribe(DB) failed");
    }

    if (access_start_listeners() != 0)
    {
        LOG_ERROR("ACCESS: listener start failed (console channel unavailable)");
    }

    /* 阻塞直到订阅的 peer（DB）真正 CONNECTED，再投递一条 DB_READY，让 IPC worker 线程
     * 统一做建表/恢复（与其它模块一致：DB 操作都在 worker 单线程，无并发、无需加锁）。
     * subscribe 的 synth 事件也会投递一条，worker 顺序处理、由 g_db_restored 去重。 */
    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);
    if (dev_ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        dev_ipc_message_t *m = dev_ipc_message_create(ACCESS_MSG_INTERNAL_DB_READY, DEV_MODULE_ID_ACCESS,
                                                      DEV_MODULE_ID_ACCESS, 0, NULL, 0, NULL);
        if (m)
        {
            g_async_queue_push(ctx->msg_queue, m);
        }
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("ACCESS: notify_ready to DEV failed");
    }
    LOG_INFO("ACCESS: module ready");

    return 0;
}

void access_module_cleanup(void)
{
    LOG_INFO("ACCESS: module cleanup");

    g_access.running = 0;
    if (g_access.server_thread != 0)
    {
        pthread_join(g_access.server_thread, NULL);
        g_access.server_thread = 0;
    }

    dev_ipc_context_t *ctx = g_access.dev_ipc_ctx;
    if (ctx)
    {
        dev_ipc_pre_exit_notify(ctx, 3000);
        g_access.dev_ipc_ctx = NULL;
        dev_ipc_destroy(ctx);
    }

    if (g_access.telnet_lis.fd >= 0)
    {
        close(g_access.telnet_lis.fd);
        g_access.telnet_lis.fd = -1;
    }
    if (g_access.console_lis.fd >= 0)
    {
        close(g_access.console_lis.fd);
        g_access.console_lis.fd = -1;
    }
    if (g_access.console_path[0] != '\0')
    {
        unlink(g_access.console_path);
    }
    if (g_access.epoll_fd >= 0)
    {
        close(g_access.epoll_fd);
        g_access.epoll_fd = -1;
    }

    access_line_pool_cleanup();
}
