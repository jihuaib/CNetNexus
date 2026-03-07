/**
 * @file   bgp_session.h
 * @brief  BGP 会话结构定义（内嵌 pri_conn/sec_conn，持有 VRF 反向指针）
 * @author jhb
 * @date   2026/03/03
 */
#ifndef BGP_SESSION_H
#define BGP_SESSION_H

#include <glib.h>
#include <stdint.h>

#include "bgp_conn.h"
#include "bit.h"
#include "net_addr.h"

/** OPEN 能力协商标记位（存储于 bgp_session_t.flags） */
#define BGP_SESS_CAP_AS4 (1U << 0)           /**< 支持 4 字节 AS 号（RFC 6793） */
#define BGP_SESS_CAP_ROUTE_REFRESH (1U << 1) /**< 支持 Route Refresh（RFC 2918） */

/** 默认 OPEN 能力值（AS4 + Route Refresh 均使能） */
#define BGP_SESS_CAP_DEFAULT (BGP_SESS_CAP_AS4 | BGP_SESS_CAP_ROUTE_REFRESH)

/* 前向声明，避免循环包含 */
typedef struct bgp_vrf bgp_vrf_t;
typedef struct bgp_peer bgp_peer_t;

/* 前向声明，避免 bgp_retry_sentinel_t 与 bgp_session_t 的循环依赖 */
struct bgp_session;

/**
 * @brief epoll timerfd 反向引用结构
 *
 * 内嵌于 bgp_session_t。注册 timerfd 到 epoll 时，data.ptr 设为
 * (void *)((uintptr_t)&sess->retry_sentinel | 1UL)，以 bit0=1
 * 与 bgp_conn_t* 区分，无需修改 bgp_conn_t。
 */
typedef struct bgp_retry_sentinel
{
    struct bgp_session *session; /**< 所属 session（借用引用，不持有所有权） */
} bgp_retry_sentinel_t;

/**
 * @brief BGP 会话结构（一条 neighbor 配置）
 *
 * pri_conn：主连接指针（NULL=无连接）；碰撞解决后唯一存活的连接始终在此
 * sec_conn：次连接指针（NULL=无连接）；被动接入时临时使用，碰撞解决后置 NULL
 * remote_id / negotiated_afs：由 OPEN 报文协商填入，连接断开后保留到下次连接覆盖
 * recv_buf / recv_len：TCP 接收缓冲区，每次新建连接前由调用方重置 recv_len
 * peer_list：当前 session 在各 AF 下使能的 bgp_peer_t* 列表（借用引用，所有权在 inst->peer_hash）
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
    GList *peer_list;  /**< 此 session 在各 AF 下使能的 bgp_peer_t*（借用引用，不持有所有权） */
    uint32_t flags;    /**< 能力标记位（BGP_SESS_CAP_*），控制 OPEN 报文携带的能力集合 */
    int retry_timerfd; /**< connect-retry timerfd，-1 表示未调度 */
    bgp_retry_sentinel_t retry_sentinel; /**< timerfd 注册 epoll 时使用的 data.ptr 目标（bit0=1 标记） */
    bgp_vrf_t *vrf;                      /**< 所属 VRF（借用引用，不持有所有权） */
    bgp_conn_state_t state;              /**< BGP 协议握手状态（fd>=0 且 is_connecting=FALSE 时有效） */
} bgp_session_t;

/**
 * @brief 创建 BGP 会话结构
 * @param addr      邻居 IP 地址
 * @param remote_as 远端 AS 号
 * @param vrf       所属 VRF（借用引用）
 * @return 新建的会话结构指针
 */
bgp_session_t *bgp_session_create(const net_addr_t *addr, uint32_t remote_as, bgp_vrf_t *vrf);

/**
 * @brief 销毁 BGP 会话结构（cleanup 两个 conn，释放 negotiated_afs）
 * @param session 会话结构指针（允许为 NULL）
 */
void bgp_session_destroy(bgp_session_t *session);

/**
 * @brief 为 session 启动 connect-retry timerfd 并注册到 epoll
 *
 * 若已调度（retry_timerfd >= 0）则幂等跳过。
 * timerfd 注册时 data.ptr 携带 bit0=1 标记，与 bgp_conn_t* 区分。
 *
 * @param sess      会话结构
 * @param epoll_fd  BGP server 的 epoll fd
 * @param retry_sec connect-retry 间隔（秒），取自 VRF 配置
 */
void bgp_session_arm_retry(bgp_session_t *sess, int epoll_fd, uint16_t retry_sec);

/**
 * @brief 取消 session 的 connect-retry timerfd（从 epoll 移除并关闭）
 *
 * @param sess     会话结构
 * @param epoll_fd BGP server 的 epoll fd
 */
void bgp_session_cancel_retry(bgp_session_t *sess, int epoll_fd);

#endif /* BGP_SESSION_H */
