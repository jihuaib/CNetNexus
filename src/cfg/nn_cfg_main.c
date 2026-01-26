#include <arpa/inet.h>
#include <glib.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nn_cfg.h"
#include "nn_cfg_registry.h"
#include "nn_cli_handler.h"
#include "nn_cli_xml_parser.h"
#include "nn_dev.h"
#include "nn_errcode.h"
#include "nn_path_utils.h"

enum
{
    CFG_PORT = 3788,
    CFG_BACKLOG = 5
};

// Server state
static int g_server_socket = -1;
static pthread_t g_server_thread;
static pthread_t g_worker_thread;
static int g_server_running = 0;
static int g_worker_running = 0;

static int g_cfg_event_fd = -1;
static nn_dev_module_mq_t *g_cfg_mq = NULL;

// Forward declarations
static void *cfg_server_thread(void *arg);
static void *cfg_client_thread(void *arg);
static void *cfg_worker_thread(void *arg);

// Client thread function
static void *cfg_client_thread(void *arg)
{
    int client_fd = *(int *)arg;
    g_free(arg);

    pthread_detach(pthread_self());

    printf("[cfg] Client connected (fd: %d)\n", client_fd);
    handle_client(client_fd);
    printf("[cfg] Client disconnected (fd: %d)\n", client_fd);

    return NULL;
}

// Server thread function
static void *cfg_server_thread(void *arg)
{
    (void)arg;

    struct sockaddr_in client_addr;
    socklen_t client_len;
    int client_fd;
    pthread_t thread_id;

    printf("[cfg] Server thread started on port %d\n", CFG_PORT);

    while (g_server_running && !nn_shutdown_requested())
    {
        client_len = sizeof(client_addr);
        client_fd = accept(g_server_socket, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0)
        {
            if (g_server_running && !nn_shutdown_requested())
            {
                perror("[cfg] Accept failed");
            }
            continue;
        }

        int *client_fd_ptr = g_malloc0(sizeof(int));

        *client_fd_ptr = client_fd;

        if (pthread_create(&thread_id, NULL, cfg_client_thread, client_fd_ptr) != NN_ERRCODE_SUCCESS)
        {
            perror("[cfg] Failed to create client thread");
            g_free(client_fd_ptr);
            close(client_fd);
        }
    }

    printf("[cfg] Server thread exiting\n");
    return NULL;
}

// Module initialization
static int32_t cfg_module_init()
{
    extern nn_cli_view_tree_t g_view_tree;

    // 1. Initialize CLI view tree
    nn_cli_cleanup();

    nn_cli_view_node_t *user_view = nn_cli_view_create(NN_CFG_CLI_VIEW_USER, "user", "<{hostname}>");
    if (!user_view)
    {
        fprintf(stderr, "[cfg] Failed to create user view\n");
        return NN_ERRCODE_FAIL;
    }
    g_view_tree.root = user_view;

    nn_cli_view_node_t *config_view = nn_cli_view_create(NN_CFG_CLI_VIEW_CONFIG, "config", "<{hostname}(config)>");
    if (!config_view)
    {
        fprintf(stderr, "[cfg] Failed to create config view\n");
        return NN_ERRCODE_FAIL;
    }
    nn_cli_view_add_child(user_view, config_view);

    // 2. Initialize all modules that registered XML (iterate g_xml_registry)
    printf("[cfg] Initializing cli modules:\n");
    printf("======================================\n");

    int failed_count = 0;
    extern GSList *g_xml_registry; // Access cfg registry's list directly

    for (GSList *node = g_xml_registry; node != NULL; node = node->next)
    {
        nn_cfg_xml_entry_t *entry = (nn_cfg_xml_entry_t *)node->data;

        // Load XML
        printf("[cfg] Loading: %s\n", entry->xml_path);
        if (nn_cli_xml_load_view_tree(entry->xml_path, &g_view_tree) == NN_ERRCODE_SUCCESS)
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

    // Create server socket
    g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_socket < 0)
    {
        perror("[cfg] Failed to create socket");
        return NN_ERRCODE_FAIL;
    }

    int opt = 1;
    if (setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("[cfg] Failed to set socket options");
        close(g_server_socket);
        return NN_ERRCODE_FAIL;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(CFG_PORT);

    if (bind(g_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("[cfg] Failed to bind socket");
        close(g_server_socket);
        return NN_ERRCODE_FAIL;
    }

    if (listen(g_server_socket, CFG_BACKLOG) < 0)
    {
        perror("[cfg] Failed to listen");
        close(g_server_socket);
        return NN_ERRCODE_FAIL;
    }

    // Start server thread
    g_server_running = 1;
    if (pthread_create(&g_server_thread, NULL, cfg_server_thread, NULL) != NN_ERRCODE_SUCCESS)
    {
        perror("[cfg] Failed to create server thread");
        close(g_server_socket);
        return NN_ERRCODE_FAIL;
    }

    printf("[cfg] Telnet server listening on port %d\n", CFG_PORT);
    return NN_ERRCODE_SUCCESS;
}

// Module cleanup
static void cfg_module_cleanup(void)
{
    printf("[cfg] Shutting down server...\n");

    g_server_running = 0;
    g_worker_running = 0;

    if (g_server_socket >= 0)
    {
        // Use shutdown() to unblock accept()
        shutdown(g_server_socket, SHUT_RDWR);
        close(g_server_socket);
        g_server_socket = -1;
    }

    // Wait for server thread to exit
    pthread_join(g_server_thread, NULL);
    pthread_join(g_worker_thread, NULL);

    if (g_cfg_event_fd >= 0)
    {
        close(g_cfg_event_fd);
    }
    if (g_cfg_mq)
    {
        nn_nn_mq_destroy(g_cfg_mq);
    }

    nn_cli_cleanup();
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
