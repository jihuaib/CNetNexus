/**
 * @file   bgp_session.h
 * @brief  BGP 会话结构定义（内嵌三类 timerfd 哨兵，持有 VRF 反向指针）
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

/* 前向声明：bgp_timer_sentinel_t 与 bgp_session_t 互相引用 */
struct bgp_session;

/** timerfd 定时器类型 */
typedef enum bgp_timer_type
{
    BGP_TIMER_TYPE_RETRY = 0,     /**< connect-retry 定时器 */
    BGP_TIMER_TYPE_KEEPALIVE = 1, /**< keepalive 周期定时器 */
    BGP_TIMER_TYPE_HOLD = 2,      /**< hold time 超时定时器 */
    BGP_TIMER_TYPE_WORK = 3,      /**< 路由工作队列（优选 + 发布）定时器 */
} bgp_timer_type_t;

/**
 * @brief epoll timerfd 反向引用结构（内嵌于 bgp_session_t，通用于三类定时器）
 *
 * 注册 timerfd 到 epoll 时，data.ptr 设为
 * (void *)((uintptr_t)&sess->xxx_sentinel | 1UL)，以 bit0=1
 * 与 bgp_conn_t* 区分；type 字段区分三种定时器。
 */
typedef struct bgp_timer_sentinel
{
    struct bgp_session *session; /**< 所属 session（借用引用，不持有所有权） */
    bgp_timer_type_t type;       /**< 定时器类型 */
} bgp_timer_sentinel_t;

/**
 * @brief BGP 会话结构（一条 neighbor 配置）
 *
 * pri_conn：主连接；碰撞解决后唯一存活的连接始终在此
 * sec_conn：碰撞检测期间临时持有第二条连接，碰撞解决后置 NULL
 * remote_id / negotiated_afs：由 OPEN 报文协商填入（afs 以 (afi<<16|safi) 打包存储，remote_id 为主机序 uint32_t）
 * local_router_id：发送 OPEN 时使用的本地 BGP Identifier（主机序 uint32_t，用于 §6.8 碰撞检测）
 * peer_list：当前 session 在各 AF 下使能的 bgp_peer_t* 列表（借用引用）
 *
 * timerfd 生命周期：
 *   retry_timerfd  — connect 失败后单次触发，成功后取消
 *   ka_timerfd     — ESTABLISHED 后周期触发，断开后取消
 *   hold_timerfd   — ESTABLISHED 后单次触发，每收到 KA/UPDATE 重置，断开后取消
 */
typedef struct bgp_session
{
    net_addr_t neighbor_addr; /**< 邻居 IP 地址（sess_hash 的键） */
    uint32_t remote_as;       /**< 远端 AS 号（配置值） */
    bgp_conn_t *pri_conn;     /**< 主连接（NULL=无） */
    bgp_conn_t *sec_conn;     /**< 碰撞检测期间的第二条连接（NULL=无） */
    uint32_t remote_id;       /**< 对端 BGP Router ID（主机序 32 位，由 OPEN 填入，0 表示未建立） */
    uint32_t local_router_id; /**< 本地 BGP Router ID（主机序 32 位，发送 OPEN 时保存，用于 RFC §6.8 比较） */
    GArray *negotiated_afs; /**< 协商地址族列表（每元素为 guint32，以 afi<<16|safi 打包） */
    GList *peer_list;       /**< 各 AF 下使能的 bgp_peer_t*（借用引用） */

    /* ---- 能力字段 ---- */
    uint32_t flags;           /**< 已配置的本地能力集（BGP_SESS_CAP_*） */
    uint32_t local_caps;      /**< 最近一次 OPEN 发出时实际携带的能力集 */
    uint32_t remote_caps;     /**< 对端 OPEN 报告的能力集 */
    uint32_t negotiated_caps; /**< 协商能力集 = local_caps ∩ remote_caps */
    uint16_t remote_hold;     /**< 对端 OPEN 报告的 Hold Time（秒） */
    uint16_t negotiated_hold; /**< 协商后的 Hold Time = min(本地, 对端) */

    /* ---- 定时器字段 ---- */
    int retry_timerfd;                   /**< connect-retry timerfd，-1 表示未调度 */
    int ka_timerfd;                      /**< keepalive 周期 timerfd，-1 表示未调度 */
    int hold_timerfd;                    /**< hold time 超时 timerfd，-1 表示未调度 */
    bgp_timer_sentinel_t retry_sentinel; /**< epoll data.ptr 目标（bit0=1 标记，type=RETRY） */
    bgp_timer_sentinel_t ka_sentinel;    /**< epoll data.ptr 目标（bit0=1 标记，type=KEEPALIVE） */
    bgp_timer_sentinel_t hold_sentinel;  /**< epoll data.ptr 目标（bit0=1 标记，type=HOLD） */

    gboolean hold_reset_pending; /**< 收到 KA 或 UPDATE 后需由 bgp_main 重置 hold 定时器 */

    bgp_vrf_t *vrf; /**< 所属 VRF（借用引用，不持有所有权） */
} bgp_session_t;

/**
 * @brief 创建 BGP 会话结构
 */
bgp_session_t *bgp_session_create(const net_addr_t *addr, uint32_t remote_as, bgp_vrf_t *vrf);

/**
 * @brief 销毁 BGP 会话结构
 */
void bgp_session_destroy(bgp_session_t *session);

/**
 * @brief 重置会话的所有协商参数
 *
 * 断邻居后调用，清除 OPEN 协商产生的所有中间状态，使下次重连时
 * 重新完整协商 router-id、hold-time、能力集、地址族列表等参数。
 *
 * @param sess 目标会话
 */
void bgp_session_reset_negotiated(bgp_session_t *sess);

/**
 * @brief 主动断邻居：发送 NOTIFICATION、关闭连接、重置协商参数、调度重连
 *
 * 流程：
 *   1. 向已完成 TCP 握手的 pri_conn 发送 NOTIFICATION Cease/Admin-Reset（6/4）
 *   2. 取消 keepalive / hold / retry 定时器
 *   3. 关闭 pri_conn 和 sec_conn，清除该邻居在 RIB 中的所有路由
 *   4. 重置 OPEN 协商参数（router-id、hold-time、caps、negotiated-afs 等）
 *   5. 按 vrf->connect_retry 调度重连定时器，触发后重新发起完整 OPEN 协商
 *
 * @param sess     目标会话（不可为 NULL）
 * @param epoll_fd BGP server 的 epoll fd（用于定时器和连接的 epoll 操作）
 */
void bgp_neighbor_down(bgp_session_t *sess, int epoll_fd);

/* ---- connect-retry 定时器 ---- */
void bgp_session_arm_retry(bgp_session_t *sess, int epoll_fd, uint16_t retry_sec);
void bgp_session_cancel_retry(bgp_session_t *sess, int epoll_fd);

/* ---- keepalive 周期定时器 ---- */
/**
 * @brief 启动 keepalive 周期定时器（ESTABLISHED 后调用）
 * @param ka_sec keepalive 间隔（秒），0 时不启动
 */
void bgp_session_arm_keepalive(bgp_session_t *sess, int epoll_fd, uint16_t ka_sec);

/**
 * @brief 取消 keepalive 定时器（断开连接时调用）
 */
void bgp_session_cancel_keepalive(bgp_session_t *sess, int epoll_fd);

/* ---- hold time 超时定时器 ---- */
/**
 * @brief 启动 hold time 单次超时定时器（ESTABLISHED 后调用）
 * @param hold_sec hold time（秒），0 时不启动（无限 hold）
 */
void bgp_session_arm_hold(bgp_session_t *sess, int epoll_fd, uint16_t hold_sec);

/**
 * @brief 重置 hold time 定时器（收到 KA 或 UPDATE 后调用，不需要 epoll_fd）
 */
void bgp_session_reset_hold(bgp_session_t *sess);

/**
 * @brief 取消 hold time 定时器（断开连接时调用）
 */
void bgp_session_cancel_hold(bgp_session_t *sess, int epoll_fd);

#endif /* BGP_SESSION_H */
