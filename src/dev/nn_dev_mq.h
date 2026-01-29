#ifndef NN_DEV_MQ_H
#define NN_DEV_MQ_H

#include <glib.h>
#include <stdint.h>

#include "nn_dev.h"

// Module message queue structure
struct nn_dev_module_mq
{
    GQueue *message_queue; // Message queue (thread-safe with mutex)
    GMutex queue_mutex;    // Queue mutex
};

// Internal Message Queue APIs
nn_dev_message_t *nn_dev_message_create_inner(uint32_t msg_type, uint32_t sender_id, uint32_t request_id, void *data,
                                              size_t data_len, void (*free_fn)(void *));

void nn_dev_message_free_inner(nn_dev_message_t *msg);

nn_dev_module_mq_t *nn_dev_mq_create_inner();

void nn_dev_mq_destroy_inner(nn_dev_module_mq_t *mq);

int nn_nn_mq_send_inner(int event_fd, nn_dev_module_mq_t *mq, nn_dev_message_t *msg);

nn_dev_message_t *nn_dev_mq_receive_inner(int event_fd, nn_dev_module_mq_t *mq);

#endif // NN_DEV_MQ_H