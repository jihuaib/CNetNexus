/**
 * @file   bgp_bmp.h
 * @brief  BMP 实例运行态数据结构与 API（BMP 线程内使用）
 * @author jhb
 * @date   2026/03/29
 */
#ifndef BGP_BMP_H
#define BGP_BMP_H

#include <glib.h>
#include <stdint.h>

#include "bgp_bmp_thread.h"
#include "net_addr.h"

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
// BMP 定时器哨兵（BMP 线程专用）
// ============================================================================

/** BMP 定时器类型 */
typedef enum bgp_bmp_timer_type
{
    BMP_TIMER_RECONNECT = 0, /**< 重连定时器 */
    BMP_TIMER_STATS = 1,     /**< Stats Report 周期定时器 */
    BMP_TIMER_CONN = 2,      /**< TCP 连接 fd 事件 */
} bgp_bmp_timer_type_t;

/**
 * @brief BMP epoll 事件哨兵（注册到 epoll data.ptr，bit0=1 标记）
 */
typedef struct bgp_bmp_timer_sentinel
{
    bgp_bmp_timer_type_t type; /**< 定时器类型 */
} bgp_bmp_timer_sentinel_t;

// ============================================================================
// BMP 实例运行态
// ============================================================================

/**
 * @brief BMP 实例运行态结构
 *
 * 每个 BMP 实例对应一条到 collector 的 TCP 连接，
 * 在 BMP 线程中管理连接生命周期和报文发送。
 */
struct bgp_bmp_instance
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
    int reconnect_timerfd;                       /**< 重连定时器 fd，-1 表示未调度 */
    int stats_timerfd;                           /**< Stats Report 定时器 fd，-1 表示未调度 */
    bgp_bmp_timer_sentinel_t reconnect_sentinel; /**< type=BMP_TIMER_RECONNECT */
    bgp_bmp_timer_sentinel_t stats_sentinel;     /**< type=BMP_TIMER_STATS */
    bgp_bmp_timer_sentinel_t conn_sentinel;      /**< type=BMP_TIMER_CONN */

    /* ---- 统计 ---- */
    gint64 connected_at_usec; /**< 连接建立时间（g_get_real_time，0=未连接） */
    uint32_t peer_up_sent;
    uint32_t peer_down_sent;
    uint32_t route_monitor_sent;
    uint32_t stats_report_sent;
    uint32_t initiation_sent;
};

// ============================================================================
// 生命周期 API（BMP 线程内调用）
// ============================================================================

/**
 * @brief 创建 BMP 实例运行态
 */
bgp_bmp_instance_t *bgp_bmp_instance_create(const char *name);

/**
 * @brief 销毁 BMP 实例运行态
 */
void bgp_bmp_instance_destroy(bgp_bmp_instance_t *inst, int epoll_fd);

/**
 * @brief 判断该实例是否应监控指定邻居
 */
gboolean bgp_bmp_should_monitor(const bgp_bmp_instance_t *inst, const char *peer_ip);

// ============================================================================
// 连接管理 API（BMP 线程内调用）
// ============================================================================

void bgp_bmp_connect(bgp_bmp_instance_t *inst, int epoll_fd);
void bgp_bmp_handle_connect_result(bgp_bmp_instance_t *inst, int epoll_fd);
void bgp_bmp_handle_read(bgp_bmp_instance_t *inst, int epoll_fd);
void bgp_bmp_handle_reconnect(bgp_bmp_instance_t *inst, int epoll_fd);
void bgp_bmp_disconnect(bgp_bmp_instance_t *inst, int epoll_fd);

// ============================================================================
// 报文发送 API（BMP 线程内调用，使用 peer_info 快照）
// ============================================================================

void bgp_bmp_send_initiation(bgp_bmp_instance_t *inst);
void bgp_bmp_send_termination(bgp_bmp_instance_t *inst);
void bgp_bmp_send_peer_up(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer);
void bgp_bmp_send_peer_down(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer, uint8_t reason);
void bgp_bmp_send_route_monitoring(bgp_bmp_instance_t *inst, const bgp_bmp_peer_info_t *peer, const uint8_t *bgp_pdu,
                                   uint16_t pdu_len);
void bgp_bmp_send_stats_report(bgp_bmp_instance_t *inst);
void bgp_bmp_handle_stats_timer(bgp_bmp_instance_t *inst);

#endif /* BGP_BMP_H */
