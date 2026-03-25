/**
 * @file   route_main.h
 * @brief  Route 模块主入口头文件
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_MAIN_H
#define ROUTE_MAIN_H

#include <glib.h>

#include "cli.h"
#include "dev.h"
#include "net_addr.h"
#include "route_rib.h"

/**
 * @brief 批量路由追踪条目（内存 RIB + DB route_batch 表，以 name 为主键）
 */
typedef struct route_batch_entry
{
    char name[64];           /**< batch 名称（DB 主键） */
    uint32_t vrf_id;         /**< VRF ID */
    uint16_t afi;            /**< 地址族 */
    uint8_t prefix_len;      /**< 前缀长度 */
    uint8_t _pad;            /**< 填充对齐 */
    net_addr_t prefix_addr;  /**< 前缀地址（二进制） */
    net_addr_t nexthop_addr; /**< 下一跳地址（二进制） */
} route_batch_entry_t;

/**
 * @brief Route 模块本地状态
 */
typedef struct route_local
{
    dev_ipc_context_t *dev_ipc_ctx; /**< IPC 上下文 */
    cli_chunk_stream_t show_stream; /**< CLI show 命令分片输出状态 */
    volatile int running;           /**< 运行标志 */
    route_rib_t *rib;               /**< 内存路由信息库 */
    GList *subscribers;             /**< 订阅者列表 GList<route_subscriber_t*> */
    GList *batch_entries;           /**< 批量路由条目列表 GList<route_batch_entry_t*> */
} route_local_t;

extern route_local_t *g_route_local;

/**
 * @brief 获取 Route 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *route_local_ipc_ctx(void)
{
    return g_route_local->dev_ipc_ctx;
}

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void route_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief Route 模块初始化（由 route_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int route_module_init(void);

/**
 * @brief 统一入口：向 RIB 添加一条路径，并向订阅者发送增量通知
 *
 * @param vrf_id      VRF ID
 * @param afi         地址族
 * @param prefix_addr 前缀地址
 * @param prefix_len  前缀长度
 * @param protocol    协议来源
 * @param source_addr 路径来源地址
 * @param nexthop_addr 下一跳地址
 * @param metric      度量
 * @param preference  管理距离/优先级
 * @return <0 失败；0 表示更新已有路径；>0 表示新增路径（通知失败不会回滚）
 */
int route_add_and_notify(uint32_t vrf_id, uint16_t afi, const net_addr_t *prefix_addr, uint8_t prefix_len,
                         uint32_t protocol, const net_addr_t *source_addr, const net_addr_t *nexthop_addr,
                         int32_t metric, int32_t preference, uint32_t out_ifindex);

/**
 * @brief 全量重算“已注册 nexthop watch”的可达性，并按状态变化回推
 */
void route_recompute_iter_paths(void);

#endif /* ROUTE_MAIN_H */
