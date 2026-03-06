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
 * pri_conn：主连接指针（NULL=无连接）；碰撞解决后唯一存活的连接始终在此
 * sec_conn：次连接指针（NULL=无连接）；被动接入时临时使用，碰撞解决后置 NULL
 * remote_id / negotiated_afs：由 OPEN 报文协商填入，连接断开后保留到下次连接覆盖
 * recv_buf / recv_len：TCP 接收缓冲区，每次新建连接前由调用方重置 recv_len
 */
typedef struct bgp_session
{
    net_addr_t neighbor_addr;            /**< 邻居 IP 地址（sess_hash 的键） */
    uint32_t remote_as;                  /**< 远端 AS 号（配置值，OPEN 协商后也写入此字段） */
    bgp_conn_t *pri_conn;                /**< 主连接（NULL=无） */
    bgp_conn_t *sec_conn;                /**< 次连接（NULL=无） */
    char remote_id[16];                  /**< 对端 BGP Router ID（点分十进制，OPEN 后填入） */
    uint8_t recv_buf[BGP_RECV_BUF_SIZE]; /**< TCP 接收缓冲区 */
    uint32_t recv_len;                   /**< 缓冲区中已有数据长度 */
    GList *negotiated_afs;               /**< 协商地址族列表（gchar* "afi-safi"，如 "1-1"） */
} bgp_session_t;

/**
 * @brief 创建 BGP 会话结构
 * @param addr      邻居 IP 地址
 * @param remote_as 远端 AS 号
 * @return 新建的会话结构指针
 */
bgp_session_t *bgp_session_create(const net_addr_t *addr, uint32_t remote_as);

/**
 * @brief 销毁 BGP 会话结构（cleanup 两个 conn，释放 negotiated_afs）
 * @param session 会话结构指针（允许为 NULL）
 */
void bgp_session_destroy(bgp_session_t *session);

#endif /* BGP_SESSION_H */
