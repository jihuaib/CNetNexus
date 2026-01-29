#ifndef NN_DEV_MAIN_H
#define NN_DEV_MAIN_H

#include <pthread.h>

#include "nn_dev.h"

typedef struct nn_dev_local
{
    int epoll_fd;
    int event_fd;
    nn_dev_module_mq_t *mq;
    pthread_t worker_thread;
    volatile int running;

    // Registered modules: module_id -> nn_dev_pubsub_subscriber_t*
    GHashTable *registered_modules;
    // Unicast subscriptions: uint64_t key (publisher_id << 32 | event_id) -> GList of nn_dev_pubsub_subscriber_t*
    GHashTable *unicast_subss;
    // Multicast groups: group_id -> nn_dev_pubsub_group_t*
    GHashTable *multicast_groups;
    // Global mutex for thread-safe access
    GMutex pubsub_mutex;
} nn_dev_local_t;

extern nn_dev_local_t *g_nn_dev_local;

#endif // NN_DEV_MAIN_H