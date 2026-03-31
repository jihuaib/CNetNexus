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
#include "bgp_fsm.h"
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
 * @brief 向对端发送 BGP NOTIFICATION 报文
 *
 * 报文格式：BGP 头部（19 B）+ error_code（1 B）+ error_subcode（1 B）。
 * 发送后调用方负责关闭连接，NOTIFICATION 本身不触发任何状态变更。
 *
 * @param conn          连接处理器（fd 必须有效）
 * @param error_code    错误码，见 BGP_ERR_*
 * @param error_subcode 错误子码，见 BGP_CEASE_* 等
 * @return 0 成功，-1 发送失败
 */
int bgp_pkt_send_notification(bgp_conn_t *conn, uint8_t error_code, uint8_t error_subcode);

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
 * @brief 接收并处理对端数据，按完整 BGP PDU 驱动 session 级协议状态
 *
 * 内部直接派发 FSM 事件和碰撞处理，调用方无需额外处理返回值。
 *
 * @param conn     连接处理器
 * @param epoll_fd BGP server 的 epoll fd
 */
void bgp_pkt_on_data(bgp_conn_t *conn, int epoll_fd);

/**
 * @brief 注册所有内置 AF 编码器（幂等，在 bgp_module_init 中调用一次）
 */
void bgp_pkt_build_init(void);

#endif /* BGP_PKT_H */
