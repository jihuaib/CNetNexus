/**
 * @file   route_main.h
 * @brief  Route 模块主入口头文件
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_MAIN_H
#define ROUTE_MAIN_H

#include "ipc.h"

typedef struct route_local
{
    ipc_context_t *ipc_ctx;
    volatile int running;
} route_local_t;

extern route_local_t *g_route_local;

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void route_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg);

#endif // ROUTE_MAIN_H
