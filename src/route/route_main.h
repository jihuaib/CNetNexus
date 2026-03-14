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
#include "route_rib.h"

/**
 * @brief 批量路由追踪条目（仅在内存 RIB，不存 DB）
 */
typedef struct route_batch_entry
{
    uint32_t vrf_id;                     /**< VRF ID */
    uint16_t afi;                        /**< 地址族 */
    uint8_t prefix_len;                  /**< 前缀长度 */
    char prefix[ROUTE_RIB_PREFIX_MAX];   /**< 前缀地址 */
    char nexthop[ROUTE_RIB_NEXTHOP_MAX]; /**< 下一跳 */
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
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void route_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief Route 模块初始化（由 route_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int route_module_init(void);

#endif /* ROUTE_MAIN_H */
