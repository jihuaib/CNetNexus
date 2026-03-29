/**
 * @file   bgp_worker.h
 * @brief  BGP worker 线程与命令队列
 */
#ifndef BGP_WORKER_H
#define BGP_WORKER_H

#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgp.h"
#include "bgp_bmp.h"
#include "bgp_instance.h"
#include "bgp_protocol.h"
#include "dev.h"
#include "if_event.h"
#include "net_addr.h"

typedef struct bgp_session bgp_session_t;

typedef struct bgp_peer_update_ingest_stats
{
    uint32_t reach_injected;
    uint32_t reach_failed;
    uint32_t unreach_injected;
    uint32_t unreach_failed;
} bgp_peer_update_ingest_stats_t;

/**
 * @brief BGP worker 本地状态（仅 work 子系统持有生命周期）
 */
typedef struct bgp_work_local
{
    bgp_protocol_t *protocol; /**< BGP 协议结构（bgp 使能后非 NULL） */

    /* BGP TCP worker */
    int epoll_fd;            /**< BGP worker epoll fd */
    int running;             /**< worker 线程运行标志 */
    pthread_t worker_thread; /**< BGP worker 线程句柄 */

    int listen_fd; /**< 全局 0.0.0.0:179 listen socket fd，-1 表示未监听 */

    /* BMP 实例哈希表（inst_name -> bgp_bmp_instance_t*） */
    GHashTable *bmp_instances; /**< BMP 实例运行态，key=实例名字符串 */

    /* IPC worker -> BGP worker 命令投递（eventfd + queue） */
    int cmd_eventfd;        /**< 命令唤醒 eventfd，-1 表示未创建 */
    GAsyncQueue *cmd_queue; /**< 队列元素：bgp_worker_cmd_t*（定义在 work/bgp_worker.c） */
} bgp_work_local_t;

extern bgp_work_local_t *g_bgp_work_local;

// ============================================================================
// 跨线程配置应用命令（work 线程填写输入，server 线程填写输出）
// ============================================================================

/** 应用命令执行结果码 */
typedef enum bgp_apply_rc
{
    BGP_APPLY_RC_OK = 0,    /**< 成功，已应用，work 线程需写 DB */
    BGP_APPLY_RC_NOOP = 1,  /**< 同配置，无变更，work 线程无需写 DB */
    BGP_APPLY_RC_FAIL = -1, /**< 参数或状态错误 */
} bgp_apply_rc_t;

/**
 * @brief 跨线程配置/查询命令结构体
 *
 * 栈分配即可。work 线程填写公共字段和联合体输入分支，
 * 调用 bgp_worker_dispatch_apply() 同步阻塞，
 * 返回后 rc、out 和 errmsg 由 server 线程已填写。
 */
typedef struct bgp_apply_cmd
{
    /* ---- 公共字段 ---- */
    uint32_t group_id; /**< CLI group_id（使用 BGP_CLI_GROUP_ID_*） */
    bool isNo;         /**< 是否 no 命令 */
    uint32_t vrf_id;   /**< VRF ID（PROTOCOL 可不填，其余必填） */

    /* ---- group_id 特定输入参数（联合体，按 group_id 选择对应分支） ---- */
    union
    {
        /** BGP_CLI_GROUP_ID_PROTOCOL */
        struct
        {
            uint32_t as_number; /**< 本地 AS 号 */
        } protocol;

        /** BGP_CLI_GROUP_ID_NEIGHBOR */
        struct
        {
            net_addr_t addr;    /**< 邻居 IP 地址 */
            uint32_t remote_as; /**< 对端 AS 号 */
        } neighbor;

        /** BGP_CLI_GROUP_ID_ADDR_FAMILY */
        struct
        {
            bgp_afi_t afi;   /**< 地址族 */
            bgp_safi_t safi; /**< 子地址族 */
        } instance;

        /** BGP_CLI_GROUP_ID_AF_NEIGHBOR */
        struct
        {
            net_addr_t addr; /**< 邻居 IP 地址 */
            bgp_afi_t afi;   /**< 地址族 */
            bgp_safi_t safi; /**< 子地址族 */
        } af_neighbor;

        /** BGP_CLI_GROUP_ID_ROUTER_ID */
        struct
        {
            char id[16]; /**< Router-ID 点分十进制字符串 */
        } router_id;

        /** BGP_CLI_GROUP_ID_TIMERS */
        struct
        {
            uint16_t keepalive; /**< Keepalive 时间（秒） */
            uint16_t hold_time; /**< Hold-Time（秒） */
        } timers;

        /** BGP_CLI_GROUP_ID_CONNECT_RETRY */
        struct
        {
            uint16_t interval; /**< Connect-Retry 时间（秒） */
        } connect_retry;

        /** BGP_CLI_GROUP_ID_OPEN_CAP */
        struct
        {
            net_addr_t addr;  /**< 邻居 IP 地址 */
            uint32_t cap_bit; /**< 能力标志位（BGP_SESS_CAP_*） */
        } open_cap;

        /** BGP_CLI_GROUP_ID_IMPORT_ROUTE */
        struct
        {
            bgp_afi_t afi;         /**< 地址族 */
            bgp_safi_t safi;       /**< 子地址族 */
            uint32_t import_proto; /**< 协议索引（ROUTE_PROTOCOL_*） */
        } import_route;

        /** BGP_CLI_GROUP_ID_SOURCE_IF */
        struct
        {
            net_addr_t addr;                   /**< 邻居 IP 地址 */
            char if_name[IF_LOGICAL_NAME_MAX]; /**< source-interface 逻辑接口名 */
            net_addr_t source_addr;            /**< 解析出的源地址（server 线程回填） */
        } source_if;

        /** BGP_CLI_GROUP_ID_EBGP_MULTIHOP */
        struct
        {
            net_addr_t addr; /**< 邻居 IP 地址 */
            uint8_t ttl;     /**< eBGP multihop TTL（1-255，0 表示未配置） */
        } ebgp_multihop;

        /** BGP_CLI_GROUP_ID_BMP_INSTANCE */
        struct
        {
            char name[BGP_BMP_INST_NAME_MAX]; /**< BMP 实例名 */
        } bmp_instance;

        /** BGP_CLI_GROUP_ID_BMP_COLLECTOR */
        struct
        {
            char inst_name[BGP_BMP_INST_NAME_MAX]; /**< BMP 实例名 */
            net_addr_t addr;                       /**< 采集器 IP 地址 */
            uint16_t port;                         /**< 采集器端口 */
        } bmp_collector;

        /** BGP_CLI_GROUP_ID_BMP_STATS_INTERVAL */
        struct
        {
            char inst_name[BGP_BMP_INST_NAME_MAX]; /**< BMP 实例名 */
            uint16_t interval;                     /**< 间隔（秒） */
        } bmp_stats;

        /** BGP_CLI_GROUP_ID_BMP_RECONNECT */
        struct
        {
            char inst_name[BGP_BMP_INST_NAME_MAX]; /**< BMP 实例名 */
            uint16_t interval;                     /**< 间隔（秒） */
        } bmp_reconnect;

        /** BGP_CLI_GROUP_ID_BMP_MONITOR */
        struct
        {
            char inst_name[BGP_BMP_INST_NAME_MAX]; /**< BMP 实例名 */
            net_addr_t addr;                       /**< 邻居 IP（is_all=TRUE 时无效） */
            gboolean is_all;                       /**< TRUE 表示 "monitor neighbor all" */
        } bmp_monitor;
    } u;

    /* ---- 输出字段（server 线程填写，work 线程在 dispatch 返回后读取） ---- */
    bgp_apply_rc_t rc; /**< 执行结果码 */
    union
    {
        uint32_t sess_flags;    /**< OPEN_CAPABILITY: 更新后的 sess->flags */
        uint32_t import_protos; /**< IMPORT_ROUTE: 更新后的 inst->import_protos */
    } out;
    char errmsg[256]; /**< 失败时的错误描述 */
} bgp_apply_cmd_t;

/**
 * @brief 向 server 线程同步派发结构化配置应用命令
 *
 * 函数阻塞直到 server 线程完成处理。返回后 apply->rc 和输出字段已填写。
 * @return 0 成功提交并处理，-1 提交失败（队列不可用）
 */
int bgp_worker_dispatch_apply(bgp_apply_cmd_t *apply);

// ============================================================================
// server 线程管理 API
// ============================================================================

/**
 * @brief 初始化 runtime server 资源（epoll + cmd channel）
 * @return ERRCODE_SUCCESS / ERRCODE_FAIL
 */
int bgp_worker_prepare(void);

/**
 * @brief 启动 runtime server 线程
 * @return ERRCODE_SUCCESS / ERRCODE_FAIL
 */
int bgp_worker_launch(void);

/**
 * @brief worker 线程投递 show CLI 消息给 server 线程
 *
 * 仅用于 show 命令分发：
 * - CLI_MSG_TYPE（show 命令）
 * - CLI_MSG_TYPE_CONTINUE（分片继续）
 *
 * @return 0 成功，-1 失败
 */
int bgp_worker_post_show_cli(dev_ipc_message_t *msg);

/**
 * @brief 处理 show CLI 消息（在 BGP worker 线程调用）
 */
int bgp_work_handle_show_msg(dev_ipc_message_t *msg);

/**
 * @brief 处理 show CONTINUE 消息（在 BGP worker 线程调用）
 */
int bgp_work_handle_continue_msg(dev_ipc_message_t *msg);

/**
 * @brief 清理 show 分片状态（仅 worker 生命周期清理路径调用）
 */
void bgp_work_show_cleanup(void);

/**
 * @brief worker 线程投递 ROUTE 更新消息给 server 线程
 *
 * 支持 ROUTE_MSG_TYPE_UPDATE / ROUTE_MSG_TYPE_REPORT。
 *
 * @return 0 成功，-1 失败
 */
int bgp_worker_post_route_message(dev_ipc_message_t *msg);

/**
 * @brief 将对端 UPDATE 写入 BGP relay（维护 nexthop<->route 关系，并向 ROUTE 注册 nexthop 门禁）
 */
void bgp_worker_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd,
                                   bgp_peer_update_ingest_stats_t *stats);

/**
 * @brief 清理指定 source 的 relay 路由（peer down / 邻居删除时调用）
 */
void bgp_worker_flush_peer_routes(uint32_t vrf_id, const net_addr_t *source);

/**
 * @brief 启动/停止 BGP 179 监听（仅 worker 线程调用）
 */
void bgp_listen_start(void);
void bgp_listen_stop(void);

/**
 * @brief 会话连接控制（仅 worker 线程调用）
 */
void bgp_server_start_active_conn(bgp_session_t *session);
void bgp_server_stop_session_conns(bgp_session_t *session);

/* 前向声明，避免引入 bgp_vrf.h */
struct bgp_vrf;

/**
 * @brief 重置 VRF 内所有有活跃连接的 session（router-id / timer 变更后调用）
 */
void bgp_server_reset_all_sessions(struct bgp_vrf *vrf);

/**
 * @brief 按当前 VRF connect-retry 配置，重排已挂起的 retry 定时器
 */
void bgp_server_rearm_retry_timers(struct bgp_vrf *vrf);

/**
 * @brief 停止 runtime server（发送 shutdown、join、释放 server 资源）
 */
void bgp_worker_shutdown(void);

#endif /* BGP_WORKER_H */
