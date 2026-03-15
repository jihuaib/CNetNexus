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

/* 包含顺序：bgp_peer.h 定义枚举，必须先于 bgp.h（定义同名宏） */
#include "bgp.h"
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
 * @param router_id 本地 BGP Identifier（主机序 32 位整数，0 时填充 0.0.0.0）
 * @param af_peers  地址族 peer 列表（bgp_peer_t*），非 NULL 时携带 MP 扩展能力
 * @return 0 成功，-1 失败
 */
int bgp_pkt_send_open(bgp_conn_t *conn, uint32_t local_as, uint32_t router_id, GList *af_peers);

/**
 * @brief 向对端发送 BGP KEEPALIVE 报文
 * @param conn 连接处理器
 * @return 0 成功，-1 失败
 */
int bgp_pkt_send_keepalive(bgp_conn_t *conn);

/**
 * @brief 向对端发送 BGP UPDATE 报文（宣告一条路由）
 *
 * - IPv4 unicast：NEXT_HOP 属性 + NLRI 字段
 * - IPv6 unicast：MP_REACH_NLRI 扩展属性（RFC 4760）
 *
 * @param conn    连接处理器（fd 必须有效）
 * @param nlri    NLRI 条目（当前仅支持 BGP_NLRI_PREFIX）
 * @param attr    路径属性（ORIGIN / AS_PATH / LOCAL_PREF / MED / COMMUNITY）
 * @param nexthop 下一跳
 * @return 0 成功，-1 失败
 */
int bgp_pkt_send_update(bgp_conn_t *conn, const bgp_nlri_entry_t *nlri, const bgp_attr_t *attr,
                        const bgp_nexthop_t *nexthop);

/**
 * @brief 向对端发送 BGP UPDATE 报文（撤销一条路由）
 *
 * - IPv4 unicast：Withdrawn Routes 字段
 * - IPv6 unicast：MP_UNREACH_NLRI 扩展属性（RFC 4760）
 *
 * @param conn 连接处理器（fd 必须有效）
 * @param nlri NLRI 条目（当前仅支持 BGP_NLRI_PREFIX）
 * @return 0 成功，-1 失败
 */
int bgp_pkt_send_withdraw(bgp_conn_t *conn, const bgp_nlri_entry_t *nlri);

/**
 * @brief bgp_pkt_on_data 特殊返回值
 *
 * RFC 4271 §6.8 碰撞检测结果：
 *   COLLISION_CLOSE_ME    - 当前 conn 应关闭（另一条连接保留）
 *   COLLISION_CLOSE_OTHER - 另一条连接应关闭（当前 conn 已进入 OPEN_CONFIRM）
 */
#define BGP_PKT_ON_DATA_COLLISION_CLOSE_ME (-2)
#define BGP_PKT_ON_DATA_COLLISION_CLOSE_OTHER (-3)

/**
 * @brief 接收并处理对端数据，驱动握手状态机
 * @param conn 连接处理器
 * @return 0 继续；-1 连接应关闭；BGP_PKT_ON_DATA_COLLISION_CLOSE_ME/OTHER 碰撞检测结果
 */
int bgp_pkt_on_data(bgp_conn_t *conn);

/**
 * @brief 注册所有内置 AF 编码器（幂等，在 bgp_module_init 中调用一次）
 */
void bgp_pkt_build_init(void);

#endif /* BGP_PKT_H */
