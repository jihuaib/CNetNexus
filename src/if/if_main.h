/**
 * @file   if_main.h
 * @brief  接口模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_MAIN_H
#define IF_MAIN_H

#include "dev.h"

typedef struct
{
    dev_ipc_context_t *dev_ipc_ctx;
} if_local_t;

extern if_local_t *g_if_local;

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void if_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

#endif // IF_MAIN_H
