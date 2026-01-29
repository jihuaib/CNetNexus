#ifndef NN_BGP_MAIN_H
#define NN_BGP_MAIN_H

#include <pthread.h>

#include "nn_dev.h"

typedef struct nn_bgp_local
{
    int epoll_fd;
    int event_fd;
    nn_dev_module_mq_t *mq;
    pthread_t worker_thread;
    volatile int running;
} nn_bgp_local_t;

extern nn_bgp_local_t *g_nn_bgp_local;

#endif // NN_BGP_MAIN_H
