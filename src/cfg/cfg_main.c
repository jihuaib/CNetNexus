/**
 * @file   cfg_main.c
 * @brief  CFG 模块主入口，三阶段初始化
 * @author jhb
 * @date   2026/01/22
 */
#include "cfg_main.h"

#include <arpa/inet.h>
#include <glib.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "cfg_registry.h"
#include "cli.h"
#include "cli_handler.h"
#include "cli_xml_parser.h"
#include "dev.h"
#include "errcode.h"

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

    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (!dev_shutdown_requested())
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
            perror("[cfg] epoll_wait failed");
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
                    if (!dev_shutdown_requested())
                    {
                        perror("[cfg] Accept failed");
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
                        perror("[cfg] Failed to add client to epoll");
                        g_hash_table_remove(g_cfg_local->sessions, fd_key);
                    }
                    else
                    {
                        printf("[cfg] Client connected (fd: %d)\n", conn_fd);
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
                        printf("[cfg] Client disconnected (fd: %d)\n", fd);
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
        perror("[cfg] Failed to create socket");
        return DEV_INVALID_FD;
    }

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        close(server_socket);
        perror("[cfg] Failed to set socket options");
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
        perror("[cfg] Failed to bind socket");
        return DEV_INVALID_FD;
    }

    if (listen(server_socket, CFG_BACKLOG) < 0)
    {
        close(server_socket);
        perror("[cfg] Failed to listen");
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
// Phase 1: MODULE_START - 创建上下文、epoll、Telnet
// ============================================================================

static void cfg_on_start(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[cfg] Phase 1: MODULE_START\n");

    g_cfg_local = g_malloc0(sizeof(cfg_local_t));
    pthread_mutex_init(&g_cfg_local->history_mutex, NULL);
    g_cfg_local->epoll_fd = DEV_INVALID_FD;
    g_cfg_local->listen_sock = DEV_INVALID_FD;
    g_cfg_local->worker_thread = 0;
    g_cfg_local->ipc_ctx = ctx;
    g_cfg_local->sessions = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, (GDestroyNotify)cli_session_destroy);

    /* 创建 Telnet 服务器的 epoll */
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        perror("[cfg] Failed to create epoll");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_cfg_local->epoll_fd = epoll_fd;

    int32_t listen_sock = cfg_create_listen_sock();
    if (listen_sock < 0)
    {
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_cfg_local->listen_sock = listen_sock;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_sock, &ev) < 0)
    {
        perror("[cfg] Failed to add listen socket to epoll");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    /* 启动 server 线程 */
    if (pthread_create(&g_cfg_local->worker_thread, NULL, cfg_server_thread, NULL) != ERRCODE_SUCCESS)
    {
        perror("[cfg] Failed to create server thread");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    printf("[cfg] Telnet server listening on port %d\n", CFG_PORT);

    /* 创建视图树 */
    cli_view_node_t *user_view = cli_view_create(CLI_VIEW_USER, "user", "<NetNexus>");
    if (!user_view)
    {
        fprintf(stderr, "[cfg] Failed to create user view\n");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    g_cfg_local->view_tree.root = user_view;

    cli_view_node_t *config_view = cli_view_create(CLI_VIEW_CONFIG, "config", "<NetNexus(config)>");
    if (!config_view)
    {
        fprintf(stderr, "[cfg] Failed to create config view\n");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }
    cli_view_add_child(user_view, config_view);

    printf("[cfg] Module started\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT - CFG 不主动连接其他模块
// ============================================================================

static void cfg_on_connect(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[cfg] Phase 2: MODULE_CONNECT (无需主动连接)\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY - 加载所有 XML
// ============================================================================

static void cfg_on_ready(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[cfg] Phase 3: MODULE_READY - 加载 XML\n");

    int failed_count = 0;
    extern GSList *g_xml_registry;

    for (GSList *node = g_xml_registry; node != NULL; node = node->next)
    {
        cfg_xml_entry_t *entry = (cfg_xml_entry_t *)node->data;

        printf("[cfg] Loading: %s\n", entry->xml_path);
        if (cli_xml_load_view_tree(entry->xml_path, &g_cfg_local->view_tree) == ERRCODE_SUCCESS)
        {
            printf("[cfg] Commands loaded success\n");
        }
        else
        {
            fprintf(stderr, "[cfg] Failed to load XML\n");
            failed_count++;
        }
    }

    printf("[cfg] Module cli initialization complete (failures: %d)\n", failed_count);
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown - 清理本地状态
// ============================================================================

static void cfg_on_shutdown(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[cfg] Shutting down server...\n");

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

    printf("[cfg] Server shutdown complete\n");
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
// 入口函数：创建 IPC 上下文
// ============================================================================

