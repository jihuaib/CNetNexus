/**
 * @file   dev_subscribe.h
 * @brief  DEV 侧订阅/按需启动接口
 * @author jhb
 * @date   2026/05/21
 */
#ifndef DEV_SUBSCRIBE_H
#define DEV_SUBSCRIBE_H

#include <stdint.h>

#include "dev.h"
#include "dev_module.h"

/**
 * @brief 处理 SUBSCRIBE 请求（DEV msg_handler 路由）
 */
void dev_subscribe_handle_subscribe(dev_ipc_message_t *msg);

/**
 * @brief 处理 UNSUBSCRIBE 通知
 */
void dev_subscribe_handle_unsubscribe(dev_ipc_message_t *msg);

/**
 * @brief 处理 NOTIFY_READY 通知（推送 MODULE_EVENT 给所有订阅者）
 */
void dev_subscribe_handle_notify_ready(dev_ipc_message_t *msg);

/**
 * @brief 向 target 的所有订阅者广播指定事件（READY/DOWN）
 *
 * 用于 NOTIFY_READY 和 SIGCHLD 路径。
 */
void dev_subscribe_broadcast_event(dev_module_t *target, uint8_t event);

#endif // DEV_SUBSCRIBE_H
