#ifndef TUNNEL_WORKER_H
#define TUNNEL_WORKER_H

#include <glib.h>
#include <pthread.h>

#include "dev.h"
#include "tunnel_rib.h"

typedef enum tunnel_worker_cmd_type
{
    TUNNEL_WORKER_CMD_CANDIDATE_ADD = 1,
    TUNNEL_WORKER_CMD_CANDIDATE_DEL = 2,
    TUNNEL_WORKER_CMD_RESOLVE_REGISTER = 3,
    TUNNEL_WORKER_CMD_RESOLVE_UNREGISTER = 4,
    TUNNEL_WORKER_CMD_LABEL_ALLOC = 5,
    TUNNEL_WORKER_CMD_LABEL_RELEASE = 6,
    TUNNEL_WORKER_CMD_RESOLVE_QUERY = 7,
    TUNNEL_WORKER_CMD_CLI_SHOW = 8,
    TUNNEL_WORKER_CMD_SHUTDOWN = 9,
} tunnel_worker_cmd_type_t;

typedef struct tunnel_work_local
{
    tunnel_rib_t *rib;
    int epoll_fd;
    volatile int running;
    pthread_t thread;
    int cmd_eventfd;
    GAsyncQueue *cmd_queue;
} tunnel_work_local_t;

extern tunnel_work_local_t *g_tunnel_work_local;

int tunnel_worker_prepare(void);
int tunnel_worker_launch(void);
int tunnel_worker_post(tunnel_worker_cmd_type_t type, dev_ipc_message_t *msg);
int tunnel_worker_post_show_cli(dev_ipc_message_t *msg);
void tunnel_worker_shutdown(void);

#endif /* TUNNEL_WORKER_H */
