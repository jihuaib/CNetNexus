/**
 * @file   fib_worker.h
 * @brief  FIB worker 线程：epoll 事件循环与业务命令队列
 */
#ifndef FIB_WORKER_H
#define FIB_WORKER_H

#include <glib.h>
#include <pthread.h>

#include "dev.h"
#include "fib_rib.h"

typedef enum fib_worker_cmd_type
{
    FIB_WORKER_CMD_ROUTE_UPSERT = 1,
    FIB_WORKER_CMD_ROUTE_DELETE = 2,
    FIB_WORKER_CMD_TUNNEL_UPSERT = 3,
    FIB_WORKER_CMD_TUNNEL_DELETE = 4,
    FIB_WORKER_CMD_SHOW_CLI = 5,
    FIB_WORKER_CMD_SHUTDOWN = 6,
} fib_worker_cmd_type_t;

typedef struct fib_work_local
{
    fib_rib_t *rib;
    int epoll_fd;
    volatile int running;
    pthread_t thread;
    int cmd_eventfd;
    GAsyncQueue *cmd_queue;
} fib_work_local_t;

extern fib_work_local_t *g_fib_work_local;

int fib_worker_prepare(void);
int fib_worker_launch(void);
int fib_worker_post(fib_worker_cmd_type_t type, dev_ipc_message_t *msg);
void fib_worker_shutdown(void);

#endif /* FIB_WORKER_H */
