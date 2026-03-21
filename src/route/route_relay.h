/**
 * @file   route_relay.h
 * @brief  Route nexthop 迭代 relay（注册/回调）
 */
#ifndef ROUTE_RELAY_H
#define ROUTE_RELAY_H

#include <glib.h>

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
 * @brief 格式化输出 nexthop watch 表到缓冲区
 * @param buf          输出 GString 缓冲区
 * @param module_filter 按 owner_module_id 过滤（0 表示不过滤）
 * @param has_filter   非零表示启用 module_filter 过滤
 */
void route_relay_show(GString *buf, uint32_t module_filter, int has_filter);

/**
 * @brief 清理 relay 内部状态
 */
void route_relay_cleanup(void);

#endif /* ROUTE_RELAY_H */
