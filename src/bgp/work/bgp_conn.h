/**
 * @file   bgp_conn.h
 * @brief  BGP TCP 连接处理器（负责 TCP 连接的建立与销毁）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_CONN_H
#define BGP_CONN_H

#include <glib.h>
#include <stdint.h>

#include "net_addr.h"

/** TCP 连接接收缓冲区大小 */
#define BGP_RECV_BUF_SIZE 4096

/* 前向声明：bgp_conn_t 内嵌于 bgp_session_t，此处仅需指针 */
struct bgp_session;

/**
 * @brief BGP TCP 连接处理器
 *
 * fd=-1 表示无连接；is_connecting=TRUE 表示 TCP 握手中（等待 EPOLLOUT）
 * session 反向指针在 bgp_session_create / bgp_handle_passive_accept 中赋值。
 * BGP 协议握手状态统一由 session->fsm_state 持有；conn 仅保留 TCP/socket 运行态。
 * local_addr/has_local_addr 用于主动建连时绑定源地址（neighbor source-interface）。
 * has_ttl/ttl 用于主动建连时设置 IP TTL/Hop-Limit（neighbor ebgp-multihop）。
 */
typedef struct bgp_conn
{
    struct bgp_session *session; /**< 所属 session 反向指针（借用引用，不持有所有权） */
    int fd;                      /**< TCP socket fd；-1 表示无连接 */
    net_addr_t peer_addr;        /**< 对端 IP 地址 */
    gboolean is_active;          /**< TRUE=本方发起的主动连接；FALSE=被动接入 */
    gboolean is_connecting;      /**< TRUE=TCP 握手中；FALSE=已建立或无连接 */
    gboolean has_local_addr;     /**< TRUE=主动建连前需 bind(local_addr) */
    gboolean has_ttl;            /**< TRUE=主动建连前需 setsockopt TTL/Hop-Limit */
    int last_socket_error;       /**< 最近一次 socket 层错误码（SO_ERROR/errno，0=无错误） */
    net_addr_t local_addr;       /**< 主动建连绑定源地址（has_local_addr=TRUE 时有效） */
    uint8_t ttl;                 /**< 出向 TTL/Hop-Limit（has_ttl=TRUE 时有效） */
} bgp_conn_t;

/**
 * @brief 堆分配并初始化连接结构（fd=-1，session 反向指针指向 sess）
 * @param sess 所属会话（借用引用）
 * @return 新建的连接结构指针
 */
bgp_conn_t *bgp_conn_create(struct bgp_session *sess);

/**
 * @brief 清理并释放连接结构（关闭 fd，g_free 对象）
 * @param conn 连接结构指针（允许为 NULL）
 */
void bgp_conn_destroy(bgp_conn_t *conn);

/**
 * @brief 发起非阻塞主动 TCP 连接到 peer_addr:179，注册 EPOLLOUT，设置 is_active/is_connecting
 * @param conn      连接结构指针（fd 必须为 -1）
 * @param peer_addr 目标邻居地址
 * @param epoll_fd  BGP server 的 epoll fd
 * @return 成功返回 socket fd，失败返回 -1
 */
int bgp_conn_start_active(bgp_conn_t *conn, const net_addr_t *peer_addr, int epoll_fd);

/**
 * @brief 获取连接当前本地端点地址（基于 getsockname）
 * @param conn      连接结构（fd 必须有效）
 * @param out_addr  输出本地地址
 * @return 0 成功，-1 失败
 */
int bgp_conn_get_local_addr(const bgp_conn_t *conn, net_addr_t *out_addr);

/**
 * @brief 关闭 session 上指定 slot 的连接（epoll 移除 + 销毁 + 记录 socket error）
 * @param sess     所属会话
 * @param slot     &sess->pri_conn 或 &sess->sec_conn
 * @param epoll_fd BGP server 的 epoll fd
 */
void bgp_conn_close(struct bgp_session *sess, bgp_conn_t **slot, int epoll_fd);

#endif /* BGP_CONN_H */
