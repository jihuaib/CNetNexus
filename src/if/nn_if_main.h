/**
 * @file   nn_if_main.h
 * @brief  接口模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef NN_IF_MAIN_H
#define NN_IF_MAIN_H

#include <pthread.h>

#include "nn_dev.h"

typedef struct
{
    nn_dev_module_mq_t *mq;
    int epoll_fd;
    int event_fd;
    pthread_t worker_thread;
    int running;
} nn_if_local_t;

extern nn_if_local_t *g_nn_if_local;

#endif // NN_IF_MAIN_H
