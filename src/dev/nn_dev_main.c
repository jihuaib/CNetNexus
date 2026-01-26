#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "nn_cfg.h"
#include "nn_dev.h"
#include "nn_dev_module.h"
#include "nn_dev_pubsub.h"
#include "nn_errcode.h"
#include "nn_path_utils.h"

// BGP module context
typedef struct nn_dev_context
{
    int epoll_fd;           // epoll file descriptor
    int event_fd;           // eventfd for message notification
    nn_dev_module_mq_t *mq; // message queue
} nn_dev_context_t;

static nn_dev_context_t g_nn_dev_ctx;

// Module initialization
static int32_t dev_module_init()
{
    memset(&g_nn_dev_ctx, 0, sizeof(g_nn_dev_ctx));

    nn_dev_pubsub_init();

    // Create message queue
    nn_dev_module_mq_t *mq = nn_dev_mq_create();
    if (mq == NULL)
    {
        nn_dev_pubsub_cleanup();
        fprintf(stderr, "[dev] Failed to create message queue\n");
        return NN_ERRCODE_FAIL;
    }

    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0)
    {
        nn_dev_pubsub_cleanup();
        nn_dev_mq_destroy(mq);
        fprintf(stderr, "[dev] Failed to create event fd\n");
        return NN_ERRCODE_FAIL;
    }

    g_nn_dev_ctx.event_fd = event_fd;
    g_nn_dev_ctx.mq = mq;

    // Create epoll instance
    g_nn_dev_ctx.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_nn_dev_ctx.epoll_fd < 0)
    {
        nn_dev_pubsub_cleanup();
        close(g_nn_dev_ctx.epoll_fd);
        nn_dev_mq_destroy(mq);
        perror("[dev] Failed to create epoll");
        return NN_ERRCODE_FAIL;
    }

    // Add eventfd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_nn_dev_ctx.event_fd;
    if (epoll_ctl(g_nn_dev_ctx.epoll_fd, EPOLL_CTL_ADD, g_nn_dev_ctx.event_fd, &ev) < 0)
    {
        nn_dev_pubsub_cleanup();
        close(g_nn_dev_ctx.epoll_fd);
        nn_dev_mq_destroy(mq);
        perror("[dev] Failed to add eventfd to epoll");
        return NN_ERRCODE_FAIL;
    }

    // Register with pub/sub system
    int ret = nn_dev_pubsub_register(NN_DEV_MODULE_ID_DEV, g_nn_dev_ctx.event_fd, g_nn_dev_ctx.mq);
    if (ret != NN_ERRCODE_SUCCESS)
    {
        nn_dev_pubsub_cleanup();
        close(g_nn_dev_ctx.epoll_fd);
        nn_dev_mq_destroy(mq);
        fprintf(stderr, "[dev] Failed to register with pub/sub system\n");
        return NN_ERRCODE_FAIL;
    }

    // Subscribe to events from DEV module
    nn_dev_pubsub_subscribe(NN_DEV_MODULE_ID_DEV, NN_DEV_MODULE_ID_CFG, NN_DEV_EVENT_CFG);

    printf("[dev] DEV module initialized (epoll_fd=%d, event_fd=%d)\n", g_nn_dev_ctx.epoll_fd, g_nn_dev_ctx.event_fd);
    return NN_ERRCODE_SUCCESS;
}

// Module cleanup
static void dev_module_cleanup(void)
{
    printf("[dev] Dev module cleanup\n");
}

// Register dev module using constructor attribute
static void __attribute__((constructor)) register_dev_module(void)
{
    nn_dev_register_module(NN_DEV_MODULE_ID_DEV, "nn_dev", dev_module_init, dev_module_cleanup);

    char dev_xml_path[256];
    if (nn_resolve_xml_path("dev", dev_xml_path, sizeof(dev_xml_path)) == 0)
    {
        nn_cfg_register_module_xml(NN_DEV_MODULE_ID_DEV, dev_xml_path);
    }
    else
    {
        fprintf(stderr, "[dev] Warning: Could not resolve XML path for dev module\n");
    }
}
