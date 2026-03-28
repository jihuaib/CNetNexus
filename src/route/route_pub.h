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
#include "route.h"
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
 * @param subscribers 订阅者列表 GList<route_subscriber_t*>
 * @param head        前缀头
 * @param path        路径
 * @param is_withdraw 1=撤销路由, 0=新增/更新路由
 */
void route_pub_notify(GList *subscribers, const route_head_t *head, const route_path_t *path, int is_withdraw);

/**
 * @brief 直接使用 route_msg_entry_t 向所有匹配订阅者发送通知
 *
 * 用于 route_calc 等不持有 RIB head/path 指针、但已知完整条目的场景。
 * 订阅者过滤规则与 route_pub_notify 一致（按 protocol 和 vrf_id 匹配）。
 *
 * @param subscribers 订阅者列表 GList<route_subscriber_t*>
 * @param entry       路由条目（is_withdraw 字段已填充）
 */
void route_pub_notify_entry(GList *subscribers, const route_msg_entry_t *entry);

/**
 * @brief 向指定模块发送单条增量路由更新（用于迭代 owner 回推）
 */
void route_pub_notify_module(uint32_t dst_module_id, const route_head_t *head, const route_path_t *path,
                             int is_withdraw);

#endif /* ROUTE_PUB_H */
