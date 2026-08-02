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

#include "bgp.h"
#include "bgp_conn.h"
#include "bgp_fsm.h"
#include "bit.h"
#include "if.h"
#include "net_addr.h"

/** OPEN 能力协商标记位（存储于 bgp_session_t.flags） */
#define BGP_SESS_CAP_AS4 (1U << 0)           /**< 支持 4 字节 AS 号（RFC 6793） */
#define BGP_SESS_CAP_ROUTE_REFRESH (1U << 1) /**< 支持 Route Refresh（RFC 2918） */
#define BGP_SESS_CAP_EXT_NEXTHOP (1U << 2)   /**< 扩展下一跳编码（RFC 8950） */

/** 默认 OPEN 能力值（AS4 + Route Refresh 使能；EXT_NEXTHOP 由 AF enable 时按需设置） */
#define BGP_SESS_CAP_DEFAULT (BGP_SESS_CAP_AS4 | BGP_SESS_CAP_ROUTE_REFRESH)

/** BGP 会话类型（iBGP / eBGP），在会话创建或 remote-as 变更时确定 */
typedef enum bgp_sess_type
{
    BGP_SESS_TYPE_UNKNOWN = 0, /**< AS 未配置，类型未定 */
    BGP_SESS_TYPE_IBGP = 1,    /**< iBGP 会话（remote_as == local_as） */
    BGP_SESS_TYPE_EBGP = 2,    /**< eBGP 会话（remote_as != local_as） */
} bgp_sess_type_t;

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
} bgp_timer_type_t;

/**
 * @brief BGP 会话收包统计（按协议报文类型分类）
 *
 * 记录该 session 生命周期内从对端收到的每种 BGP 协议报文的累计个数，
 * 计数在报文头部校验通过、完成一帧读取后递增，与 FSM 分发逻辑独立，
 * 因此解析失败 / 触发 NOTIFICATION 的报文同样会被计入对应分类。
 * 计数跨 TCP 连接重建保留（不在 reset_negotiated / neighbor_down 中清零），
 * 仅在 session 销毁时随结构释放。
 */
typedef struct bgp_msg_rx_stats
{
    uint32_t open;          /**< 收到 OPEN 报文个数 */
    uint32_t update;        /**< 收到 UPDATE 报文个数 */
    uint32_t notification;  /**< 收到 NOTIFICATION 报文个数 */
    uint32_t keepalive;     /**< 收到 KEEPALIVE 报文个数 */
    uint32_t route_refresh; /**< 收到 ROUTE-REFRESH 报文个数（RFC 2918） */
    uint32_t unknown;       /**< 收到未知/非法类型报文个数 */
    uint32_t total;         /**< 收到 BGP 报文总个数（所有类型之和） */
} bgp_msg_rx_stats_t;

/**
 * @brief BGP 会话发包统计（按协议报文类型分类）
 *
 * 记录该 session 生命周期内向对端实际发出的每种 BGP 协议报文的累计个数，
 * 仅在 send() 完整写出整帧后递增（短写不计）。语义、生命周期与 rx_msg_stats 对称。
 */
typedef struct bgp_msg_tx_stats
{
    uint32_t open;          /**< 发出 OPEN 报文个数 */
    uint32_t update;        /**< 发出 UPDATE 报文个数 */
    uint32_t notification;  /**< 发出 NOTIFICATION 报文个数 */
    uint32_t keepalive;     /**< 发出 KEEPALIVE 报文个数 */
    uint32_t route_refresh; /**< 发出 ROUTE-REFRESH 报文个数（RFC 2918） */
    uint32_t total;         /**< 发出 BGP 报文总个数（所有类型之和） */
} bgp_msg_tx_stats_t;

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
 * remote_id / remote_afs：由对端 OPEN 报文携带的 MP 能力填入（afs 以 (afi<<16|safi) 打包存储，remote_id 为主机序
 * uint32_t） local_afs：本端发送 OPEN 时实际携带的 MP 能力快照（与对端协商成功后保留） local_router_id：发送 OPEN
 * 时使用的本地 BGP Identifier（主机序 uint32_t，用于 §6.8 碰撞检测） peer_list：当前 session 在各 AF 下使能的
 * bgp_peer_t* 列表（借用引用）
 *
 * timerfd 生命周期：
 *   retry_timerfd  — connect 失败后单次触发，成功后取消
 *   ka_timerfd     — ESTABLISHED 后周期触发，断开后取消
 *   hold_timerfd   — ESTABLISHED 后单次触发，每收到 KA/UPDATE 重置，断开后取消
 */
typedef struct bgp_session
{
    net_addr_t neighbor_addr;  /**< 邻居 IP 地址（sess_hash 的键） */
    uint32_t remote_as;        /**< 远端 AS 号（配置值） */
    bgp_sess_type_t sess_type; /**< 会话类型（iBGP/eBGP），由 remote_as 与 local_as 比较确定 */
    char source_if_name[IF_LOGICAL_NAME_MAX]; /**< 出向建连 source-interface（空串表示未配置） */
    net_addr_t source_addr;    /**< source-interface 当前解析出的源地址（family=0 表示未配置） */
    uint8_t ebgp_multihop_ttl; /**< eBGP multihop TTL（0 表示未配置，默认直连） */
    bgp_conn_t *pri_conn;      /**< 主连接（NULL=无） */
    bgp_conn_t *sec_conn;      /**< 碰撞检测期间的第二条连接（NULL=无） */
    int pri_last_socket_error; /**< 主连接槽最近一次 socket 错误码（0=无） */
    int sec_last_socket_error; /**< 次连接槽最近一次 socket 错误码（0=无） */
    uint32_t remote_id;        /**< 对端 BGP Router ID（主机序 32 位，由 OPEN 填入，0 表示未建立） */
    uint32_t local_router_id; /**< 本地 BGP Router ID（主机序 32 位，发送 OPEN 时保存，用于 RFC §6.8 比较） */
    GArray *local_afs;     /**< 本端发送 OPEN 携带的 MP 能力快照（每元素 (afi<<16|safi) 打包） */
    GArray *remote_afs;    /**< 对端 OPEN 携带的 MP 能力（每元素 (afi<<16|safi) 打包） */
    GArray *local_ext_nh;  /**< 本端 OPEN 携带的 RFC 8950 tuple（bgp_ext_nh_entry_t） */
    GArray *remote_ext_nh; /**< 对端 OPEN 携带的 RFC 8950 tuple（bgp_ext_nh_entry_t） */
    GList *peer_list;      /**< 各 AF 下使能的 bgp_peer_t*（借用引用） */

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

    gint64 established_at_usec; /**< 会话最近一次进入 ESTABLISHED 状态的时间戳（g_get_real_time，0 表示未建立） */

    bgp_msg_rx_stats_t rx_msg_stats; /**< 收到的 BGP 协议报文累计统计（按类型分类） */
    bgp_msg_tx_stats_t tx_msg_stats; /**< 发出的 BGP 协议报文累计统计（按类型分类） */

    bgp_vrf_t *vrf;            /**< 所属 VRF（借用引用，不持有所有权） */
    bgp_fsm_state_t fsm_state; /**< RFC 4271 §8 FSM 当前状态（唯一 BGP 协议态） */
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
 * @brief 根据 local_as 更新会话类型（iBGP/eBGP）
 * @param sess     目标会话
 * @param local_as 本地 AS 号
 */
void bgp_session_update_type(bgp_session_t *sess, uint32_t local_as);

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
 * @brief 判断 RFC 8950 Extended Next Hop 三元组是否完成双向协商
 *
 * 只有本端与对端最近一次 OPEN 都携带完全相同的
 * (NLRI AFI, NLRI SAFI, Nexthop AFI) 时才返回 TRUE。
 */
gboolean bgp_session_ext_nh_negotiated(const bgp_session_t *sess, uint16_t nlri_afi, uint16_t nlri_safi,
                                       uint16_t nh_afi);

/**
 * @brief 触发本 session 发起主动 TCP 连接（仅 worker 线程调用）
 *
 * 实际由 FSM 的 AUTO_START 事件驱动连接建立；此函数主要用于配置变更后
 * 启动新会话或从 IDLE 重新建连的入口。
 */
void bgp_session_start_active(bgp_session_t *session);

/**
 * @brief 全面关闭本 session 的所有资源（仅 worker 线程调用）
 *
 * 取消三类定时器、从各 AF subgroup 脱队、关闭 pri/sec 连接、清理 RIB 中
 * 属于该邻居的 relay 路由，最后将 FSM 状态重置为 IDLE。
 */
void bgp_session_stop_all(bgp_session_t *session);

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

/**
 * @brief 为 session 的收包统计计数一个 BGP 报文
 *
 * 在报文头部校验通过、帧体读取完成后调用，按 msg_type 归类计数；
 * 未知/非法类型统一落入 unknown 桶。total 始终递增。
 *
 * @param sess     目标会话（NULL 时为空操作）
 * @param msg_type BGP 报文头部中的 Type 字段（bgp_msg_type_t 枚举值）
 */
void bgp_session_rx_msg_count(bgp_session_t *sess, uint8_t msg_type);

/**
 * @brief 为 session 的发包统计计数一个 BGP 报文
 *
 * 在 send() 完整写出整帧后调用，按 msg_type 归类计数；
 * 仅统计真实发出的 BGP 报文，因此短写/失败不会计入。
 *
 * @param sess     目标会话（NULL 时为空操作）
 * @param msg_type BGP 报文头部中的 Type 字段（bgp_msg_type_t 枚举值）
 */
void bgp_session_tx_msg_count(bgp_session_t *sess, uint8_t msg_type);

#endif /* BGP_SESSION_H */
