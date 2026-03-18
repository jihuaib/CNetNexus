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

/** BGP TCP 连接握手状态 */
typedef enum bgp_conn_state
{
    BGP_CONN_STATE_OPEN_SENT = 0,    /**< 已发送 OPEN，等待对端 OPEN */
    BGP_CONN_STATE_OPEN_CONFIRM = 1, /**< 已收到 OPEN，已回 KEEPALIVE，等待对端 KEEPALIVE */
    BGP_CONN_STATE_ESTABLISHED = 2,  /**< 会话已建立 */
} bgp_conn_state_t;

/* 前向声明：bgp_conn_t 内嵌于 bgp_session_t，此处仅需指针 */
struct bgp_session;

/**
 * @brief BGP TCP 连接处理器
 *
 * fd=-1 表示无连接；is_connecting=TRUE 表示 TCP 握手中（等待 EPOLLOUT）
 * session 反向指针在 bgp_session_create / bgp_handle_passive_accept 中赋值。
 * recv_buf/recv_len 为每连接独立接收缓冲区，支持碰撞检测期间两条连接并存。
 * state 为本条连接的 BGP 握手状态（每连接独立，碰撞检测期间 pri/sec 可处于不同状态）。
 */
typedef struct bgp_conn
{
    struct bgp_session *session;         /**< 所属 session 反向指针（借用引用，不持有所有权） */
    int fd;                              /**< TCP socket fd；-1 表示无连接 */
    net_addr_t peer_addr;                /**< 对端 IP 地址 */
    gboolean is_active;                  /**< TRUE=本方发起的主动连接；FALSE=被动接入 */
    gboolean is_connecting;              /**< TRUE=TCP 握手中；FALSE=已建立或无连接 */
    bgp_conn_state_t state;              /**< BGP 握手状态机（属于连接，不属于会话） */
    uint8_t recv_buf[BGP_RECV_BUF_SIZE]; /**< 每连接独立 TCP 接收缓冲区 */
    uint32_t recv_len;                   /**< 缓冲区中已有数据长度 */
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

#endif /* BGP_CONN_H */
