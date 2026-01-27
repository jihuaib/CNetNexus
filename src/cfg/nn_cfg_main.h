#ifndef NN_CFG_MAIN_H
#define NN_CFG_MAIN_H

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nn_cli_handler.h"
#include "nn_cli_view.h"
#include "nn_dev.h"

typedef struct nn_cfg_local
{
    nn_cli_view_tree_t view_tree;
    nn_cli_global_history_t global_history;
    pthread_mutex_t history_mutex;
    int epoll_fd;           // epoll file descriptor
    int event_fd;           // eventfd for message notification
    nn_dev_module_mq_t *mq; // message queue
    int listen_sock;
    pthread_t worker_thread;
    GHashTable *sessions; // Registry: fd -> nn_cli_session_t*
} nn_cfg_local_t;

extern nn_cfg_local_t *g_nn_cfg_local;

#endif // NN_CFG_MAIN_H