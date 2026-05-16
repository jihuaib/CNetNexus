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
    BGP_MSG_OPEN = 1,          /**< OPEN 报文 */
    BGP_MSG_UPDATE = 2,        /**< UPDATE 报文 */
    BGP_MSG_NOTIFICATION = 3,  /**< NOTIFICATION 报文 */
    BGP_MSG_KEEPALIVE = 4,     /**< KEEPALIVE 报文 */
    BGP_MSG_ROUTE_REFRESH = 5, /**< ROUTE-REFRESH 报文（RFC 2918） */
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
 * @brief 向对端发送 BGP ROUTE-REFRESH 报文（RFC 2918）
 *
 * 报文格式：BGP 头部（19 B）+ AFI（2 B）+ Reserved（1 B）+ SAFI（1 B），共 23 B。
 * 仅当对端在 OPEN 中宣告了 Route Refresh 能力时才应发送。
 *
 * @param conn 连接处理器（fd 必须有效）
 * @param afi  请求刷新的地址族
 * @param safi 请求刷新的子地址族
 * @return 0 成功，-1 发送失败
 */
int bgp_pkt_send_route_refresh(bgp_conn_t *conn, uint16_t afi, uint8_t safi);

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
 * @brief 构建打包 UPDATE 报文（多条共享属性的 NLRI）
 *
 * 将多条共享同一 (attr, nexthop) 的 NLRI 打包进一个 UPDATE 报文，
 * 同一子组内所有 session 可共享此报文发送。
 *
 * - IPv4+IPv4 nh：单个 NEXT_HOP 属性 + 多条 NLRI（传统编码）
 * - IPv4+IPv6 nh / IPv6：MP_REACH_NLRI 内嵌多条前缀（RFC 4760/8950）
 *
 * 缓冲区不足时仅打包前 *out_packed_count 条 NLRI；调用方需循环处理剩余。
 *
 * @param buf              输出缓冲区
 * @param buf_size         缓冲区大小（建议 BGP_UPDATE_BUF_SIZE=4096）
 * @param nlri_list        NLRI 指针数组
 * @param nlri_count       数组长度
 * @param attr             共享路径属性
 * @param nexthop          共享 nexthop
 * @param afi              地址族
 * @param safi             子地址族
 * @param out_packed_count 实际打包的 NLRI 数
 * @return 报文总字节数，-1=失败（连单条都放不下）
 */
int bgp_pkt_build_packed_update(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                const bgp_attr_t *attr, const bgp_nexthop_t *nexthop, uint16_t afi, uint8_t safi,
                                int *out_packed_count);

/**
 * @brief 构建打包 WITHDRAW 报文（多条 NLRI）
 *
 * - IPv4 peer（无 EXT_NEXTHOP）：Withdrawn Routes 段内嵌多条 IPv4 前缀
 * - IPv6 peer：MP_UNREACH_NLRI 内嵌多条前缀
 *
 * @param buf              输出缓冲区
 * @param buf_size         缓冲区大小
 * @param nlri_list        NLRI 指针数组
 * @param nlri_count       数组长度
 * @param conn             目标连接（用于判断 RFC 8950 场景）
 * @param afi              地址族
 * @param safi             子地址族
 * @param out_packed_count 实际打包的 NLRI 数
 * @return 报文总字节数，-1=失败
 */
int bgp_pkt_build_packed_withdraw(uint8_t *buf, int buf_size, const bgp_nlri_entry_t *const *nlri_list, int nlri_count,
                                  const bgp_conn_t *conn, uint16_t afi, uint8_t safi, int *out_packed_count);

/**
 * @brief 接收并处理对端数据，按完整 BGP PDU 驱动 session 级协议状态
 *
 * 内部直接派发 FSM 事件和碰撞处理，调用方无需额外处理返回值。
 * 仅 pri_conn 的事件经过 FSM；sec_conn 事件在此函数内直接处理（关闭/提升）。
 *
 * @param conn 连接处理器
 */
void bgp_pkt_on_data(bgp_conn_t *conn);

/**
 * @brief 注册所有内置 AF 编码器（幂等，在 bgp_module_init 中调用一次）
 */
void bgp_pkt_build_init(void);

#endif /* BGP_PKT_H */
