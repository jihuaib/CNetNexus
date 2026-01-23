#include "nn_message_queue.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

// Create a message
nn_message_t *nn_message_create(const char *type, void *data, size_t data_len, void (*free_fn)(void *))
{
    nn_message_t *msg = g_malloc(sizeof(nn_message_t));
    if (!msg)
    {
        return NULL;
    }

    msg->type = g_strdup(type);
    msg->data = data;
    msg->data_len = data_len;
    msg->free_fn = free_fn;

    return msg;
}

// Free a message
void nn_message_free(nn_message_t *msg)
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
nn_module_mq_t *nn_mq_create(void)
{
    nn_module_mq_t *mq = g_malloc(sizeof(nn_module_mq_t));
    if (!mq)
    {
        return NULL;
    }

    // Create eventfd for notifications
    mq->eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (mq->eventfd < 0)
    {
        perror("eventfd");
        g_free(mq);
        return NULL;
    }

    // Create message queue
    mq->message_queue = g_queue_new();
    g_mutex_init(&mq->queue_mutex);

    return mq;
}

// Destroy module message queue
void nn_mq_destroy(nn_module_mq_t *mq)
{
    if (!mq)
    {
        return;
    }

    // Close eventfd
    if (mq->eventfd >= 0)
    {
        close(mq->eventfd);
    }

    // Clear and free message queue
    g_mutex_lock(&mq->queue_mutex);

    while (!g_queue_is_empty(mq->message_queue))
    {
        nn_message_t *msg = g_queue_pop_head(mq->message_queue);
        nn_message_free(msg);
    }

    g_queue_free(mq->message_queue);
    g_mutex_unlock(&mq->queue_mutex);

    g_mutex_clear(&mq->queue_mutex);
    g_free(mq);
}

// Send message to module queue (thread-safe)
int nn_mq_send(nn_module_mq_t *mq, nn_message_t *msg)
{
    if (!mq || !msg)
    {
        return -1;
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
        return -1;
    }

    return 0;
}

// Receive message from queue (non-blocking, thread-safe)
nn_message_t *nn_mq_receive(nn_module_mq_t *mq)
{
    if (!mq)
    {
        return NULL;
    }

    g_mutex_lock(&mq->queue_mutex);

    nn_message_t *msg = NULL;
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
int nn_mq_wait(nn_module_mq_t *mq, int timeout_ms)
{
    if (!mq || mq->eventfd < 0)
    {
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = mq->eventfd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);

    if (ret > 0 && (pfd.revents & POLLIN))
    {
        return 1; // Message available
    }
    else if (ret == 0)
    {
        return 0; // Timeout
    }
    else
    {
        return -1; // Error
    }
}

// Get eventfd for external polling
int nn_mq_get_eventfd(nn_module_mq_t *mq)
{
    return mq ? mq->eventfd : -1;
}
