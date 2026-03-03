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
#include "bgp_listen.h"
#include "bgp_protocol.h"
#include "dev.h"

/**
 * @brief 描述 epoll 中每个非 listener fd 的元数据
 *
 * is_active/is_connecting 通过 entry->conn->is_active/is_connecting 访问
 */
typedef struct bgp_fd_entry
{
    bgp_session_t *session; /**< 所属 session（借用引用，不持有所有权） */
    bgp_conn_t *conn;       /**< 指向 session->pri_conn 或 session->sec_conn（借用引用） */
} bgp_fd_entry_t;

typedef struct bgp_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    bgp_protocol_t *protocol; /**< BGP 协议结构（bgp 使能后非 NULL） */

    /* BGP TCP server */
    int epoll_fd;            /**< BGP server epoll fd */
    int running;             /**< server 线程运行标志 */
    pthread_t server_thread; /**< BGP server 线程句柄 */

    bgp_listen_t *listener;   /**< 全局 0.0.0.0:179 listen socket */
    GHashTable *fd_entries;   /**< int* fd → bgp_fd_entry_t*（所有 non-listener fd） */
    pthread_mutex_t fd_mutex; /**< 保护 fd_entries（CLI 线程和 server 线程均访问） */
} bgp_local_t;

extern bgp_local_t *g_bgp_local;

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

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
