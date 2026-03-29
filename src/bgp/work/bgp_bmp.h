/**
 * @file   bgp_bmp.h
 * @brief  BGP BMP 客户端运行态数据结构与常量定义
 * @author jhb
 * @date   2026/03/29
 */
#ifndef BGP_BMP_H
#define BGP_BMP_H

#include <glib.h>
#include <stdint.h>

#include "bgp_session.h"
#include "net_addr.h"

/** BMP 实例名最大长度（含 '\0'） */
#define BGP_BMP_INST_NAME_MAX 32

// ============================================================================
// BMP 协议常量（RFC 7854）
// ============================================================================

/** BMP 协议版本 */
#define BGP_BMP_VERSION 3

/** BMP 公共头长度（Version:1 + MsgLen:4 + MsgType:1） */
#define BGP_BMP_COMMON_HDR_LEN 6

/** BMP Per-Peer 头长度（固定 42 字节） */
#define BGP_BMP_PER_PEER_HDR_LEN 42

/** BMP 消息类型 */
typedef enum bgp_bmp_msg_type
{
    BGP_BMP_MSG_ROUTE_MONITORING = 0,
    BGP_BMP_MSG_STATS_REPORT = 1,
    BGP_BMP_MSG_PEER_DOWN = 2,
    BGP_BMP_MSG_PEER_UP = 3,
    BGP_BMP_MSG_INITIATION = 4,
    BGP_BMP_MSG_TERMINATION = 5,
} bgp_bmp_msg_type_t;

/** BMP Initiation TLV 类型 */
#define BGP_BMP_INIT_TLV_STRING 0
#define BGP_BMP_INIT_TLV_SYS_DESCR 1
#define BGP_BMP_INIT_TLV_SYS_NAME 2

/** BMP Termination TLV 类型 */
#define BGP_BMP_TERM_TLV_STRING 0
#define BGP_BMP_TERM_TLV_REASON 1

/** BMP Termination 原因码 */
#define BGP_BMP_TERM_REASON_ADMIN 0
#define BGP_BMP_TERM_REASON_UNSPECIFIED 1

/** BMP Peer Down 原因码 */
#define BGP_BMP_PEER_DOWN_LOCAL_NOTIFY 1
#define BGP_BMP_PEER_DOWN_LOCAL_NO_NOTIFY 2
#define BGP_BMP_PEER_DOWN_REMOTE_NOTIFY 3
#define BGP_BMP_PEER_DOWN_REMOTE_NO_NOTIFY 4

/** BMP Per-Peer 类型 */
#define BGP_BMP_PEER_TYPE_GLOBAL 0
#define BGP_BMP_PEER_TYPE_RD 1
#define BGP_BMP_PEER_TYPE_LOCAL 2

/** BMP Per-Peer flags */
#define BGP_BMP_PEER_FLAG_V (1U << 7) /**< IPv6 地址 */
#define BGP_BMP_PEER_FLAG_L (1U << 6) /**< Post-policy */

// ============================================================================
// BMP 连接状态
// ============================================================================

/** BMP 连接状态 */
typedef enum bgp_bmp_conn_state
{
    BGP_BMP_CONN_IDLE = 0,       /**< 未配置 collector 或未启动 */
    BGP_BMP_CONN_CONNECTING = 1, /**< TCP 三次握手中 */
    BGP_BMP_CONN_UP = 2,         /**< 已连接，已发送 Initiation */
    BGP_BMP_CONN_WAIT = 3,       /**< 等待重连定时器触发 */
} bgp_bmp_conn_state_t;

// ============================================================================
// BMP 实例运行态
// ============================================================================

/**
 * @brief BMP 实例运行态结构
 *
 * 每个 BMP 实例对应一条到 collector 的 TCP 连接，
 * 在 worker 线程中管理连接生命周期和报文发送。
 */
typedef struct bgp_bmp_instance
{
    char name[BGP_BMP_INST_NAME_MAX]; /**< 实例名 */

    /* ---- 配置参数 ---- */
    net_addr_t collector_addr;   /**< 采集器 IP 地址 */
    uint16_t collector_port;     /**< 采集器端口 */
    uint16_t stats_interval;     /**< Stats Report 间隔（秒，0=不发送） */
    uint16_t reconnect_interval; /**< TCP 重连间隔（秒） */
    gboolean monitor_all;        /**< TRUE=监控全部邻居 */
    GHashTable *monitor_peers;   /**< peer_ip_str -> NULL（仅 monitor_all=FALSE 时有效） */

    /* ---- 连接状态 ---- */
    bgp_bmp_conn_state_t conn_state; /**< 当前连接状态 */
    int fd;                          /**< TCP socket fd，-1 表示未连接 */

    /* ---- 定时器 ---- */
    int reconnect_timerfd;                   /**< 重连定时器 fd，-1 表示未调度 */
    int stats_timerfd;                       /**< Stats Report 定时器 fd，-1 表示未调度 */
    bgp_timer_sentinel_t reconnect_sentinel; /**< type=BGP_TIMER_TYPE_BMP_RECONNECT */
    bgp_timer_sentinel_t stats_sentinel;     /**< type=BGP_TIMER_TYPE_BMP_STATS */

    /** BMP TCP fd 在 epoll 中的哨兵（type=BGP_TIMER_TYPE_BMP_CONN） */
    bgp_timer_sentinel_t conn_sentinel;

    /* ---- 统计 ---- */
    gint64 connected_at_usec; /**< 连接建立时间（g_get_real_time，0=未连接） */
    uint32_t peer_up_sent;
    uint32_t peer_down_sent;
    uint32_t route_monitor_sent;
    uint32_t stats_report_sent;
    uint32_t initiation_sent;
} bgp_bmp_instance_t;

// ============================================================================
// 生命周期 API（worker 线程内调用）
// ============================================================================

/**
 * @brief 创建 BMP 实例运行态
 * @param name 实例名
 * @return 新建的实例指针
 */
bgp_bmp_instance_t *bgp_bmp_instance_create(const char *name);

/**
 * @brief 销毁 BMP 实例运行态（关闭连接、释放资源）
 * @param inst 实例指针
 * @param epoll_fd worker 的 epoll fd
 */
void bgp_bmp_instance_destroy(bgp_bmp_instance_t *inst, int epoll_fd);

/**
 * @brief 判断该实例是否应监控指定邻居
 * @param inst BMP 实例
 * @param peer_ip 邻居 IP 字符串
 * @return TRUE 应监控
 */
gboolean bgp_bmp_should_monitor(const bgp_bmp_instance_t *inst, const char *peer_ip);

// ============================================================================
// 连接管理 API（worker 线程内调用）
// ============================================================================

/**
 * @brief 发起 TCP 连接到 collector
 */
void bgp_bmp_connect(bgp_bmp_instance_t *inst, int epoll_fd);

/**
 * @brief 处理 TCP 连接结果（epoll EPOLLOUT 触发）
 */
void bgp_bmp_handle_connect_result(bgp_bmp_instance_t *inst, int epoll_fd);

/**
 * @brief 处理 BMP TCP fd 读事件（检测断连）
 */
void bgp_bmp_handle_read(bgp_bmp_instance_t *inst, int epoll_fd);

/**
 * @brief 处理重连定时器触发
 */
void bgp_bmp_handle_reconnect(bgp_bmp_instance_t *inst, int epoll_fd);

/**
 * @brief 主动断开连接（不发送 Termination）
 */
void bgp_bmp_disconnect(bgp_bmp_instance_t *inst, int epoll_fd);

// ============================================================================
// 报文发送 API（worker 线程内调用）
// ============================================================================

/**
 * @brief 发送 Initiation 消息
 */
void bgp_bmp_send_initiation(bgp_bmp_instance_t *inst);

/**
 * @brief 发送 Termination 消息
 */
void bgp_bmp_send_termination(bgp_bmp_instance_t *inst);

/**
 * @brief 发送 Peer Up 通知
 * @param inst BMP 实例
 * @param sess 进入 ESTABLISHED 的 BGP 会话
 */
void bgp_bmp_send_peer_up(bgp_bmp_instance_t *inst, bgp_session_t *sess);

/**
 * @brief 发送 Peer Down 通知
 * @param inst BMP 实例
 * @param sess 离开 ESTABLISHED 的 BGP 会话
 * @param reason Peer Down 原因码
 */
void bgp_bmp_send_peer_down(bgp_bmp_instance_t *inst, bgp_session_t *sess, uint8_t reason);

/**
 * @brief 发送 Route Monitoring（封装原始 BGP UPDATE PDU）
 * @param inst BMP 实例
 * @param sess 来源 BGP 会话
 * @param bgp_pdu 原始 BGP UPDATE 报文（含 19 字节 BGP header）
 * @param pdu_len 报文长度
 */
void bgp_bmp_send_route_monitoring(bgp_bmp_instance_t *inst, bgp_session_t *sess, const uint8_t *bgp_pdu,
                                   uint16_t pdu_len);

/**
 * @brief 发送 Stats Report
 */
void bgp_bmp_send_stats_report(bgp_bmp_instance_t *inst);

/**
 * @brief 处理 Stats Report 定时器触发
 */
void bgp_bmp_handle_stats_timer(bgp_bmp_instance_t *inst);

// ============================================================================
// 事件钩子（供 bgp_fsm / bgp_session / bgp_pkt 调用）
// ============================================================================

/**
 * @brief 邻居进入 ESTABLISHED 后的 BMP 通知
 * @param sess 目标会话
 */
void bgp_bmp_notify_peer_up(bgp_session_t *sess);

/**
 * @brief 邻居离开 ESTABLISHED 前的 BMP 通知
 * @param sess 目标会话
 * @param reason Peer Down 原因码
 */
void bgp_bmp_notify_peer_down(bgp_session_t *sess, uint8_t reason);

/**
 * @brief 收到 UPDATE 报文后的 BMP Route Monitoring 通知
 * @param sess 来源会话
 * @param bgp_pdu 原始 BGP UPDATE PDU
 * @param pdu_len 报文长度
 */
void bgp_bmp_notify_route_monitoring(bgp_session_t *sess, const uint8_t *bgp_pdu, uint16_t pdu_len);

// ============================================================================
// 全局管理 API
// ============================================================================

/**
 * @brief 清理所有 BMP 实例（BGP 协议删除时调用）
 * @param epoll_fd worker 的 epoll fd
 */
void bgp_bmp_cleanup_all(int epoll_fd);

#endif /* BGP_BMP_H */
