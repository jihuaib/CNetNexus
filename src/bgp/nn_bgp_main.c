#include "nn_bgp_main.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "nn_bgp_cli.h"
#include "nn_cfg.h"
#include "nn_dev.h"
#include "nn_errcode.h"
#include "nn_path_utils.h"

#define BGP_MAX_EPOLL_EVENTS 16

nn_bgp_local_t *g_nn_bgp_local = NULL;

// Process all pending messages from queue
static void bgp_process_messages(nn_bgp_local_t *ctx)
{
    nn_dev_message_t *msg;

    // Clear eventfd
    uint64_t val;
    read(ctx->event_fd, &val, sizeof(val));

    // Process all pending messages
    while ((msg = nn_dev_mq_receive(ctx->event_fd, ctx->mq)) != NULL)
    {
        // Handle different message types
        switch (msg->msg_type)
        {
            case NN_CFG_MSG_TYPE_CLI:
                // CLI command from cfg module
                printf("[bgp] Received CLI command message (%zu bytes)\n", msg->data_len);
                nn_bgp_cli_handle_message(msg);
                break;

            default:
                printf("[bgp] Received unknown message type: 0x%08X\n", msg->msg_type);
                break;
        }

        nn_dev_message_free(msg);
    }
}

// BGP worker thread with epoll
static void *bgp_worker_thread(void *arg)
{
    (void)arg;
    struct epoll_event events[BGP_MAX_EPOLL_EVENTS];

    printf("[bgp] Worker thread started (epoll_fd=%d, event_fd=%d)\n", g_nn_bgp_local->epoll_fd,
           g_nn_bgp_local->event_fd);

    while (g_nn_bgp_local->running && !nn_dev_shutdown_requested())
    {
        // Wait for events with 1 second timeout
        int nfds = epoll_wait(g_nn_bgp_local->epoll_fd, events, BGP_MAX_EPOLL_EVENTS, 1000);

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
            if (events[i].data.fd == g_nn_bgp_local->event_fd)
            {
                // Message queue has data
                bgp_process_messages(g_nn_bgp_local);
            }
            // Add other fd handlers here (e.g., BGP peer sockets)
        }
    }

    printf("[bgp] Worker thread exiting\n");
    return NULL;
}

static int nn_bgp_init_local()
{
    g_nn_bgp_local = g_malloc0(sizeof(nn_bgp_local_t));
    g_nn_bgp_local->epoll_fd = NN_DEV_INVALID_FD;
    g_nn_bgp_local->event_fd = NN_DEV_INVALID_FD;
    g_nn_bgp_local->worker_thread = 0;
    g_nn_bgp_local->running = 0;

    // Create message queue
    nn_dev_module_mq_t *mq = nn_dev_mq_create();
    if (mq == NULL)
    {
        fprintf(stderr, "[bgp] Failed to create message queue\n");
        return NN_ERRCODE_FAIL;
    }
    g_nn_bgp_local->mq = mq;

    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0)
    {
        fprintf(stderr, "[bgp] Failed to create event fd\n");
        return NN_ERRCODE_FAIL;
    }
    g_nn_bgp_local->event_fd = event_fd;

    // Create epoll instance
    g_nn_bgp_local->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_nn_bgp_local->epoll_fd < 0)
    {
        perror("[bgp] Failed to create epoll");
        return NN_ERRCODE_FAIL;
    }

    // Add eventfd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_nn_bgp_local->event_fd;
    if (epoll_ctl(g_nn_bgp_local->epoll_fd, EPOLL_CTL_ADD, g_nn_bgp_local->event_fd, &ev) < 0)
    {
        perror("[bgp] Failed to add eventfd to epoll");
        return NN_ERRCODE_FAIL;
    }

    // Register with pub/sub system
    int ret = nn_dev_pubsub_register(NN_DEV_MODULE_ID_BGP, g_nn_bgp_local->event_fd, g_nn_bgp_local->mq);
    if (ret != NN_ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[bgp] Failed to register with pub/sub system\n");
        return NN_ERRCODE_FAIL;
    }

    // Subscribe to events from CFG module
    nn_dev_pubsub_subscribe(NN_DEV_MODULE_ID_BGP, NN_DEV_MODULE_ID_CFG, NN_DEV_EVENT_CFG);

    g_nn_bgp_local->running = 1;

    if (pthread_create(&g_nn_bgp_local->worker_thread, NULL, bgp_worker_thread, NULL) != 0)
    {
        perror("[bgp] Failed to create worker thread");
        return NN_ERRCODE_FAIL;
    }

    return NN_ERRCODE_SUCCESS;
}

static void nn_bgp_cleanup_local()
{
    if (g_nn_bgp_local == NULL)
    {
        return;
    }

    printf("[bgp] Shutting down BGP module...\n");

    g_nn_bgp_local->running = 0;

    // Unregister from pub/sub
    nn_dev_pubsub_unregister(NN_DEV_MODULE_ID_BGP);

    // Wait for thread to exit
    if (g_nn_bgp_local->worker_thread != 0)
    {
        pthread_join(g_nn_bgp_local->worker_thread, NULL);
    }

    // Close epoll fd
    if (g_nn_bgp_local->epoll_fd >= 0)
    {
        close(g_nn_bgp_local->epoll_fd);
    }

    if (g_nn_bgp_local->event_fd >= 0)
    {
        close(g_nn_bgp_local->event_fd);
    }

    if (g_nn_bgp_local->mq)
    {
        nn_dev_mq_destroy(g_nn_bgp_local->mq);
    }

    g_free(g_nn_bgp_local);
    g_nn_bgp_local = NULL;

    printf("[bgp] BGP module cleanup complete\n");
}

// Module initialization
static int32_t bgp_module_init()
{
    int ret = nn_bgp_init_local();
    if (ret != NN_ERRCODE_SUCCESS)
    {
        nn_bgp_cleanup_local();
        return NN_ERRCODE_FAIL;
    }

    printf("[bgp] BGP module initialized (epoll_fd=%d, event_fd=%d)\n", g_nn_bgp_local->epoll_fd,
           g_nn_bgp_local->event_fd);
    return NN_ERRCODE_SUCCESS;
}

// Module cleanup
static void bgp_module_cleanup(void)
{
    nn_bgp_cleanup_local();
}

// Register BGP module using constructor attribute
static void __attribute__((constructor)) register_bgp_module(void)
{
    nn_dev_register_module(NN_DEV_MODULE_ID_BGP, "bgp", bgp_module_init, bgp_module_cleanup);

    char bgp_xml_path[256];
    if (nn_resolve_xml_path("bgp", bgp_xml_path, sizeof(bgp_xml_path)) == 0)
    {
        nn_cfg_register_module_xml(NN_DEV_MODULE_ID_BGP, bgp_xml_path);
    }
    else
    {
        fprintf(stderr, "[bgp] Warning: Could not resolve XML path for bgp module\n");
    }
}
