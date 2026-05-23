/**
 * @file   ldp_main.h
 * @brief  LDP 模块主入口头文件
 * @author jhb
 * @date   2026/05/05
 */
#ifndef LDP_MAIN_H
#define LDP_MAIN_H

#include "dev.h"

typedef struct ldp_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} ldp_local_t;

extern ldp_local_t *g_ldp_local;

static inline dev_ipc_context_t *ldp_local_ipc_ctx(void)
{
    return g_ldp_local ? g_ldp_local->dev_ipc_ctx : NULL;
}

/** LDP 内部消息：IF 模块就绪（含初次 + 重启后），worker 线程上调 if_api_subscribe_all 重新订阅
 *  category=LDP, subtype=0xFFFE */
#define LDP_MSG_TYPE_INTERNAL_IF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_LDP, 0xFFFE)

/** LDP 内部消息：ROUTE 模块就绪（含初次 + 重启后），worker 线程重发 ROUTE 订阅
 *  category=LDP, subtype=0xFFFD */
#define LDP_MSG_TYPE_INTERNAL_ROUTE_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_LDP, 0xFFFD)

/** LDP 内部消息：IF 模块下线（DEV_MODULE_EVENT_DOWN），worker 线程清空 IF 缓存、
 *  关闭所有接口 UDP socket、拆所有 LDP hello adjacency 和 TCP 会话，
 *  避免靠 keepalive 超时（默认 ~15s）才感知。
 *  category=LDP, subtype=0xFFFC */
#define LDP_MSG_TYPE_INTERNAL_IF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_LDP, 0xFFFC)

void ldp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int ldp_module_init(void);
void ldp_module_cleanup(void);

#endif /* LDP_MAIN_H */
