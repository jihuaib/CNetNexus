/**
 * @file   cfg_main.c
 * @brief  CFG 模块主入口，三阶段初始化
 * @author jhb
 * @date   2026/01/22
 */
#include "cfg_main.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <glib.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "cli.h"
#include "cli_handler.h"
#include "cli_xml_parser.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "path_utils.h"

enum
{
    CFG_PORT = 3788,
    CFG_BACKLOG = 5
};

#define CFG_MAX_EPOLL_EVENTS 16

cfg_local_t *g_cfg_local = NULL;

// Forward declarations
static void *cfg_server_thread(void *arg);

// Server thread function
static void *cfg_server_thread(void *arg)
{
    (void)arg;
    log_set_tag(ipc_get_self_name(g_cfg_local->ipc_ctx));

    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (g_cfg_local && g_cfg_local->running)
    {
        struct epoll_event events[CFG_MAX_EPOLL_EVENTS];
        // Wait for events with 1 second timeout
        int nfds = epoll_wait(g_cfg_local->epoll_fd, events, CFG_MAX_EPOLL_EVENTS, 1000);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_PERROR("epoll_wait failed");
            break;
        }

        if (nfds == 0)
        {
            continue;
        }

        // Process events
        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd == g_cfg_local->listen_sock)
            {
                // New connection
                client_len = sizeof(client_addr);
                int conn_fd = accept(g_cfg_local->listen_sock, (struct sockaddr *)&client_addr, &client_len);

                if (conn_fd < 0)
                {
                    if (g_cfg_local && g_cfg_local->running)
                    {
                        LOG_PERROR("Accept failed");
                    }
                    continue;
                }

                int *fd_key = g_malloc(sizeof(int));
                *fd_key = conn_fd;

                cli_session_t *session = cli_session_create(conn_fd);
                if (session)
                {
                    g_hash_table_insert(g_cfg_local->sessions, fd_key, session);

                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = conn_fd;
                    if (epoll_ctl(g_cfg_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &client_ev) < 0)
                    {
                        LOG_PERROR("Failed to add client to epoll");
                        g_hash_table_remove(g_cfg_local->sessions, fd_key);
                    }
                    else
                    {
                        LOG_INFO("Client connected (fd: %d)", conn_fd);
                    }
                }
                else
                {
                    g_free(fd_key);
                    close(conn_fd);
                }
            }
            else
            {
                // Input from existing client
                int fd = events[i].data.fd;
                cli_session_t *session = g_hash_table_lookup(g_cfg_local->sessions, &fd);
                if (session)
                {
                    if (cli_process_input(session) < 0)
                    {
                        LOG_INFO("Client disconnected (fd: %d)", fd);
                        epoll_ctl(g_cfg_local->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        g_hash_table_remove(g_cfg_local->sessions, &fd);
                    }
                }
            }
        }
    }

    return NULL;
}

int32_t cfg_create_listen_sock()
{
    int32_t server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        LOG_PERROR("Failed to create socket");
        return DEV_INVALID_FD;
    }

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        close(server_socket);
        LOG_PERROR("Failed to set socket options");
        return DEV_INVALID_FD;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(CFG_PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        close(server_socket);
        LOG_PERROR("Failed to bind socket");
        return DEV_INVALID_FD;
    }

    if (listen(server_socket, CFG_BACKLOG) < 0)
    {
        close(server_socket);
        LOG_PERROR("Failed to listen");
        return DEV_INVALID_FD;
    }

    return server_socket;
}

// ============================================================================
// 三阶段回调辅助函数
// ============================================================================

static void send_phase_response(ipc_context_t *ctx, ipc_message_t *msg, int32_t result)
{
    ipc_message_t *resp = ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_CFG, msg->src_module_id,
                                             msg->request_id, NULL, 0, NULL);
    ipc_send_response(ctx, resp);
    ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// 自动发现并加载 commands.xml
// ============================================================================

/**
 * @brief 扫描指定目录下所有子目录的 resources/commands.xml 并加载到视图树
 * @param base_dir 基础目录路径
 * @return 成功加载的 XML 数量
 */
static int cfg_scan_and_load_xml(const char *base_dir)
{
    DIR *dir = opendir(base_dir);
    if (!dir)
    {
        return 0;
    }

    int loaded = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }

        /* 兼容两种布局：
         *   dev  布局: {base_dir}/{module}/resources/commands.xml  (源码树)
         *   prod 布局: {base_dir}/{module}/commands.xml            (部署包) */
        struct stat st;
        char *xml_path = g_build_filename(base_dir, entry->d_name, "resources", "commands.xml", NULL);
        if (stat(xml_path, &st) != 0)
        {
            g_free(xml_path);
            xml_path = g_build_filename(base_dir, entry->d_name, "commands.xml", NULL);
            if (stat(xml_path, &st) != 0)
            {
                g_free(xml_path);
                continue;
            }
        }

        LOG_INFO("发现 XML: %s", xml_path);
        if (cli_xml_load_view_tree(xml_path, &g_cfg_local->view_tree) == ERRCODE_SUCCESS)
        {
            LOG_INFO("加载 XML 成功: %s", entry->d_name);
            loaded++;
        }
        else
        {
            LOG_ERROR("加载 XML 失败: %s", xml_path);
        }
        g_free(xml_path);
    }

    closedir(dir);
    return loaded;
}

/**
 * @brief 自动发现并加载所有模块的 commands.xml
 *
 * 按以下优先级扫描目录：
 * 1. 环境变量 NN_WORK_DIR（取 resources 子目录）
 * 2. 相对于可执行文件的开发路径
 */
static void cfg_discover_and_load_xml(void)
{
    LOG_INFO("自动发现并加载 commands.xml...");

    int total = 0;

    /* 优先级 1: 环境变量 NN_WORK_DIR（生产环境） */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        char *resources_dir = g_build_filename(work_dir, "resources", NULL);
        total = cfg_scan_and_load_xml(resources_dir);
        g_free(resources_dir);
        if (total > 0)
        {
            LOG_INFO("从 NN_WORK_DIR 加载了 %d 个 XML 配置", total);
            return;
        }
    }

    /* 优先级 2: 相对于可执行文件的开发路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        char *dev_path = g_build_filename(exe_dir, "..", "..", "src", NULL);
        total = cfg_scan_and_load_xml(dev_path);
        g_free(dev_path);
        if (total > 0)
        {
            LOG_INFO("从开发路径加载了 %d 个 XML 配置", total);
            return;
        }
    }

    LOG_ERROR("未发现任何 commands.xml");
}

// ============================================================================
// 本地状态初始化（从 constructor 调用）
// ============================================================================

void cfg_init_local(ipc_context_t *ctx)
{
    g_cfg_local = g_malloc0(sizeof(cfg_local_t));
    pthread_mutex_init(&g_cfg_local->history_mutex, NULL);
    g_cfg_local->running = 0;
    g_cfg_local->epoll_fd = DEV_INVALID_FD;
    g_cfg_local->listen_sock = DEV_INVALID_FD;
    g_cfg_local->worker_thread = 0;
    g_cfg_local->ipc_ctx = ctx;
    g_cfg_local->sessions = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, (GDestroyNotify)cli_session_destroy);

    /* 创建视图树 */
    cli_view_node_t *user_view = cli_view_create(CLI_VIEW_USER, "user", "<NetNexus>");
    if (!user_view)
    {
        LOG_ERROR("Failed to create user view");
        return;
    }
    g_cfg_local->view_tree.root = user_view;

    cli_view_node_t *config_view = cli_view_create(CLI_VIEW_CONFIG, "config", "<NetNexus(config)>");
    if (!config_view)
    {
        LOG_ERROR("Failed to create config view");
        return;
    }
    cli_view_add_child(user_view, config_view);

    /* 自动发现并加载所有模块的 commands.xml */
    cfg_discover_and_load_xml();

    LOG_INFO("本地状态初始化完成");
}

// ============================================================================
// Phase 1: MODULE_START — CFG 不需要连接其他模块，直接回复 OK
// ============================================================================

static void cfg_on_start(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START (无需连接其他模块)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void cfg_on_connect(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 加载所有 XML
// ============================================================================

static void cfg_on_ready(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY — 启动 Telnet 服务器");

    /* 创建 epoll */
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("Failed to create epoll");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_cfg_local->epoll_fd = epoll_fd;

    /* 创建监听 socket */
    int32_t listen_sock = cfg_create_listen_sock();
    if (listen_sock < 0)
    {
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_cfg_local->listen_sock = listen_sock;

    /* 将监听 socket 加入 epoll */
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_sock, &ev) < 0)
    {
        LOG_PERROR("Failed to add listen socket to epoll");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* 启动 Telnet server 线程 */
    if (pthread_create(&g_cfg_local->worker_thread, NULL, cfg_server_thread, NULL) != 0)
    {
        LOG_PERROR("Failed to create server thread");
        g_cfg_local->running = 0;
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_cfg_local->running = 1;

    LOG_INFO("Telnet server listening on port %d", CFG_PORT);
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown - 清理本地状态
// ============================================================================

static void cfg_on_shutdown(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Shutting down server...");
    g_cfg_local->running = 0;

    cli_cleanup();

    cli_global_history_cleanup(&g_cfg_local->global_history);
    pthread_mutex_destroy(&g_cfg_local->history_mutex);

    if (g_cfg_local->listen_sock != DEV_INVALID_FD)
    {
        close(g_cfg_local->listen_sock);
    }

    if (g_cfg_local->epoll_fd != DEV_INVALID_FD)
    {
        close(g_cfg_local->epoll_fd);
    }

    if (g_cfg_local->worker_thread != 0)
    {
        pthread_join(g_cfg_local->worker_thread, NULL);
    }

    if (g_cfg_local->sessions != NULL)
    {
        g_hash_table_destroy(g_cfg_local->sessions);
    }

    /* 注意: ipc_ctx 由 DEV 管理，此处不销毁 */
    g_cfg_local->ipc_ctx = NULL;

    g_free(g_cfg_local);
    g_cfg_local = NULL;

    LOG_INFO("Server shutdown complete");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void cfg_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case IPC_MSG_TYPE_DEV_MODULE_START:
            cfg_on_start(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            cfg_on_connect(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_READY:
            cfg_on_ready(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            cfg_on_shutdown(ctx, msg);
            return;

        default:
            break;
    }

    ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================

#include "cfg_main.h"
#include "dev.h"
#include "ipc.h"
#include "module_ports.h"

__attribute__((constructor)) static void cfg_so_init(void)
{
    LOG_INFO(".so 加载，自初始化");

    /* 创建 IPC 上下文 */
    ipc_context_t *ctx = ipc_init(DEV_MODULE_ID_CFG, "cfg", MODULE_PORT_CFG, cfg_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC 初始化失败");
        return;
    }

    /* 初始化本地状态（epoll、socket、server thread、view tree） */
    cfg_init_local(ctx);
}
