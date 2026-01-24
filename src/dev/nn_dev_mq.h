//
// Created by jhb on 1/24/26.
//

#ifndef NETNEXUS_NN_DEV_MQ_H
#define NETNEXUS_NN_DEV_MQ_H

#include <glib.h>
#include <stdint.h>

// Message structure
typedef struct nn_message
{
    char *type;              // Message type
    void *data;              // Message data
    size_t data_len;         // Data length
    void (*free_fn)(void *); // Data g_free function
} nn_dev_message_t;

// Module message queue structure
typedef struct nn_module_mq
{
    int eventfd;           // Event notification fd
    GQueue *message_queue; // Message queue (thread-safe with mutex)
    GMutex queue_mutex;    // Queue mutex
} nn_dev_module_mq_t;

#endif // NETNEXUS_NN_DEV_MQ_H