/**
 * @file   cli_main.h
 * @brief  CFG 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef CLI_MAIN_H
#define CLI_MAIN_H

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

typedef struct cli_local
{
    cli_view_tree_t view_tree;
    cli_global_history_t global_history;
    pthread_mutex_t history_mutex;
    volatile int running; /**< Telnet 工作线程运行标志 */
    int epoll_fd;         /**< epoll 文件描述符（Telnet 用） */
    int listen_sock;      /**< Telnet 监听 socket */
    pthread_t worker_thread;
    GHashTable *sessions;              /**< 注册表: fd -> cli_session_t* */
    dev_ipc_context_t *dev_ipc_ctx;    /**< IPC 上下文 */
    char sysname[CLI_SYSNAME_MAX_LEN]; /**< 当前系统名（默认 "NetNexus"，由 DEV 通过 IPC 推送） */
} cli_local_t;

extern cli_local_t *g_cli_local;

/**
 * @brief 获取 CLI 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *cli_local_ipc_ctx(void)
{
    return g_cli_local->dev_ipc_ctx;
}

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void cli_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief CFG 本地状态初始化（由 cli_module_init 调用）
 * @param ctx IPC 上下文
 */
void cli_init_local(dev_ipc_context_t *ctx);

/**
 * @brief CFG 模块初始化（由 cli_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int cli_module_init(void);

/**
 * @brief CLI 模块清理（由 cli_proc.c main() 退出前调用）
 */
void cli_module_cleanup(void);

#endif // CLI_MAIN_H
