/**
 * @file   cfg_main.c
 * @brief  CFG 模块主入口，模块注册和初始化
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
#include "db.h"
#include "db_rpc.h"
#include "dev.h"
#include "errcode.h"
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
            perror("[bgp] epoll_wait failed");
            break;
        }

        if (nfds == 0)
        {
            // Timeout - do periodic BGP tasks
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
                        // session_destroy will close conn_fd
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
    // Create server socket
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

static int cfg_init_local()
{
    g_cfg_local = g_malloc0(sizeof(cfg_local_t));
    pthread_mutex_init(&g_cfg_local->history_mutex, NULL);
    g_cfg_local->epoll_fd = DEV_INVALID_FD;
    g_cfg_local->listen_sock = DEV_INVALID_FD;
    g_cfg_local->worker_thread = 0;
    g_cfg_local->sessions = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, (GDestroyNotify)cli_session_destroy);
    g_cfg_local->xml_db_defs = NULL;

    // 初始化 IPC（CFG 不需要接收回调消息，设 msg_handler 为 NULL）
    g_cfg_local->ipc_ctx = ipc_init(DEV_MODULE_ID_CFG, "cfg", NULL, NULL);
    if (!g_cfg_local->ipc_ctx)
    {
        fprintf(stderr, "[cfg] Failed to initialize IPC\n");
        return ERRCODE_FAIL;
    }

    // 各模块主动连接到 CFG，CFG 只需监听
    // CFG 主动连接到 DB（用于 db_rpc）
    ipc_connect(g_cfg_local->ipc_ctx, DEV_MODULE_ID_DB);

    // 创建 Telnet 服务器的 epoll
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        perror("[cfg] Failed to create epoll");
        return ERRCODE_FAIL;
    }
    g_cfg_local->epoll_fd = epoll_fd;

    struct epoll_event ev;

    int32_t listen_sock = cfg_create_listen_sock();
    if (listen_sock < 0)
    {
        return ERRCODE_FAIL;
    }
    g_cfg_local->listen_sock = listen_sock;

    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_sock, &ev) < 0)
    {
        perror("[cfg] Failed to add listen socket to epoll");
        return ERRCODE_FAIL;
    }

    // Start server thread
    if (pthread_create(&g_cfg_local->worker_thread, NULL, cfg_server_thread, NULL) != ERRCODE_SUCCESS)
    {
        perror("[cfg] Failed to create server thread");
        return ERRCODE_FAIL;
    }

    printf("[cfg] Telnet server listening on port %d\n", CFG_PORT);

    return ERRCODE_SUCCESS;
}

static void cfg_cleanup_local()
{
    if (g_cfg_local == NULL)
    {
        return;
    }

    cli_global_history_cleanup(&g_cfg_local->global_history);
    pthread_mutex_destroy(&g_cfg_local->history_mutex);

    // 销毁 IPC
    if (g_cfg_local->ipc_ctx)
    {
        ipc_destroy(g_cfg_local->ipc_ctx);
    }

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

    if (g_cfg_local->xml_db_defs != NULL)
    {
        g_list_free_full(g_cfg_local->xml_db_defs, (GDestroyNotify)cfg_xml_db_def_free);
    }

    g_free(g_cfg_local);
    g_cfg_local = NULL;
}

// Module initialization
static int32_t cfg_module_init()
{
    int ret = cfg_init_local();
    if (ret != ERRCODE_SUCCESS)
    {
        cfg_cleanup_local();
        return ERRCODE_FAIL;
    }

    cli_view_node_t *user_view = cli_view_create(CLI_VIEW_USER, "user", "<NetNexus>");
    if (!user_view)
    {
        cfg_cleanup_local();
        fprintf(stderr, "[cfg] Failed to create user view\n");
        return ERRCODE_FAIL;
    }
    g_cfg_local->view_tree.root = user_view;

    cli_view_node_t *config_view = cli_view_create(CLI_VIEW_CONFIG, "config", "<NetNexus(config)>");
    if (!config_view)
    {
        cfg_cleanup_local();
        fprintf(stderr, "[cfg] Failed to create config view\n");
        return ERRCODE_FAIL;
    }
    cli_view_add_child(user_view, config_view);

    // Initialize all modules that registered XML (iterate g_xml_registry)
    printf("[cfg] Initializing cli modules:\n");
    printf("======================================\n");

    int failed_count = 0;
    extern GSList *g_xml_registry; // Access cfg registry's list directly

    for (GSList *node = g_xml_registry; node != NULL; node = node->next)
    {
        cfg_xml_entry_t *entry = (cfg_xml_entry_t *)node->data;

        // Load XML
        printf("[cfg] Loading: %s\n", entry->xml_path);
        if (cli_xml_load_view_tree(entry->xml_path, &g_cfg_local->view_tree) == ERRCODE_SUCCESS)
        {
            printf("[cfg]   ✓ Commands loaded\n");
        }
        else
        {
            fprintf(stderr, "[cfg]   ✗ Failed to load XML\n");
            failed_count++;
        }
    }

    printf("\n[cfg] Module cli initialization complete (failures: %d)\n\n", failed_count);

    // Initialize databases from XML definitions
    printf("[cfg] Initializing databases:\n");
    printf("======================================\n");

    // 通过 RPC 通知 DB 模块创建数据库（只创建数据库，不创建表）
    for (GList *node = g_cfg_local->xml_db_defs; node != NULL; node = node->next)
    {
        cfg_xml_db_def_t *xml_def = (cfg_xml_db_def_t *)node->data;
        printf("[cfg] 通过 RPC 创建数据库: %s (module_id=%u)\n", xml_def->db_name, xml_def->module_id);

        if (db_rpc_create_db(g_cfg_local->ipc_ctx, xml_def->db_name, xml_def->module_id) != ERRCODE_SUCCESS)
        {
            fprintf(stderr, "[cfg] 创建数据库失败: %s\n", xml_def->db_name);
        }
    }
    // 清理中间数据
    g_list_free_full(g_cfg_local->xml_db_defs, (GDestroyNotify)cfg_xml_db_def_free);
    g_cfg_local->xml_db_defs = NULL;

    printf("\n[cfg] Database RPC client initialized\n\n");

    return ERRCODE_SUCCESS;
}

// Module cleanup
static void cfg_module_cleanup(void)
{
    printf("[cfg] Shutting down server...\n");
    cli_cleanup();
    cfg_cleanup_local();
    printf("[cfg] Server shutdown complete\n");
}

// Register cfg module using constructor attribute
static void __attribute__((constructor)) register_cfg_module(void)
{
    dev_register_module(DEV_MODULE_ID_CFG, "cfg", cfg_module_init, cfg_module_cleanup);

    char cfg_xml_path[256];
    if (resolve_xml_path("cfg", cfg_xml_path, sizeof(cfg_xml_path)) == 0)
    {
        cfg_register_module_xml(DEV_MODULE_ID_CFG, cfg_xml_path);
    }
    else
    {
        fprintf(stderr, "[cfg] Warning: Could not resolve XML path for cfg module\n");
    }
}
