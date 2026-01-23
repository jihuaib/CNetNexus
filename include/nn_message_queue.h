#ifndef NN_MESSAGE_QUEUE_H
#define NN_MESSAGE_QUEUE_H

#include <glib.h>
#include <stddef.h>
#include <stdint.h>

// Message structure
typedef struct nn_message
{
    char *type;                    // Message type
    void *data;                    // Message data
    size_t data_len;               // Data length
    void (*free_fn)(void *);       // Data free function
} nn_message_t;

// Module message queue structure
typedef struct nn_module_mq
{
    int eventfd;                   // Event notification fd
    GQueue *message_queue;         // Message queue (thread-safe with mutex)
    GMutex queue_mutex;            // Queue mutex
} nn_module_mq_t;

// Create a message
nn_message_t *nn_message_create(const char *type, void *data, size_t data_len, void (*free_fn)(void *));

// Free a message
void nn_message_free(nn_message_t *msg);

// Create module message queue
nn_module_mq_t *nn_mq_create(void);

// Destroy module message queue
void nn_mq_destroy(nn_module_mq_t *mq);

// Send message to module queue (thread-safe)
int nn_mq_send(nn_module_mq_t *mq, nn_message_t *msg);

// Receive message from queue (non-blocking, thread-safe)
nn_message_t *nn_mq_receive(nn_module_mq_t *mq);

// Wait for message on eventfd (blocking with timeout)
// Returns: >0 if message available, 0 if timeout, <0 on error
int nn_mq_wait(nn_module_mq_t *mq, int timeout_ms);

// Get eventfd for external polling
int nn_mq_get_eventfd(nn_module_mq_t *mq);

#endif // NN_MESSAGE_QUEUE_H
