/**
 * @file   isis_main.h
 * @brief  ISIS 模块主入口头文件
 * @author jhb
 * @date   2026/04/11
 */
#ifndef ISIS_MAIN_H
#define ISIS_MAIN_H

#include <signal.h>

#include "dev.h"

typedef struct isis_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    volatile sig_atomic_t shutting_down; /**< 进入 cleanup 阶段:msg_handler 全部丢弃,避免 worker 已销毁仍被投递 */
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

/** ISIS 内部消息：DB 模块就绪，worker 线程做 db_init + db_restore */
#define ISIS_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ISIS, 0xFFFB)

/** ISIS 内部消息：ROUTE 模块下线，清除 locator 快照并触发 LSP 撤销。 */
#define ISIS_MSG_TYPE_INTERNAL_ROUTE_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ISIS, 0xFFFA)

void isis_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);
int isis_module_init(void);
void isis_module_cleanup(void);

#endif /* ISIS_MAIN_H */
