/**
 * @file   route_pub.h
 * @brief  Route 模块发布/订阅接口
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_PUB_H
#define ROUTE_PUB_H

#include <glib.h>
#include <stdint.h>

#include "dev.h"
#include "route_rib.h"

/**
 * @brief 路由订阅者信息
 */
typedef struct route_subscriber
{
    uint32_t module_id; /**< 订阅方模块 ID */
    uint32_t protocol;  /**< 订阅的协议（ROUTE_PROTOCOL_MAX = 全部） */
    uint32_t vrf_id;    /**< 订阅的 VRF ID（ROUTE_VRF_ALL = 全部） */
} route_subscriber_t;

/**
 * @brief 向所有匹配的订阅者发送增量路由更新（异步 fire-and-forget）
 * @param ctx         IPC 上下文
 * @param subscribers 订阅者列表 GList<route_subscriber_t*>
 * @param head        前缀头
 * @param path        路径
 * @param is_withdraw 1=撤销路由, 0=新增/更新路由
 */
void route_pub_notify(dev_ipc_context_t *ctx, GList *subscribers, const route_head_t *head, const route_path_t *path,
                      int is_withdraw);

/**
 * @brief 向指定模块发送单条增量路由更新（用于迭代 owner 回推）
 */
void route_pub_notify_module(dev_ipc_context_t *ctx, uint32_t dst_module_id, const route_head_t *head,
                             const route_path_t *path, int is_withdraw);

/**
 * @brief 将 RIB 快照发送给指定模块（作为 SUBSCRIBE 请求的响应）
 * @param ctx         IPC 上下文
 * @param rib         路由 RIB
 * @param dst_module_id 目标模块 ID
 * @param protocol    过滤协议（ROUTE_PROTOCOL_MAX = 全部）
 * @param vrf_id      过滤 VRF ID（ROUTE_VRF_ALL = 全部）
 * @param request_id  原始请求 ID（用于响应配对）
 */
void route_pub_dump(dev_ipc_context_t *ctx, route_rib_t *rib, uint32_t dst_module_id, uint32_t protocol,
                    uint32_t vrf_id, uint32_t request_id);

#endif /* ROUTE_PUB_H */
