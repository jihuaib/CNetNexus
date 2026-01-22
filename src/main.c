#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cli/nn_cli_handler.h"

#define PORT 3788
#define MAX_CLIENTS 10
#define BACKLOG 5

// Global server socket for signal handler
static int server_socket = -1;
static volatile sig_atomic_t running = 1;

// Signal handler for graceful shutdown
void signal_handler(int signum)
{
    printf("\nReceived signal %d, shutting down...\n", signum);
    running = 0;
    if (server_socket >= 0)
    {
        close(server_socket);
    }
}

// Thread function to handle client connection
void *client_thread(void *arg)
{
    int client_fd = *(int *)arg;
    free(arg);

    // Detach thread so resources are freed automatically
    pthread_detach(pthread_self());

    printf("Client connected (fd: %d)\n", client_fd);

    // Handle the client
    handle_client(client_fd);

    printf("Client disconnected (fd: %d)\n", client_fd);

    return NULL;
}

int main(int argc, char *argv[])
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    int client_fd;
    int opt = 1;
    pthread_t thread_id;

    (void)argc; // Unused parameter
    (void)argv; // Unused parameter

    printf("NetNexus Telnet CLI Server\n");
    printf("==========================\n\n");

    // Initialize CLI base views
    if (nn_cli_init() != 0)
    {
        fprintf(stderr, "Failed to initialize CLI\n");
        exit(EXIT_FAILURE);
    }

    // Auto-register all command modules from config directory
    nn_cli_register_all_modules("../config");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Failed to create socket");
        nn_cli_cleanup();
        exit(EXIT_FAILURE);
    }

    // Set socket options to reuse address
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("Failed to set socket options");
        close(server_socket);
        nn_cli_cleanup();
        exit(EXIT_FAILURE);
    }

    // Configure server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Bind socket to address
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Failed to bind socket");
        close(server_socket);
        nn_cli_cleanup();
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(server_socket, BACKLOG) < 0)
    {
        perror("Failed to listen on socket");
        close(server_socket);
        nn_cli_cleanup();
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d\n", PORT);
    printf("Press Ctrl+C to stop the server\n\n");

    // Main accept loop
    while (running)
    {
        client_len = sizeof(client_addr);
        client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0)
        {
            if (running)
            {
                perror("Failed to accept connection");
            }
            continue;
        }

        // Allocate memory for client fd to pass to thread
        int *client_fd_ptr = malloc(sizeof(int));
        if (client_fd_ptr == NULL)
        {
            fprintf(stderr, "Failed to allocate memory for client thread\n");
            close(client_fd);
            continue;
        }
        *client_fd_ptr = client_fd;

        // Create thread to handle client
        if (pthread_create(&thread_id, NULL, client_thread, client_fd_ptr) != 0)
        {
            perror("Failed to create client thread");
            free(client_fd_ptr);
            close(client_fd);
            continue;
        }
    }

    printf("Server shutdown complete\n");
    nn_cli_cleanup();
    return EXIT_SUCCESS;
}
