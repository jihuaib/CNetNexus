/**
 * @file   cli_main.c
 * @brief  CLI 模块主入口，三阶段初始化
 * @author jhb
 * @date   2026/01/22
 */
#include "cli_main.h"

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
    CLI_PORT = 3788,
    CLI_BACKLOG = 5
};

#define CLI_MAX_EPOLL_EVENTS 16

cli_local_t *g_cli_local = NULL;

// Forward declarations
static void *cli_server_thread(void *arg);

// Server thread function
static void *cli_server_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "cli-telnet");
    log_set_tag(dev_ipc_get_self_name(g_cli_local->dev_ipc_ctx));

    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (g_cli_local && g_cli_local->running)
    {
        struct epoll_event events[CLI_MAX_EPOLL_EVENTS];
        // Wait for events with 1 second timeout
        int nfds = epoll_wait(g_cli_local->epoll_fd, events, CLI_MAX_EPOLL_EVENTS, 1000);

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
            if (events[i].data.fd == g_cli_local->listen_sock)
            {
                // New connection
                client_len = sizeof(client_addr);
                int conn_fd = accept(g_cli_local->listen_sock, (struct sockaddr *)&client_addr, &client_len);

                if (conn_fd < 0)
                {
                    if (g_cli_local && g_cli_local->running)
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
                    g_hash_table_insert(g_cli_local->sessions, fd_key, session);

                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = conn_fd;
                    if (epoll_ctl(g_cli_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &client_ev) < 0)
                    {
                        LOG_PERROR("Failed to add client to epoll");
                        g_hash_table_remove(g_cli_local->sessions, fd_key);
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
                cli_session_t *session = g_hash_table_lookup(g_cli_local->sessions, &fd);
                if (session)
                {
                    if (cli_process_input(session) < 0)
                    {
                        LOG_INFO("Client disconnected (fd: %d)", fd);
                        epoll_ctl(g_cli_local->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        g_hash_table_remove(g_cli_local->sessions, &fd);
                    }
                }
            }
        }
    }

    return NULL;
}

int32_t cli_create_listen_sock()
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
    server_addr.sin_port = htons(CLI_PORT);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        close(server_socket);
        LOG_PERROR("Failed to bind socket");
        return DEV_INVALID_FD;
    }

    if (listen(server_socket, CLI_BACKLOG) < 0)
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

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_CLI,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    dev_ipc_message_free(msg);
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
static int cli_scan_and_load_xml(const char *base_dir)
{
    struct dirent **entries = NULL;
    int count = scandir(base_dir, &entries, NULL, alphasort);
    if (count < 0)
    {
        return 0;
    }

    GPtrArray *xml_paths = g_ptr_array_new_with_free_func(g_free);
    if (!xml_paths)
    {
        for (int i = 0; i < count; i++)
        {
            free(entries[i]);
        }
        free(entries);
        return 0;
    }

    for (int i = 0; i < count; i++)
    {
        struct dirent *entry = entries[i];
        if (!entry)
        {
            continue;
        }

        if (entry->d_name[0] == '.')
        {
            free(entry);
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
                free(entry);
                continue;
            }
        }

        g_ptr_array_add(xml_paths, xml_path);
        free(entry);
    }

    free(entries);

    /* 两阶段加载，避免模块间 view 依赖受目录扫描顺序影响。 */
    int loaded = 0;
    for (guint i = 0; i < xml_paths->len; i++)
    {
        const char *xml_path = g_ptr_array_index(xml_paths, i);
        LOG_INFO("Found XML: %s", xml_path);
        if (cli_xml_load_view_tree_ex(xml_path, &g_cli_local->view_tree, CLI_XML_LOAD_VIEWS) == ERRCODE_SUCCESS)
        {
            loaded++;
        }
        else
        {
            LOG_ERROR("Failed to load XML views: %s", xml_path);
        }
    }

    for (guint i = 0; i < xml_paths->len; i++)
    {
        const char *xml_path = g_ptr_array_index(xml_paths, i);
        if (cli_xml_load_view_tree_ex(xml_path, &g_cli_local->view_tree, CLI_XML_LOAD_COMMANDS) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("Failed to load XML commands: %s", xml_path);
        }
    }

    g_ptr_array_free(xml_paths, TRUE);
    return loaded;
}

/**
 * @brief 自动发现并加载所有模块的 commands.xml
 *
 * 按以下优先级扫描目录：
 * 1. 环境变量 NN_WORK_DIR（取 resources 子目录）
 * 2. 相对于可执行文件的开发路径
 */
static void cli_discover_and_load_xml(void)
{
    LOG_INFO("Auto-discovering and loading commands.xml...");

    int total = 0;

    /* 优先级 1: 环境变量 NN_WORK_DIR（生产环境） */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        char *resources_dir = g_build_filename(work_dir, "resources", NULL);
        total = cli_scan_and_load_xml(resources_dir);
        g_free(resources_dir);
        if (total > 0)
        {
            LOG_INFO("Loaded %d XML configs from NN_WORK_DIR", total);
            return;
        }
    }

    /* 优先级 2: 相对于可执行文件的开发路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        char *dev_path = g_build_filename(exe_dir, "..", "..", "src", NULL);
        total = cli_scan_and_load_xml(dev_path);
        g_free(dev_path);
        if (total > 0)
        {
            LOG_INFO("Loaded %d XML configs from dev path", total);
            return;
        }
    }

    LOG_ERROR("No commands.xml found");
}

// ============================================================================
// 本地状态初始化（从 constructor 调用）
// ============================================================================

void cli_init_local(dev_ipc_context_t *ctx)
{
    g_cli_local = g_malloc0(sizeof(cli_local_t));
    pthread_mutex_init(&g_cli_local->history_mutex, NULL);
    g_cli_local->running = 0;
    g_cli_local->epoll_fd = DEV_INVALID_FD;
    g_cli_local->listen_sock = DEV_INVALID_FD;
    g_cli_local->worker_thread = 0;
    g_cli_local->dev_ipc_ctx = ctx;
    g_cli_local->sessions = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, (GDestroyNotify)cli_session_destroy);

    /* 创建视图树 */
    cli_view_node_t *user_view = cli_view_create(CLI_VIEW_USER, "<NetNexus>");
    if (!user_view)
    {
        LOG_ERROR("Failed to create user view");
        return;
    }
    g_cli_local->view_tree.root = user_view;

    cli_view_node_t *config_view = cli_view_create(CLI_VIEW_CONFIG, "<NetNexus(config)>");
    if (!config_view)
    {
        LOG_ERROR("Failed to create config view");
        return;
    }
    cli_view_add_child(user_view, config_view);

    /* 自动发现并加载所有模块的 commands.xml */
    cli_discover_and_load_xml();

    LOG_INFO("Local state initialization complete");
}

// ============================================================================
// Phase 1: MODULE_START — CFG 不需要连接其他模块，直接回复 OK
// ============================================================================

static void cli_on_start(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = cli_local_ipc_ctx();
    LOG_INFO("Phase 1: MODULE_START (no external connections needed)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void cli_on_connect(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = cli_local_ipc_ctx();
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 加载所有 XML
// ============================================================================

static void cli_on_ready(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = cli_local_ipc_ctx();
    LOG_INFO("Phase 3: MODULE_READY - Starting Telnet server");

    /* 创建 epoll */
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        LOG_PERROR("Failed to create epoll");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_cli_local->epoll_fd = epoll_fd;

    /* 创建监听 socket */
    int32_t listen_sock = cli_create_listen_sock();
    if (listen_sock < 0)
    {
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_cli_local->listen_sock = listen_sock;

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
    g_cli_local->running = 1;
    if (pthread_create(&g_cli_local->worker_thread, NULL, cli_server_thread, NULL) != 0)
    {
        LOG_PERROR("Failed to create server thread");
        g_cli_local->running = 0;
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    LOG_INFO("Telnet server listening on port %d", CLI_PORT);
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void cli_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            cli_on_start(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            cli_on_connect(msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            cli_on_ready(msg);
            return;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================

#include "cli_main.h"

int cli_module_init(void)
{
    log_set_tag("cli");
    LOG_INFO("Module initialization");

    /* 创建 IPC 上下文 */
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_CLI, "cli", DEV_MODULE_PORT_CLI, cli_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    /* 初始化本地状态（epoll、socket、server thread、view tree） */
    cli_init_local(ctx);
    return 0;
}

void cli_module_cleanup(void)
{
    if (!g_cli_local)
    {
        return;
    }

    dev_ipc_context_t *ctx = g_cli_local->dev_ipc_ctx;
    g_cli_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    if (!g_cli_local)
    {
        return;
    }

    g_cli_local->running = 0;
    cli_cleanup();

    cli_global_history_cleanup(&g_cli_local->global_history);
    pthread_mutex_destroy(&g_cli_local->history_mutex);

    if (g_cli_local->listen_sock != DEV_INVALID_FD)
    {
        close(g_cli_local->listen_sock);
    }

    if (g_cli_local->epoll_fd != DEV_INVALID_FD)
    {
        close(g_cli_local->epoll_fd);
    }

    if (g_cli_local->worker_thread != 0)
    {
        pthread_join(g_cli_local->worker_thread, NULL);
    }

    if (g_cli_local->sessions != NULL)
    {
        g_hash_table_destroy(g_cli_local->sessions);
    }

    g_free(g_cli_local);
    g_cli_local = NULL;
}
