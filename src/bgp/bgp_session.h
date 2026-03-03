/**
 * @file   bgp_session.h
 * @brief  BGP 会话结构定义（内嵌 pri_conn/sec_conn，无 VRF 字段）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_SESSION_H
#define BGP_SESSION_H

#include <glib.h>
#include <stdint.h>

#include "bgp_conn.h"
#include "net_addr.h"

/**
 * @brief BGP 会话结构（一条 neighbor 配置）
 *
 * pri_conn：本方主动发起的连接（fd=-1 表示无）
 * sec_conn：对端被动接入的连接（fd=-1 表示无）
 * peers：per-AF peer 借用引用链表，由 bgp_instance_t 持有所有权
 */
typedef struct bgp_session
{
    net_addr_t neighbor_addr; /**< 邻居 IP 地址（sess_hash 的键） */
    uint32_t remote_as;       /**< 远端 AS 号 */
    bgp_conn_t pri_conn;      /**< 主连接（内嵌，fd=-1=无） */
    bgp_conn_t sec_conn;      /**< 次连接（内嵌，fd=-1=无） */
    GList *peers;             /**< per-AF peer 借用引用列表（bgp_peer_t*，由 bgp_instance_t 负责销毁） */
} bgp_session_t;

/**
 * @brief 创建 BGP 会话结构
 * @param addr      邻居 IP 地址
 * @param remote_as 远端 AS 号
 * @return 新建的会话结构指针
 */
bgp_session_t *bgp_session_create(const net_addr_t *addr, uint32_t remote_as);

/**
 * @brief 销毁 BGP 会话结构（cleanup 两个 conn，释放 peers 借用链表节点，不销毁 bgp_peer_t）
 * @param session 会话结构指针（允许为 NULL）
 */
void bgp_session_destroy(bgp_session_t *session);

#endif /* BGP_SESSION_H */
