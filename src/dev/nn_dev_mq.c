//
// Created by jhb on 1/24/26.
//

#include "nn_dev_mq.h"

#include <poll.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "nn_dev_module.h"
#include "nn_errcode.h"

// Create a message
nn_dev_message_t *nn_message_create(const char *type, void *data, size_t data_len, void (*free_fn)(void *))
{
    nn_dev_message_t *msg = g_malloc(sizeof(nn_dev_message_t));

    msg->type = g_strdup(type);
    msg->data = data;
    msg->data_len = data_len;
    msg->free_fn = free_fn;

    return msg;
}

// Free a message
void nn_message_free(nn_dev_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    g_free(msg->type);

    if (msg->data && msg->free_fn)
    {
        msg->free_fn(msg->data);
    }

    g_free(msg);
}

// Create module message queue
int nn_mq_create(void *data, nn_dev_mq_info_t *info)
{
    if (data == NULL)
    {
        return NN_ERRCODE_FAIL;
    }

    nn_dev_module_t *module = (nn_dev_module_t *)data;

    nn_dev_module_mq_t *mq = g_malloc(sizeof(nn_dev_module_mq_t));

    // Create eventfd for notifications
    mq->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (mq->eventfd < 0)
    {
        perror("eventfd");
        g_free(mq);
        return NN_ERRCODE_FAIL;
    }

    // Create message queue
    mq->message_queue = g_queue_new();
    g_mutex_init(&mq->queue_mutex);

    module->mq = mq;

    info->fd = mq->eventfd;
    info->data = (void *)mq;

    return NN_ERRCODE_SUCCESS;
}

// Destroy module message queue
void nn_mq_destroy(nn_dev_mq_info_t *info)
{
    if ((info == NULL) || (info->data))
    {
        return;
    }

    nn_dev_module_mq_t *mq = (nn_dev_module_mq_t *)info->data;

    // Close eventfd
    if (mq->eventfd >= 0)
    {
        close(mq->eventfd);
    }

    // Clear and g_free message queue
    g_mutex_lock(&mq->queue_mutex);

    while (!g_queue_is_empty(mq->message_queue))
    {
        nn_dev_message_t *msg = g_queue_pop_head(mq->message_queue);
        nn_message_free(msg);
    }

    g_queue_free(mq->message_queue);
    g_mutex_unlock(&mq->queue_mutex);

    g_mutex_clear(&mq->queue_mutex);
    g_free(mq);
}

// Send message to module queue (thread-safe)
int nn_mq_send(nn_dev_module_mq_t *mq, nn_dev_message_t *msg)
{
    if (!mq || !msg)
    {
        return NN_ERRCODE_FAIL;
    }

    // Add message to queue
    g_mutex_lock(&mq->queue_mutex);
    g_queue_push_tail(mq->message_queue, msg);
    g_mutex_unlock(&mq->queue_mutex);

    // Notify via eventfd
    uint64_t val = 1;
    if (write(mq->eventfd, &val, sizeof(val)) != sizeof(val))
    {
        perror("eventfd write");
        return NN_ERRCODE_FAIL;
    }

    return NN_ERRCODE_SUCCESS;
}

// Receive message from queue (non-blocking, thread-safe)
nn_dev_message_t *nn_mq_receive(nn_dev_module_mq_t *mq)
{
    if (!mq)
    {
        return NULL;
    }

    g_mutex_lock(&mq->queue_mutex);

    nn_dev_message_t *msg = NULL;
    if (!g_queue_is_empty(mq->message_queue))
    {
        msg = g_queue_pop_head(mq->message_queue);

        // Clear eventfd if queue is now empty
        if (g_queue_is_empty(mq->message_queue))
        {
            uint64_t val;
            read(mq->eventfd, &val, sizeof(val));
        }
    }

    g_mutex_unlock(&mq->queue_mutex);

    return msg;
}

// Wait for message on eventfd (blocking with timeout)
int nn_mq_wait(nn_dev_module_mq_t *mq, int timeout_ms)
{
    if (!mq || mq->eventfd < 0)
    {
        return NN_ERRCODE_FAIL;
    }

    struct pollfd pfd;
    pfd.fd = mq->eventfd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);

    if (ret > 0 && (pfd.revents & POLLIN))
    {
        return 1; // Message available
    }
    if (ret == NN_ERRCODE_SUCCESS)
    {
        return NN_ERRCODE_SUCCESS; // Timeout
    }
    else
    {
        return NN_ERRCODE_FAIL; // Error
    }
}

// Get eventfd for external polling
int nn_mq_get_eventfd(nn_dev_module_mq_t *mq)
{
    return mq ? mq->eventfd : -1;
}