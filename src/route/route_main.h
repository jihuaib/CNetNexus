/**
 * @file   route_main.h
 * @brief  Route 模块主入口头文件
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_MAIN_H
#define ROUTE_MAIN_H

#include <glib.h>

#include "dev.h"
#include "net_addr.h"

/** ROUTE 内部消息：VRF 每次 READY（含初次 + 重启）触发 worker 重新订阅 VRF 事件
 *  category=ROUTE, subtype=0xFFFE */
#define ROUTE_MSG_TYPE_INTERNAL_VRF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0xFFFE)

/** ROUTE 内部消息：VRF 模块 DOWN，worker 清非 public VRF 的内存业务并清 cache
 *  category=ROUTE, subtype=0xFFFA */
#define ROUTE_MSG_TYPE_INTERNAL_VRF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0xFFFA)

/** ROUTE 内部消息：IF 模块 READY（含初次 + 重启），IPC 线程重新订阅 IF 事件
 *  category=ROUTE, subtype=0xFFFD */
#define ROUTE_MSG_TYPE_INTERNAL_IF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0xFFFD)

/** ROUTE 内部消息：IF 模块 DOWN（process stop/crash），worker 清 IF 缓存并重算 nexthop
 *  category=ROUTE, subtype=0xFFFC */
#define ROUTE_MSG_TYPE_INTERNAL_IF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0xFFFC)

/** ROUTE 内部消息：DB 模块 READY（含初次 + 重启），延后建表并恢复持久化路由
 *  category=ROUTE, subtype=0xFFFB */
#define ROUTE_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_ROUTE, 0xFFFB)

/**
 * @brief Route 模块本地状态（IPC 线程专用）
 *
 * 仅保存 IPC 上下文与运行标志。业务数据（RIB、订阅者、batch 条目）
 * 存放在 g_route_work_local（worker 线程专用）。
 */
typedef struct route_local
{
    dev_ipc_context_t *dev_ipc_ctx; /**< IPC 上下文 */
} route_local_t;

extern route_local_t *g_route_local;

/**
 * @brief 获取 Route 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *route_local_ipc_ctx(void)
{
    return g_route_local->dev_ipc_ctx;
}

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void route_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief Route 模块初始化（由 route_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int route_module_init(void);

/**
 * @brief Route 模块清理（由 route_proc.c main() 退出前调用）
 */
void route_module_cleanup(void);

#endif /* ROUTE_MAIN_H */
