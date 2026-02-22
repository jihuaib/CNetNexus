/**
 * @file   cfg_main.h
 * @brief  CFG 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CFG_MAIN_H
#define CFG_MAIN_H

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

#include "cli_handler.h"
#include "cli_view.h"
#include "dev.h"
#include "ipc.h"

typedef struct cfg_local
{
    cli_view_tree_t view_tree;
    cli_global_history_t global_history;
    pthread_mutex_t history_mutex;
    int epoll_fd;    // epoll file descriptor（Telnet 用）
    int listen_sock; // Telnet 监听 socket
    pthread_t worker_thread;
    GHashTable *sessions;   // Registry: fd -> cli_session_t*
    ipc_context_t *ipc_ctx; // IPC 上下文
} cfg_local_t;

extern cfg_local_t *g_cfg_local;

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void cfg_msg_handler(ipc_context_t *ctx, ipc_message_t *msg);

/**
 * @brief CFG 本地状态初始化（从 constructor 调用）
 * @param ctx IPC 上下文
 */
void cfg_init_local(ipc_context_t *ctx);

#endif // CFG_MAIN_H
