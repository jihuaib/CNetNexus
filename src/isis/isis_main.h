/**
 * @file   isis_main.h
 * @brief  ISIS 模块主入口头文件
 * @author jhb
 * @date   2026/04/11
 */
#ifndef ISIS_MAIN_H
#define ISIS_MAIN_H

#include "dev.h"

typedef struct isis_local
{
    dev_ipc_context_t *dev_ipc_ctx;
} isis_local_t;

extern isis_local_t *g_isis_local;

static inline dev_ipc_context_t *isis_local_ipc_ctx(void)
{
    return g_isis_local->dev_ipc_ctx;
}

/** ISIS 内部消息：IF 模块就绪（含初次 + 重启后），worker 线程上调 if_api_subscribe_all 重新订阅
 *  category=ISIS, subtype=0xFFFE */
#define ISIS_MSG_TYPE_INTERNAL_IF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ISIS, 0xFFFE)

/** ISIS 内部消息：ROUTE 模块就绪（含初次 + 重启后），worker 线程重刷 ISIS 路由
 *  category=ISIS, subtype=0xFFFD */
#define ISIS_MSG_TYPE_INTERNAL_ROUTE_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ISIS, 0xFFFD)

/** ISIS 内部消息：IF 模块下线（DEV_MODULE_EVENT_DOWN），worker 线程清空 IF 缓存并
 *  立即拆除所有邻接、撤销所有 ISIS 路由，避免靠 hello hold-time 走 ~9s 黑洞。
 *  category=ISIS, subtype=0xFFFC */
#define ISIS_MSG_TYPE_INTERNAL_IF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ISIS, 0xFFFC)

void isis_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int isis_module_init(void);
void isis_module_cleanup(void);

#endif /* ISIS_MAIN_H */
