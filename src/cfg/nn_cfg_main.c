#include "nn_dev.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nn_cli_handler.h"
#include "nn_cli_xml_parser.h"

#define CFG_PORT 3788
#define CFG_BACKLOG 5

// Server state
static int g_server_socket = -1;
static pthread_t g_server_thread;
static int g_server_running = 0;

// Forward declarations
static void *cfg_server_thread(void *arg);
static void *cfg_client_thread(void *arg);

// Client thread function
static void *cfg_client_thread(void *arg)
{
    int client_fd = *(int *)arg;
    free(arg);

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

        int *client_fd_ptr = malloc(sizeof(int));
        if (!client_fd_ptr)
        {
            close(client_fd);
            continue;
        }
        *client_fd_ptr = client_fd;

        if (pthread_create(&thread_id, NULL, cfg_client_thread, client_fd_ptr) != 0)
        {
            perror("[cfg] Failed to create client thread");
            free(client_fd_ptr);
            close(client_fd);
        }
    }

    printf("[cfg] Server thread exiting\n");
    return NULL;
}

// Module initialization
static int32_t cfg_module_init(void)
{
    extern nn_cli_view_tree_t g_view_tree;

    // Initialize CLI view tree
    nn_cli_cleanup();

    // init base view structure
    // user view
    nn_cli_view_node_t *user_view = nn_cli_view_create("user", "<{hostname}>");
    if (user_view == NULL)
    {
        fprintf(stderr, "[cfg] Failed to create user view\n");
        return -1;
    }
    g_view_tree.root = user_view;

    // config view
    nn_cli_view_node_t *config_view = nn_cli_view_create("config", "<{hostname}(config)>");
    if (config_view == NULL)
    {
        fprintf(stderr, "[cfg] Failed to create config view\n");
        return -1;
    }
    nn_cli_view_add_child(user_view, config_view);

    // Load commands from all modules that have xml_path
    GHashTable *module_registry = nn_get_module_registry();
    if (module_registry)
    {
        GHashTableIter iter;
        gpointer key, value;
        
        g_hash_table_iter_init(&iter, module_registry);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            nn_dev_module_t *module = (nn_dev_module_t *)value;
            if (module->xml_path)
            {
                printf("[cfg] Loading commands from: %s\n", module->xml_path);
                if (nn_cli_xml_load_view_tree(module->xml_path, &g_view_tree) == 0)
                {
                    printf("[cfg]   OK: %s\n", module->name);
                }
                else
                {
                    fprintf(stderr, "[cfg]   FAIL: %s\n", module->name);
                }
            }
        }
    }

    // Create server socket
    g_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_socket < 0)
    {
        perror("[cfg] Failed to create socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(g_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("[cfg] Failed to set socket options");
        close(g_server_socket);
        return -1;
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
        return -1;
    }

    if (listen(g_server_socket, CFG_BACKLOG) < 0)
    {
        perror("[cfg] Failed to listen");
        close(g_server_socket);
        return -1;
    }

    // Start server thread
    g_server_running = 1;
    if (pthread_create(&g_server_thread, NULL, cfg_server_thread, NULL) != 0)
    {
        perror("[cfg] Failed to create server thread");
        close(g_server_socket);
        return -1;
    }

    printf("[cfg] Telnet server listening on port %d\n", CFG_PORT);
    return 0;
}

// Module cleanup
static void cfg_module_cleanup(void)
{
    printf("[cfg] Shutting down server...\n");

    g_server_running = 0;

    if (g_server_socket >= 0)
    {
        // Use shutdown() to unblock accept()
        shutdown(g_server_socket, SHUT_RDWR);
        close(g_server_socket);
        g_server_socket = -1;
    }

    // Wait for server thread to exit
    pthread_join(g_server_thread, NULL);

    nn_cli_cleanup();
    printf("[cfg] Server shutdown complete\n");
}

// Register cfg module using constructor attribute
static void __attribute__((constructor)) register_cfg_module(void)
{
    nn_dev_register_module(NN_MODULE_ID_CFG, "nn_cfg", cfg_module_init, cfg_module_cleanup);
}
