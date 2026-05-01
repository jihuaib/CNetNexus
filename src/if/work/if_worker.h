/**
 * @file   if_worker.h
 * @brief  IF work 线程接口：串行处理 IPC 业务、链路监控事件、同步配置应用
 * @author jhb
 * @date   2026/04/21
 */
#ifndef IF_WORKER_H
#define IF_WORKER_H

#include <glib.h>
#include <net/if.h>
#include <pthread.h>
#include <stdint.h>

#include "cli.h"
#include "dev.h"
#include "if_map.h"
#include "if_netlink.h"

/**
 * @brief 配置应用命令操作类型
 */
typedef enum if_apply_op
{
    IF_APPLY_OP_IP_SET = 1,
    IF_APPLY_OP_SHUTDOWN_SET = 2,
    IF_APPLY_OP_LOOP_CREATE = 3,
    IF_APPLY_OP_LOOP_DELETE = 4,
} if_apply_op_t;

/**
 * @brief 配置应用命令载荷（IPC 线程分配，worker 线程执行）
 */
typedef struct if_apply_cmd
{
    if_apply_op_t op;
    int rc;

    union
    {
        struct
        {
            char ifname[LOGICAL_NAME_LEN];
            int is_no;
            net_prefix_t prefix;
        } ip_set;
        struct
        {
            char ifname[LOGICAL_NAME_LEN];
            int is_no;
        } shutdown_set;
        struct
        {
            uint32_t loop_id;
        } loop_create;
        struct
        {
            uint32_t loop_id;
        } loop_delete;
    } u;
} if_apply_cmd_t;

/**
 * @brief 链路监控事件载荷（监听线程投递给 worker）
 */
typedef struct if_work_link_event
{
    uint16_t nlmsg_type;   /**< RTM_NEWLINK / RTM_DELLINK */
    uint32_t ifindex;      /**< NEWLINK 时有效，DELLINK 可为 0 */
    uint8_t link_up_known; /**< 1=携带链路状态，0=未知 */
    uint8_t link_up;       /**< 1=链路UP，0=链路DOWN */
    uint16_t _pad0;
    char physical_name[IFNAMSIZ]; /**< 物理接口名 */
} if_work_link_event_t;

/**
 * @brief 地址监控事件载荷（监听线程投递给 worker）
 */
typedef struct if_work_addr_event
{
    uint16_t nlmsg_type; /**< RTM_NEWADDR / RTM_DELADDR */
    uint16_t _pad0;
    uint32_t ifindex;             /**< 接口索引 */
    uint32_t addr_flags;          /**< 地址标志（IF_ADDR_FLAG_*） */
    char physical_name[IFNAMSIZ]; /**< 物理接口名 */
    net_prefix_t prefix;          /**< 地址前缀 */
} if_work_addr_event_t;

/**
 * @brief IF worker 线程本地状态
 *
 * 业务数据（interface_map / subscribers / show_stream）由 worker 线程独占访问，
 * 由 if_worker_prepare() 初始化，由 if_worker_shutdown() 释放。
 */
typedef struct if_work_local
{
    /* ---- 业务数据（worker 线程独占） ---- */
    if_map_t interface_map;         /**< 接口逻辑名到物理名的映射表 */
    GList *subscribers;             /**< IF 事件订阅者列表 GList<if_subscriber_t*> */
    cli_chunk_stream_t show_stream; /**< CLI show 命令分片输出状态 */

    /* ---- Worker 基础设施 ---- */
    pthread_t thread;
    volatile int running;
    GAsyncQueue *cmd_queue;
} if_work_local_t;

extern if_work_local_t *g_if_work_local;

/**
 * @brief 初始化 worker 业务数据与基础设施（不启动线程）
 * @return ERRCODE_SUCCESS 成功，ERRCODE_FAIL 失败
 */
int if_worker_prepare(void);

/**
 * @brief 启动 worker 线程（须在 if_worker_prepare 之后调用）
 * @return ERRCODE_SUCCESS 成功，ERRCODE_FAIL 失败
 */
int if_worker_launch(void);

/**
 * @brief 判断 worker 是否在运行
 */
int if_worker_is_running(void);

/**
 * @brief 关闭 worker 线程并释放所有资源
 */
void if_worker_shutdown(void);

/**
 * @brief 向 worker 异步投递 IPC 消息（msg 所有权转移）
 *
 * 适用于 CLI 配置/show/continue/query-candidates、IF 订阅/取消订阅/查询接口映射
 * 等需要访问 worker 独占业务数据的消息。
 *
 * @param msg IPC 消息（不可为 NULL），所有权转移给 worker
 * @return ERRCODE_SUCCESS 成功，ERRCODE_FAIL 失败
 */
int if_worker_post_ipc_message(dev_ipc_message_t *msg);

/**
 * @brief 向 worker 异步投递链路事件
 */
int if_worker_post_link_event(uint16_t nlmsg_type, const char *physical_name, uint32_t ifindex, uint8_t link_up_known,
                              uint8_t link_up);

/**
 * @brief 向 worker 异步投递地址事件
 */
int if_worker_post_addr_event(uint16_t nlmsg_type, const char *physical_name, uint32_t ifindex, uint32_t addr_flags,
                              const net_prefix_t *prefix);

/**
 * @brief 向 worker 同步派发配置应用命令（阻塞至 worker 处理完毕）
 *
 * @param apply 配置命令（调用方拥有生命周期）
 * @return ERRCODE_SUCCESS 表示已完成派发，实际应用结果见 apply->rc
 */
int if_worker_dispatch_apply(if_apply_cmd_t *apply);

#endif /* IF_WORKER_H */
