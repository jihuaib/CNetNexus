/**
 * @file   route_relay.h
 * @brief  Route nexthop 迭代 relay（注册/回调）
 */
#ifndef ROUTE_RELAY_H
#define ROUTE_RELAY_H

#include "dev.h"

/**
 * @brief 全量重算已注册 nexthop 的可达性，并按变化回推 owner 模块
 */
void route_recompute_iter_paths(dev_ipc_context_t *ctx);

/**
 * @brief 处理 nexthop 注册消息（ROUTE_MSG_TYPE_NH_REGISTER）
 */
void route_relay_handle_nh_register(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief 处理 nexthop 取消注册消息（ROUTE_MSG_TYPE_NH_UNREGISTER）
 */
void route_relay_handle_nh_unregister(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief 清理 relay 内部状态
 */
void route_relay_cleanup(void);

#endif /* ROUTE_RELAY_H */
