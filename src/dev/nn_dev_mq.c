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
nn_dev_message_t *nn_dev_message_create_inner(uint32_t msg_type, uint32_t sender_id, uint32_t request_id, void *data,
                                              size_t data_len, void (*free_fn)(void *))
{
    nn_dev_message_t *msg = g_malloc0(sizeof(nn_dev_message_t));

    msg->msg_type = msg_type;
    msg->sender_id = sender_id;
    msg->request_id = request_id;
    msg->data = data;
    msg->data_len = data_len;
    msg->free_fn = free_fn;

    return msg;
}

// Free a message
void nn_dev_message_free_inner(nn_dev_message_t *msg)
{
    if (!msg)
    {
        return;
    }

    if (msg->data && msg->free_fn)
    {
        msg->free_fn(msg->data);
    }

    g_free(msg);
}

// Create module message queue
nn_dev_module_mq_t *nn_dev_mq_create_inner()
{
    nn_dev_module_mq_t *mq = g_malloc0(sizeof(nn_dev_module_mq_t));

    // Create message queue
    mq->message_queue = g_queue_new();
    g_mutex_init(&mq->queue_mutex);

    return mq;
}

// Destroy module message queue
void nn_dev_mq_destroy_inner(nn_dev_module_mq_t *mq)
{
    if (mq == NULL)
    {
        return;
    }

    // Clear and g_free message queue
    g_mutex_lock(&mq->queue_mutex);

    while (!g_queue_is_empty(mq->message_queue))
    {
        nn_dev_message_t *msg = g_queue_pop_head(mq->message_queue);
        nn_dev_message_free(msg);
    }

    g_queue_free(mq->message_queue);
    g_mutex_unlock(&mq->queue_mutex);

    g_mutex_clear(&mq->queue_mutex);
    g_free(mq);
}

// Send message to module queue (thread-safe)
int nn_nn_mq_send_inner(int event_fd, nn_dev_module_mq_t *mq, nn_dev_message_t *msg)
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
    if (write(event_fd, &val, sizeof(val)) != sizeof(val))
    {
        perror("eventfd write");
        return NN_ERRCODE_FAIL;
    }

    return NN_ERRCODE_SUCCESS;
}

// Receive message from queue (non-blocking, thread-safe)
nn_dev_message_t *nn_dev_mq_receive_inner(int event_fd, nn_dev_module_mq_t *mq)
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
            read(event_fd, &val, sizeof(val));
        }
    }

    g_mutex_unlock(&mq->queue_mutex);

    return msg;
}