#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "nn_cfg.h"
#include "nn_cli_handler.h"
#include "nn_dev.h"
#include "nn_errcode.h"

// BGP module state
static pthread_t g_bgp_thread;
static int g_bgp_running = 0;

// Handle incoming message
static void bgp_handle_message(nn_dev_message_t *msg)
{
    printf("[bgp] Received message: type=%s, len=%zu\n", msg->type, msg->data_len);

    if (strcmp(msg->type, "config_update") == NN_ERRCODE_SUCCESS)
    {
        printf("[bgp] Config update: %s\n", (char *)msg->data);
    }
    else if (strcmp(msg->type, "command") == NN_ERRCODE_SUCCESS)
    {
        printf("[bgp] Executing command: %s\n", (char *)msg->data);
        // Here we would parse and execute the BGP command
    }
    else if (strcmp(msg->type, "shutdown") == NN_ERRCODE_SUCCESS)
    {
        printf("[bgp] Shutdown request received\n");
        g_bgp_running = 0;
    }
}

// BGP worker thread
static void *bgp_worker_thread(void *arg)
{
    int fd = *(int *)arg;

    printf("[bgp] Worker thread started\n");

    while (g_bgp_running && !nn_shutdown_requested())
    {
        // Wait for messages with 1 second timeout
        // int ret = nn_mq_wait(mq, 1000);

        // if (ret > 0)
        // {
        //     // Process all pending messages
        //     nn_dev_message_t *msg;
        //     while ((msg = nn_mq_receive(mq)) != NULL)
        //     {
        //         bgp_handle_message(msg);
        //         nn_message_free(msg);
        //     }
        // }

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
static int32_t bgp_module_init(void *data)
{
    nn_dev_mq_info_t info;

    memset(&info, 0, sizeof(info));

    // Create message queue
    int ret = nn_mq_create(data, &info);
    if (ret != NN_ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[bgp] Failed to create message queue\n");
        return NN_ERRCODE_FAIL;
    }

    g_bgp_running = 1;

    if (pthread_create(&g_bgp_thread, NULL, bgp_worker_thread, &info.fd) != NN_ERRCODE_SUCCESS)
    {
        perror("[bgp] Failed to create worker thread");
        nn_mq_destroy(&info);
        return NN_ERRCODE_FAIL;
    }

    printf("[bgp] BGP module initialized (eventfd=%d)\n", info.fd);
    return NN_ERRCODE_SUCCESS;
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
    nn_dev_register_module(NN_MODULE_ID_BGP, "nn_bgp", bgp_module_init, bgp_module_cleanup);
    nn_cfg_register_module_xml(NN_MODULE_ID_BGP, "../../src/bgp/commands.xml");
}
