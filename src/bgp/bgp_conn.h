/**
 * @file   bgp_conn.h
 * @brief  BGP TCP 连接处理器（含主动连接逻辑，内嵌于 bgp_session_t）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_CONN_H
#define BGP_CONN_H

#include <glib.h>
#include <stdint.h>

#include "bgp_peer.h"
#include "net_addr.h"

/** BGP 报文头部长度（marker 16 + length 2 + type 1） */
#define BGP_MSG_HEADER_SIZE 19
/** BGP Hold Time（秒） */
#define BGP_HOLD_TIME 90
/** TCP 连接接收缓冲区大小 */
#define BGP_RECV_BUF_SIZE 4096

/** BGP 报文类型 */
typedef enum bgp_msg_type
{
    BGP_MSG_OPEN = 1,         /**< OPEN 报文 */
    BGP_MSG_UPDATE = 2,       /**< UPDATE 报文 */
    BGP_MSG_NOTIFICATION = 3, /**< NOTIFICATION 报文 */
    BGP_MSG_KEEPALIVE = 4,    /**< KEEPALIVE 报文 */
} bgp_msg_type_t;

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
 * @brief BGP TCP 连接处理器（内嵌于 bgp_session_t，不单独堆分配）
 *
 * fd=-1 表示无连接；is_connecting=TRUE 表示 TCP 握手中（等待 EPOLLOUT）
 * session 反向指针在 bgp_session_create / bgp_handle_passive_accept 中赋值，
 * bgp_conn_init 会清零此字段，调用方负责在 init 后重新赋值。
 */
typedef struct bgp_conn
{
    struct bgp_session *session; /**< 所属 session 反向指针（借用引用，不持有所有权） */
    int fd;                      /**< TCP socket fd；-1 表示无连接 */
    net_addr_t peer_addr;        /**< 对端 IP 地址 */
    gboolean is_active;          /**< TRUE=本方发起的主动连接；FALSE=被动接入 */
    gboolean is_connecting;      /**< TRUE=TCP 握手中；FALSE=已建立或无连接 */
    bgp_conn_state_t state;      /**< BGP 协议握手状态（fd>=0 且 is_connecting=FALSE 时有效） */
} bgp_conn_t;

/**
 * @brief 堆分配并初始化连接结构（fd=-1，session 反向指针指向 sess）
 * @param sess 所属会话（借用引用）
 * @return 新建的连接结构指针
 */
bgp_conn_t *bgp_conn_create(struct bgp_session *sess);

/**
 * @brief 清理并释放连接结构（关闭 fd，释放 negotiated_afs，g_free 对象）
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
 * @brief 向对端发送 BGP OPEN 报文
 * @param conn      连接处理器
 * @param local_as  本地 AS 号
 * @param router_id 本地 BGP Identifier（点分十进制字符串）
 * @param af_peers  地址族 peer 列表（bgp_peer_t*），非 NULL 时携带 MP 扩展能力
 * @return 0 成功，-1 失败
 */
int bgp_conn_send_open(bgp_conn_t *conn, uint32_t local_as, const char *router_id, GList *af_peers);

/**
 * @brief 向对端发送 BGP KEEPALIVE 报文
 * @param conn 连接处理器
 * @return 0 成功，-1 失败
 */
int bgp_conn_send_keepalive(bgp_conn_t *conn);

/**
 * @brief 接收并处理对端数据，驱动握手状态机
 * @param conn 连接处理器
 * @return 0 继续，-1 连接应关闭
 */
int bgp_conn_on_data(bgp_conn_t *conn);

#endif /* BGP_CONN_H */
