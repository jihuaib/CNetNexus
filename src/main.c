#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "nn_module_registry.h"

// Signal handler for graceful shutdown
static void signal_handler(int signum)
{
    printf("\nReceived signal %d, requesting shutdown...\n", signum);
    nn_request_shutdown();
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize all registered modules
    if (nn_init_all_modules() != 0)
    {
        fprintf(stderr, "Warning: Some modules failed to initialize\n");
    }

    printf("All modules initialized. Press Ctrl+C to stop.\n\n");

    // Main loop - wait for shutdown signal
    while (!nn_shutdown_requested())
    {
        sleep(1);
    }

    // Cleanup all modules
    nn_cleanup_all_modules();

    printf("\nNetNexus shutdown complete\n");
    return EXIT_SUCCESS;
}
