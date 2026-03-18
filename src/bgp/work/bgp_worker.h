/**
 * @file   bgp_worker.h
 * @brief  BGP worker 线程与命令队列
 */
#ifndef BGP_WORKER_H
#define BGP_WORKER_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgp_instance.h"
#include "dev.h"
#include "net_addr.h"

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
 * @brief worker 线程投递 ROUTE 更新消息给 server 线程
 *
 * 支持 ROUTE_MSG_TYPE_UPDATE / ROUTE_MSG_TYPE_REPORT。
 *
 * @return 0 成功，-1 失败
 */
int bgp_worker_post_route_message(dev_ipc_message_t *msg);

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

/**
 * @brief 停止 runtime server（发送 shutdown、join、释放 server 资源）
 */
void bgp_worker_shutdown(void);

#endif /* BGP_WORKER_H */
