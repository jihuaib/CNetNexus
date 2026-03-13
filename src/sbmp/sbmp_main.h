/**
 * @file   sbmp_main.h
 * @brief  SBMP（BMP Server）模块主入口与运行态数据结构
 * @author jhb
 * @date   2026/03/08
 */
#ifndef SBMP_MAIN_H
#define SBMP_MAIN_H

#include <pthread.h>

#include "bgp_rib.h"
#include "dev.h"

/** BMP 客户端 ID / IP 字符串最大长度 */
#define SBMP_CLIENT_ID_MAX 64
/** Peer IP 字符串最大长度 */
#define SBMP_PEER_IP_MAX 64
/** 客户端 TCP 接收缓冲区大小 */
#define SBMP_RECV_BUF_SIZE 65536

/**
 * @brief BMP route policy 维度
 *
 * RFC 7854 per-peer header 的 L 位：
 * - L=0: pre-policy（Adj-RIB-In）
 * - L=1: post-policy
 *
 * Loc-RIB 使用 RFC 9069 peer-type=3 单独表示。
 */
typedef enum sbmp_route_policy
{
    SBMP_ROUTE_POLICY_PRE = 0,
    SBMP_ROUTE_POLICY_POST = 1,
    SBMP_ROUTE_POLICY_LOC_RIB = 2,
    SBMP_ROUTE_POLICY_MAX
} sbmp_route_policy_t;

/**
 * @brief BMP 客户端视角下的单个 BGP Peer 运行状态
 */
typedef struct sbmp_peer
{
    char peer_ip[SBMP_PEER_IP_MAX];     /**< peer 地址字符串 */
    uint32_t peer_as;                   /**< 最近一次观察到的 peer AS */
    char bgp_id[16];                    /**< 最近一次观察到的 BGP ID */
    gboolean is_loc_rib_filtered;       /**< TRUE=Loc-RIB 且带过滤标记 */
    gboolean is_up;                     /**< TRUE=up, FALSE=down */
    uint64_t up_count;                  /**< Peer Up 次数 */
    uint64_t down_count;                /**< Peer Down 次数 */
    guint64 route_monitor_msgs;         /**< Route Monitoring 报文计数 */
    guint64 route_monitor_pre_msgs;     /**< pre-policy Route Monitoring 报文计数 */
    guint64 route_monitor_post_msgs;    /**< post-policy Route Monitoring 报文计数 */
    guint64 route_monitor_loc_rib_msgs; /**< Loc-RIB Route Monitoring 报文计数 */
    guint64 update_msgs;                /**< Route Monitoring 内 UPDATE 报文计数 */
    guint64 update_pre_msgs;            /**< pre-policy UPDATE 报文计数 */
    guint64 update_post_msgs;           /**< post-policy UPDATE 报文计数 */
    guint64 update_loc_rib_msgs;        /**< Loc-RIB UPDATE 报文计数 */
    guint64 update_parse_errs;          /**< UPDATE 解析失败计数 */
    gint64 last_change_usec;            /**< 状态最近变化时间（usec） */
} sbmp_peer_t;

/**
 * @brief 单个 BMP 客户端运行态
 *
 * client_id 采用客户端源 IP（如 "192.0.2.10"），便于按客户端查询。
 */
typedef struct sbmp_client
{
    char client_id[SBMP_CLIENT_ID_MAX]; /**< 客户端 ID（源 IP） */
    char source_ip[SBMP_CLIENT_ID_MAX]; /**< 当前连接源 IP */
    uint16_t source_port;               /**< 当前连接源端口 */
    int fd;                             /**< 当前连接 fd（-1=未连接） */
    gboolean connected;                 /**< 当前连接状态 */
    gint64 connected_at_usec;           /**< 最近连接建立时间（usec） */
    gint64 last_active_usec;            /**< 最近收到 BMP 报文时间（usec） */

    guint64 msg_total;                  /**< BMP 报文总数 */
    guint64 msg_parse_err;              /**< BMP 报文解析失败计数 */
    guint64 route_monitor_msgs;         /**< Route Monitoring 报文计数 */
    guint64 route_monitor_pre_msgs;     /**< pre-policy Route Monitoring 报文计数 */
    guint64 route_monitor_post_msgs;    /**< post-policy Route Monitoring 报文计数 */
    guint64 route_monitor_loc_rib_msgs; /**< Loc-RIB Route Monitoring 报文计数 */
    guint64 update_pre_msgs;            /**< pre-policy UPDATE 报文计数 */
    guint64 update_post_msgs;           /**< post-policy UPDATE 报文计数 */
    guint64 update_loc_rib_msgs;        /**< Loc-RIB UPDATE 报文计数 */
    guint64 stats_report_msgs;          /**< Stats Report 报文计数 */
    guint64 peer_down_msgs;             /**< Peer Down 报文计数 */
    guint64 peer_up_msgs;               /**< Peer Up 报文计数 */
    guint64 initiation_msgs;            /**< Initiation 报文计数 */
    guint64 termination_msgs;           /**< Termination 报文计数 */
    guint64 route_mirroring_msgs;       /**< Route Mirroring 报文计数 */

    GHashTable *peer_hash;      /**< key=peer_ip(gchar*) value=sbmp_peer_t* */
    bgp_rib_t *rib_v4u_pre;     /**< pre-policy IPv4 unicast 路由（按 peer/source 区分） */
    bgp_rib_t *rib_v4u_post;    /**< post-policy IPv4 unicast 路由（按 peer/source 区分） */
    bgp_rib_t *rib_v4u_loc_rib; /**< Loc-RIB IPv4 unicast 路由（按 peer/source 区分） */
    bgp_rib_t *rib_v6u_pre;     /**< pre-policy IPv6 unicast 路由（按 peer/source 区分） */
    bgp_rib_t *rib_v6u_post;    /**< post-policy IPv6 unicast 路由（按 peer/source 区分） */
    bgp_rib_t *rib_v6u_loc_rib; /**< Loc-RIB IPv6 unicast 路由（按 peer/source 区分） */

    uint8_t recv_buf[SBMP_RECV_BUF_SIZE]; /**< TCP 接收缓冲区 */
    uint32_t recv_len;                    /**< 缓冲区已用字节数 */
} sbmp_client_t;

/**
 * @brief SBMP 模块本地状态
 */
typedef struct sbmp_local
{
    dev_ipc_context_t *dev_ipc_ctx; /**< IPC 上下文 */
    uint16_t server_port;           /**< BMP 监听端口（0 = 未配置） */
    int listen_fd;                  /**< BMP TCP 监听 fd（-1 = 未监听） */

    int epoll_fd;            /**< BMP server epoll fd */
    int running;             /**< server 线程运行标志 */
    pthread_t server_thread; /**< server 线程 */

    pthread_mutex_t runtime_mutex; /**< 运行态数据锁 */
    GHashTable *client_hash;       /**< key=client_id(gchar*) value=sbmp_client_t* */
    GHashTable *fd_hash;           /**< key=GINT_TO_POINTER(fd) value=sbmp_client_t* */
} sbmp_local_t;

/** SBMP 模块全局状态 */
extern sbmp_local_t *g_sbmp_local;

/**
 * @brief IPC 消息处理回调
 */
void sbmp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief 启动 BMP server 监听（绑定指定端口），已监听时幂等
 * @param port 监听端口
 */
void sbmp_listen_start(uint16_t port);

/**
 * @brief 停止 BMP server 监听，未监听时幂等
 */
void sbmp_listen_stop(void);

/**
 * @brief 加锁访问 SBMP 运行态（client/peer/rib）
 */
void sbmp_runtime_lock(void);

/**
 * @brief 解锁 SBMP 运行态
 */
void sbmp_runtime_unlock(void);

/**
 * @brief 在持锁状态下按 client-id 查找客户端
 * @param client_id 客户端 ID（源 IP）
 * @return 客户端指针，不存在返回 NULL
 */
sbmp_client_t *sbmp_runtime_find_client_locked(const char *client_id);

/**
 * @brief SBMP 模块初始化（由 sbmp_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int sbmp_module_init(void);

#endif /* SBMP_MAIN_H */
