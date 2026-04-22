/**
 * @file   if_msg.h
 * @brief  IF 订阅/查询类消息的 worker 处理入口
 * @author jhb
 * @date   2026/04/22
 */
#ifndef IF_MSG_H
#define IF_MSG_H

#include "dev.h"

/**
 * @brief 处理订阅请求（worker 线程执行，访问 g_if_work_local->subscribers）
 *        内部消费 msg 所有权。
 */
void if_msg_handle_subscribe(dev_ipc_message_t *msg);

/**
 * @brief 处理取消订阅请求（worker 线程执行）
 *        内部消费 msg 所有权。
 */
void if_msg_handle_unsubscribe(dev_ipc_message_t *msg);

#endif /* IF_MSG_H */
