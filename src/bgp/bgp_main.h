/**
 * @file   bgp_main.h
 * @brief  BGP 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef BGP_MAIN_H
#define BGP_MAIN_H

#include <glib.h>
#include <pthread.h>

#include "bgp_conn.h"
#include "bgp_protocol.h"
#include "cli.h"
#include "dev.h"

/**
 * @brief BGP 模块本地状态
 *
 * epoll 事件通过 data.ptr 直接携带 bgp_conn_t*（连接 fd）或 &bgp_listen_tag（listener），
 * 无需额外的 fd → conn 映射表。
 */
typedef struct bgp_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    cli_chunk_stream_t show_stream; /**< CLI show 命令分片输出状态 */
    bgp_protocol_t *protocol;       /**< BGP 协议结构（bgp 使能后非 NULL） */

    /* BGP TCP worker */
    int epoll_fd;            /**< BGP worker epoll fd */
    int running;             /**< worker 线程运行标志 */
    pthread_t worker_thread; /**< BGP worker 线程句柄 */

    int listen_fd; /**< 全局 0.0.0.0:179 listen socket fd，-1 表示未监听 */

    /* IPC worker -> BGP worker 命令投递（eventfd + queue） */
    int cmd_eventfd;        /**< 命令唤醒 eventfd，-1 表示未创建 */
    GAsyncQueue *cmd_queue; /**< 队列元素：bgp_worker_cmd_t*（定义在 work/bgp_worker.c） */
} bgp_local_t;

extern bgp_local_t *g_bgp_local;

/**
 * @brief 获取 BGP 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *bgp_local_ipc_ctx(void)
{
    return g_bgp_local->dev_ipc_ctx;
}

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief BGP 模块初始化（由 bgp_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int bgp_module_init(void);

#endif /* BGP_MAIN_H */
