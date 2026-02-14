/**
 * @file   bgp_main.h
 * @brief  BGP 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef BGP_MAIN_H
#define BGP_MAIN_H

#include "dev.h"
#include "ipc.h"

typedef struct bgp_local
{
    ipc_context_t *ipc_ctx;
} bgp_local_t;

extern bgp_local_t *g_bgp_local;

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void bgp_msg_handler(ipc_context_t *ctx, ipc_message_t *msg);

#endif // BGP_MAIN_H
