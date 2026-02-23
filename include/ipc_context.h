/**
 * @file   ipc_context.h
 * @brief  IPC 上下文内部定义头文件
 * @author jhb
 * @date   2026/02/02
 */

#ifndef IPC_CONTEXT_H
#define IPC_CONTEXT_H

#include <pthread.h>
#include <stdint.h>

#include "ipc.h"
#include "ipc_connection.h"
#include "ipc_query.h"

/** IPC 上下文完整定义 */
struct ipc_context
{
    /* 本模块信息 */
    uint32_t module_id;             /**< 本模块 ID */
    char name[IPC_MODULE_NAME_MAX]; /**< 模块名称 */

    /* 消息处理 */
    ipc_msg_handler_fn msg_handler; /**< 消息处理回调 */

    /* 连接 */
    ipc_connection_t *connections[IPC_MAX_CONNECTIONS]; /**< 连接数组 */
    int num_connections;                                /**< 连接数 */
    pthread_mutex_t comutex;                            /**< 连接锁 */

    /* 监听 */
    int listen_fd; /**< 监听 socket（本模块 IPC 端口） */

    /* IO 线程 */
    int epoll_fd;                    /**< epoll 文件描述符 */
    pthread_t io_thread;             /**< IO 线程 */
    volatile int running;            /**< 运行标志 */
    volatile int shutdown_requested; /**< 关闭请求标志 */

    /* 同步查询 */
    ipc_query_mgr_t *query_mgr; /**< 查询管理器 */

    /* 模块名称表（Phase 1 由 DEV 下发） */
    ipc_module_table_t *module_table; /**< 模块名称表 */
};

#endif // IPC_CONTEXT_H
