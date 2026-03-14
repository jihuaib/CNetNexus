/**
 * @file   bgp_main.h
 * @brief  BGP 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef BGP_MAIN_H
#define BGP_MAIN_H

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

    /* BGP TCP server */
    int epoll_fd;            /**< BGP server epoll fd */
    int running;             /**< server 线程运行标志 */
    pthread_t server_thread; /**< BGP server 线程句柄 */

    int listen_fd; /**< 全局 0.0.0.0:179 listen socket fd，-1 表示未监听 */
} bgp_local_t;

extern bgp_local_t *g_bgp_local;

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief BGP 模块初始化（由 bgp_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int bgp_module_init(void);

/**
 * @brief 启动全局 BGP listen socket（绑定 0.0.0.0:179 并加入 epoll），已监听时幂等
 */
void bgp_listen_start(void);

/**
 * @brief 停止全局 BGP listen socket（从 epoll 移除并关闭），未监听时幂等
 */
void bgp_listen_stop(void);

/**
 * @brief AF neighbor 使能后启动主动 TCP 连接到 session->neighbor_addr:179
 * @param session BGP 会话结构
 */
void bgp_server_start_active_conn(bgp_session_t *session);

/**
 * @brief AF neighbor 停用或 neighbor 删除时关闭 session 的所有连接
 * @param session BGP 会话结构
 */
void bgp_server_stop_session_conns(bgp_session_t *session);

#endif /* BGP_MAIN_H */
