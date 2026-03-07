/**
 * @file   bgp_pkt.h
 * @brief  BGP 报文定义、组包与解析接口
 * @author jhb
 * @date   2026/03/07
 */
#ifndef BGP_PKT_H
#define BGP_PKT_H

#include <glib.h>
#include <stdint.h>

#include "bgp_conn.h"
#include "bgp_peer.h"

/** BGP 报文头部长度（marker 16 + length 2 + type 1） */
#define BGP_MSG_HEADER_SIZE 19

/** BGP Hold Time（秒） */
#define BGP_HOLD_TIME 90

/** BGP 报文类型 */
typedef enum bgp_msg_type
{
    BGP_MSG_OPEN = 1,         /**< OPEN 报文 */
    BGP_MSG_UPDATE = 2,       /**< UPDATE 报文 */
    BGP_MSG_NOTIFICATION = 3, /**< NOTIFICATION 报文 */
    BGP_MSG_KEEPALIVE = 4,    /**< KEEPALIVE 报文 */
} bgp_msg_type_t;

/**
 * @brief 向对端发送 BGP OPEN 报文
 * @param conn      连接处理器
 * @param local_as  本地 AS 号
 * @param router_id 本地 BGP Identifier（点分十进制字符串）
 * @param af_peers  地址族 peer 列表（bgp_peer_t*），非 NULL 时携带 MP 扩展能力
 * @return 0 成功，-1 失败
 */
int bgp_pkt_send_open(bgp_conn_t *conn, uint32_t local_as, const char *router_id, GList *af_peers);

/**
 * @brief 向对端发送 BGP KEEPALIVE 报文
 * @param conn 连接处理器
 * @return 0 成功，-1 失败
 */
int bgp_pkt_send_keepalive(bgp_conn_t *conn);

/**
 * @brief 接收并处理对端数据，驱动握手状态机
 * @param conn 连接处理器
 * @return 0 继续，-1 连接应关闭
 */
int bgp_pkt_on_data(bgp_conn_t *conn);

#endif /* BGP_PKT_H */
