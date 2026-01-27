#include "nn_cfg_main.h"

#include <arpa/inet.h>
#include <glib.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include "nn_cfg.h"
#include "nn_cfg_registry.h"
#include "nn_cli_handler.h"
#include "nn_cli_xml_parser.h"
#include "nn_db.h"
#include "nn_dev.h"
#include "nn_errcode.h"
#include "nn_path_utils.h"

enum
{
    CFG_PORT = 3788,
    CFG_BACKLOG = 5
};

#define CFG_MAX_EPOLL_EVENTS 16

nn_cfg_local_t *g_nn_cfg_local = NULL;

// Forward declarations
static void *cfg_server_thread(void *arg);

// Server thread function
static void *cfg_server_thread(void *arg)
{
    (void)arg;

    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (!nn_dev_shutdown_requested())
    {
        struct epoll_event events[CFG_MAX_EPOLL_EVENTS];
        // Wait for events with 1 second timeout
        int nfds = epoll_wait(g_nn_cfg_local->epoll_fd, events, CFG_MAX_EPOLL_EVENTS, 1000);

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
            if (events[i].data.fd == g_nn_cfg_local->event_fd)
            {
                // Message queue has data
                // nn_cfg_process_messages(g_nn_cfg_local);
            }
            else if (events[i].data.fd == g_nn_cfg_local->listen_sock)
            {
                // New connection
                client_len = sizeof(client_addr);
                int conn_fd = accept(g_nn_cfg_local->listen_sock, (struct sockaddr *)&client_addr, &client_len);

                if (conn_fd < 0)
                {
                    if (!nn_dev_shutdown_requested())
                    {
                        perror("[cfg] Accept failed");
                    }
                    continue;
                }

                int *fd_key = g_malloc(sizeof(int));
                *fd_key = conn_fd;

                nn_cli_session_t *session = nn_cli_session_create(conn_fd);
                if (session)
                {
                    g_hash_table_insert(g_nn_cfg_local->sessions, fd_key, session);

                    struct epoll_event client_ev;
                    client_ev.events = EPOLLIN;
                    client_ev.data.fd = conn_fd;
                    if (epoll_ctl(g_nn_cfg_local->epoll_fd, EPOLL_CTL_ADD, conn_fd, &client_ev) < 0)
                    {
                        perror("[cfg] Failed to add client to epoll");
                        g_hash_table_remove(g_nn_cfg_local->sessions, fd_key);
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
                nn_cli_session_t *session = g_hash_table_lookup(g_nn_cfg_local->sessions, &fd);
                if (session)
                {
                    if (nn_cli_process_input(session) < 0)
                    {
                        printf("[cfg] Client disconnected (fd: %d)\n", fd);
                        epoll_ctl(g_nn_cfg_local->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                        g_hash_table_remove(g_nn_cfg_local->sessions, &fd);
                    }
                }
            }
        }
    }

    return NULL;
}

int32_t nn_cfg_create_listen_sock()
{
    // Create server socket
    int32_t server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("[cfg] Failed to create socket");
        return NN_DEV_INVALID_FD;
    }

    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        close(server_socket);
        perror("[cfg] Failed to set socket options");
        return NN_DEV_INVALID_FD;
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
        return NN_DEV_INVALID_FD;
    }

    if (listen(server_socket, CFG_BACKLOG) < 0)
    {
        close(server_socket);
        perror("[cfg] Failed to listen");
        return NN_DEV_INVALID_FD;
    }

    return server_socket;
}

static int nn_cfg_init_local()
{
    g_nn_cfg_local = g_malloc0(sizeof(nn_cfg_local_t));
    pthread_mutex_init(&g_nn_cfg_local->history_mutex, NULL);
    g_nn_cfg_local->epoll_fd = NN_DEV_INVALID_FD;
    g_nn_cfg_local->event_fd = NN_DEV_INVALID_FD;
    g_nn_cfg_local->listen_sock = NN_DEV_INVALID_FD;
    g_nn_cfg_local->worker_thread = 0;
    g_nn_cfg_local->sessions =
        g_hash_table_new_full(g_int_hash, g_int_equal, g_free, (GDestroyNotify)nn_cli_session_destroy);

    nn_dev_module_mq_t *mq = nn_dev_mq_create();
    if (mq == NULL)
    {
        fprintf(stderr, "[cfg] Failed to create message queue\n");
        return NN_ERRCODE_FAIL;
    }
    g_nn_cfg_local->mq = mq;

    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0)
    {
        fprintf(stderr, "[cfg] Failed to create event fd\n");
        return NN_ERRCODE_FAIL;
    }
    g_nn_cfg_local->event_fd = event_fd;

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        perror("[cfg] Failed to create epoll");
        return NN_ERRCODE_FAIL;
    }
    g_nn_cfg_local->epoll_fd = epoll_fd;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = event_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &ev) < 0)
    {
        perror("[cfg] Failed to add eventfd to epoll");
        return NN_ERRCODE_FAIL;
    }

    int32_t listen_sock = nn_cfg_create_listen_sock();
    if (listen_sock < 0)
    {
        return NN_ERRCODE_FAIL;
    }
    g_nn_cfg_local->listen_sock = listen_sock;

    ev.events = EPOLLIN;
    ev.data.fd = listen_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_sock, &ev) < 0)
    {
        perror("[cfg] Failed to add listen socket to epoll");
        return NN_ERRCODE_FAIL;
    }

    // Start server thread
    if (pthread_create(&g_nn_cfg_local->worker_thread, NULL, cfg_server_thread, NULL) != NN_ERRCODE_SUCCESS)
    {
        perror("[cfg] Failed to create server thread");
        return NN_ERRCODE_FAIL;
    }

    printf("[cfg] Telnet server listening on port %d\n", CFG_PORT);

    return NN_ERRCODE_SUCCESS;
}

static void nn_cfg_cleanup_local()
{
    nn_cli_global_history_cleanup(&g_nn_cfg_local->global_history);
    pthread_mutex_destroy(&g_nn_cfg_local->history_mutex);

    if (g_nn_cfg_local->mq != NULL)
    {
        nn_dev_mq_destroy(g_nn_cfg_local->mq);
    }

    if (g_nn_cfg_local->event_fd != NN_DEV_INVALID_FD)
    {
        close(g_nn_cfg_local->event_fd);
    }

    if (g_nn_cfg_local->listen_sock != NN_DEV_INVALID_FD)
    {
        close(g_nn_cfg_local->listen_sock);
    }

    if (g_nn_cfg_local->epoll_fd != NN_DEV_INVALID_FD)
    {
        close(g_nn_cfg_local->epoll_fd);
    }

    if (g_nn_cfg_local->worker_thread != 0)
    {
        pthread_join(g_nn_cfg_local->worker_thread, NULL);
    }

    if (g_nn_cfg_local->sessions != NULL)
    {
        g_hash_table_destroy(g_nn_cfg_local->sessions);
    }

    g_free(g_nn_cfg_local);
}

// Module initialization
static int32_t cfg_module_init()
{
    int ret = nn_cfg_init_local();
    if (ret != NN_ERRCODE_SUCCESS)
    {
        nn_cfg_cleanup_local();
        return NN_ERRCODE_FAIL;
    }

    nn_cli_view_node_t *user_view = nn_cli_view_create(NN_CFG_CLI_VIEW_USER, "user", "<{hostname}>");
    if (!user_view)
    {
        nn_cfg_cleanup_local();
        fprintf(stderr, "[cfg] Failed to create user view\n");
        return NN_ERRCODE_FAIL;
    }
    g_nn_cfg_local->view_tree.root = user_view;

    nn_cli_view_node_t *config_view = nn_cli_view_create(NN_CFG_CLI_VIEW_CONFIG, "config", "<{hostname}(config)>");
    if (!config_view)
    {
        nn_cfg_cleanup_local();
        fprintf(stderr, "[cfg] Failed to create config view\n");
        return NN_ERRCODE_FAIL;
    }
    nn_cli_view_add_child(user_view, config_view);

    // Initialize all modules that registered XML (iterate g_xml_registry)
    printf("[cfg] Initializing cli modules:\n");
    printf("======================================\n");

    int failed_count = 0;
    extern GSList *g_xml_registry; // Access cfg registry's list directly

    for (GSList *node = g_xml_registry; node != NULL; node = node->next)
    {
        nn_cfg_xml_entry_t *entry = (nn_cfg_xml_entry_t *)node->data;

        // Load XML
        printf("[cfg] Loading: %s\n", entry->xml_path);
        if (nn_cli_xml_load_view_tree(entry->xml_path, &g_nn_cfg_local->view_tree) == NN_ERRCODE_SUCCESS)
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
    if (nn_db_initialize_all() != NN_ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[cfg] Warning: Database initialization had errors\n");
    }
    printf("\n[cfg] Database initialization complete\n\n");

    return NN_ERRCODE_SUCCESS;
}

// Module cleanup
static void cfg_module_cleanup(void)
{
    printf("[cfg] Shutting down server...\n");
    nn_cli_cleanup();
    nn_cfg_cleanup_local();
    printf("[cfg] Server shutdown complete\n");
}

// Register cfg module using constructor attribute
static void __attribute__((constructor)) register_cfg_module(void)
{
    nn_dev_register_module(NN_DEV_MODULE_ID_CFG, "nn_cfg", cfg_module_init, cfg_module_cleanup);

    char cfg_xml_path[256];
    if (nn_resolve_xml_path("cfg", cfg_xml_path, sizeof(cfg_xml_path)) == 0)
    {
        nn_cfg_register_module_xml(NN_DEV_MODULE_ID_CFG, cfg_xml_path);
    }
    else
    {
        fprintf(stderr, "[cfg] Warning: Could not resolve XML path for cfg module\n");
    }
}
