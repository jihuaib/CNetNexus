#include "nn_dev.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "nn_cli_handler.h"

// BGP module state
static pthread_t g_bgp_thread;
static int g_bgp_running = 0;
static nn_module_mq_t *g_bgp_mq = NULL;

// Handle incoming message
static void bgp_handle_message(nn_dev_message_t *msg)
{
    printf("[bgp] Received message: type=%s, len=%zu\n", msg->type, msg->data_len);
    
    if (strcmp(msg->type, "config_update") == 0)
    {
        printf("[bgp] Config update: %s\n", (char *)msg->data);
    }
    else if (strcmp(msg->type, "command") == 0)
    {
        printf("[bgp] Executing command: %s\n", (char *)msg->data);
        // Here we would parse and execute the BGP command
    }
    else if (strcmp(msg->type, "shutdown") == 0)
    {
        printf("[bgp] Shutdown request received\n");
        g_bgp_running = 0;
    }
}

// BGP worker thread
static void *bgp_worker_thread(void *arg)
{
    nn_module_mq_t *mq = (nn_module_mq_t *)arg;

    printf("[bgp] Worker thread started\n");

    while (g_bgp_running && !nn_shutdown_requested())
    {
        // Wait for messages with 1 second timeout
        int ret = nn_mq_wait(mq, 1000);
        
        if (ret > 0)
        {
            // Process all pending messages
            nn_dev_message_t *msg;
            while ((msg = nn_mq_receive(mq)) != NULL)
            {
                bgp_handle_message(msg);
                nn_message_free(msg);
            }
        }
        
        // BGP protocol processing would go here
    }

    printf("[bgp] Worker thread exiting\n");
    return NULL;
}

// BGP command callback
void cmd_bgp(uint32_t client_fd, const char *args)
{
    (void)args;
    send_message(client_fd, "\r\nEntering BGP configuration mode...\r\n");
    send_message(client_fd, "BGP module loaded successfully\r\n");
}

// Module initialization
static int32_t bgp_module_init(void)
{
    // Create message queue
    g_bgp_mq = nn_mq_create();
    if (!g_bgp_mq)
    {
        fprintf(stderr, "[bgp] Failed to create message queue\n");
        return -1;
    }
    
    // Register message queue with module
    nn_dev_module_t *self = nn_get_module("bgp");
    if (self)
    {
        nn_module_set_mq(self, g_bgp_mq);
    }
    
    g_bgp_running = 1;

    if (pthread_create(&g_bgp_thread, NULL, bgp_worker_thread, g_bgp_mq) != 0)
    {
        perror("[bgp] Failed to create worker thread");
        nn_mq_destroy(g_bgp_mq);
        g_bgp_mq = NULL;
        return -1;
    }

    printf("[bgp] BGP module initialized (eventfd=%d)\n", nn_mq_get_eventfd(g_bgp_mq));
    return 0;
}

// Module cleanup
static void bgp_module_cleanup(void)
{
    printf("[bgp] Shutting down BGP module...\n");

    g_bgp_running = 0;
    pthread_join(g_bgp_thread, NULL);

    printf("[bgp] BGP module cleanup complete\n");
}

// Register BGP module using constructor attribute
static void __attribute__((constructor)) register_bgp_module(void)
{
    nn_dev_register_module(NN_MODULE_ID_CFG, "nn_bgp", bgp_module_init, bgp_module_cleanup);
}
