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
#include "bgp_bmp_thread.h"
#include "bgp_instance.h"
#include "bgp_protocol.h"
#include "dev.h"
#include "if.h"
#include "net_addr.h"
#include "vrf.h"

typedef struct bgp_session bgp_session_t;

/** 每次工作事件处理时每个数据队列处理的最大条目数 */
#define BGP_WORK_BATCH_SIZE 64

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

    /* IPC worker -> BGP worker 命令投递（eventfd + queue） */
    int cmd_eventfd;         /**< 命令唤醒 eventfd，-1 表示未创建 */
    GAsyncQueue *cmd_queue;  /**< 队列元素：bgp_worker_cmd_t*（定义在 work/bgp_worker.c） */
    int work_eventfd;        /**< 工作事件唤醒 eventfd，-1 表示未创建 */
    GAsyncQueue *work_queue; /**< 工作事件队列 */

    /**
     * @brief 延迟释放的 bgp_conn_t 列表
     *
     * epoll 事件处理过程中关闭的连接不能立即 g_free（同批 epoll 事件可能仍持有
     * 该指针），因此先 close(fd) + 设 fd=-1 后加入此列表，epoll 循环每轮结束后
     * 统一调用 bgp_conn_flush_deferred() 释放（定义在 bgp_conn.c）。
     */
    GSList *deferred_conns;
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
    uint32_t group_id;               /**< CLI group_id（使用 BGP_CLI_GROUP_ID_*） */
    bool isNo;                       /**< 是否 no 命令 */
    char vrf_name[VRF_NAME_MAX_LEN]; /**< CLI/DB 传入的 VRF 名称上下文 */

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

        /** BGP_CLI_GROUP_ID_QP_ROUTE */
        struct
        {
            bgp_afi_t afi;       /**< 地址族 */
            bgp_safi_t safi;     /**< 子地址族（必为 BGP_SAFI_QP） */
            uint32_t start_dqpn; /**< 起始 DQPN */
            uint32_t count;      /**< 路由条数 */
            uint8_t mask_len;    /**< 前缀长度 */
            net_addr_t ip;       /**< 前缀基地址 */
            net_addr_t bid;      /**< BID（IPv6 下一跳） */
        } qp_route;

        /** BGP_CLI_GROUP_ID_ROUTE_SELECT */
        struct
        {
            bgp_afi_t afi;   /**< 地址族 */
            bgp_safi_t safi; /**< 子地址族（必为 BGP_SAFI_QP） */
        } route_select;

        /** BGP_CLI_GROUP_ID_REFRESH */
        struct
        {
            net_addr_t addr; /**< 邻居 IP 地址 */
            bgp_afi_t afi;   /**< 地址族 */
            bgp_safi_t safi; /**< 子地址族 */
            bool is_export;  /**< TRUE=export(本端重发)，FALSE=import(发 REFRESH 拉取) */
        } refresh;

        /** BGP_CLI_GROUP_ID_CLUSTER_ID（per-AF） */
        struct
        {
            bgp_afi_t afi;       /**< 地址族 */
            bgp_safi_t safi;     /**< 子地址族 */
            uint32_t cluster_id; /**< Cluster-ID 主机序 32 位（0=未配置） */
        } cluster_id;

        /** BGP_CLI_GROUP_ID_REFLECT_CLIENT */
        struct
        {
            net_addr_t addr; /**< 邻居 IP 地址 */
            bgp_afi_t afi;   /**< 地址族 */
            bgp_safi_t safi; /**< 子地址族 */
        } reflect_client;

        /** BGP_CLI_GROUP_ID_IMPORT_RIB */
        struct
        {
            bgp_afi_t afi;   /**< 地址族 */
            bgp_safi_t safi; /**< 子地址族（应为 BGP_SAFI_UNICAST） */
            uint32_t src;    /**< bgp_import_src_t 值 */
        } import_rib;

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
        uint32_t sess_flags;         /**< OPEN_CAPABILITY: 更新后的 sess->flags */
        uint32_t import_protos;      /**< IMPORT_ROUTE: 更新后的 inst->import_protos */
        uint32_t import_rib_sources; /**< IMPORT_RIB: 更新后的 inst->import_rib_sources */
        struct
        {
            uint32_t import_protos; /**< 更新后的 inst->import_protos */
        } import_route;
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
 * @brief 向 BGP worker 投递一条 calc 工作事件
 */
int bgp_worker_post_calc_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 向 BGP worker 投递一条 route-flush 工作事件
 */
int bgp_worker_post_route_flush_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 向 BGP worker 投递一条 session-pub 工作事件
 */
int bgp_worker_post_session_pub_event(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

/**
 * @brief 在 worker 线程内同步抽干所有待处理工作事件
 */
void bgp_worker_drain_work_events(void);

/**
 * @brief 判断当前线程是否为 BGP worker 线程
 *
 * 用于各 schedule_* 函数在投递 eventfd 失败时回退到同步处理（worker 线程内调用时）。
 */
gboolean bgp_worker_is_current_thread(void);

/**
 * @brief 在 worker 线程本地 protocol 下按 (vrf_id, afi, safi) 查找实例
 *
 * 共享给 bgp_calc / bgp_route_flush / bgp_update_group 使用。
 *
 * @return bgp_instance_t* 或 NULL
 */
bgp_instance_t *bgp_worker_lookup_instance(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi);

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
 * @brief worker 线程投递 TUNNEL 更新消息给 server 线程
 *
 * 支持 TUNNEL_MSG_TYPE_RESOLVE_NOTIFY。
 *
 * @return 0 成功，-1 失败
 */
int bgp_worker_post_tunnel_message(dev_ipc_message_t *msg);

/**
 * @brief 向 BGP worker 投递 IF 接口事件消息
 *
 * 由 IPC 线程收到 IF_MSG_TYPE_EVENT 后调用，转发到 worker 线程更新本地接口缓存。
 *
 * @return 0 成功，-1 失败
 */
int bgp_worker_post_if_event(dev_ipc_message_t *msg);

/**
 * @brief 向 BGP worker 投递 VRF 事件消息
 *
 * 由 IPC 线程收到 VRF_MSG_TYPE_EVENT 后调用，转发到 worker 线程更新 VRF 缓存
 * 并联动 bgp_rd_entry_t 等内部结构。
 */
int bgp_worker_post_vrf_event(dev_ipc_message_t *msg);

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
 * @brief 处理 BMP 初始 peer 收集请求
 *
 * 由 BMP 线程调用，在 BGP worker 线程中遍历所有 ESTABLISHED 邻居，
 * 生成 peer_info 快照并异步投递到 BMP 线程。
 *
 * @param inst_name 目标 BMP 实例名
 */
void bgp_worker_handle_bmp_initial_peers(const char *inst_name);

/**
 * @brief 停止 runtime server（发送 shutdown、join、释放 server 资源）
 */
void bgp_worker_shutdown(void);

#endif /* BGP_WORKER_H */
