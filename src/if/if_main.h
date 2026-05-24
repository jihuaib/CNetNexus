/**
 * @file   if_main.h
 * @brief  接口模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_MAIN_H
#define IF_MAIN_H

#include "dev.h"

/** IF 内部消息：VRF 每次 READY（含初次 + 重启）触发 worker 重新订阅 VRF 事件
 *  category=IF, subtype=0xFFFE */
#define IF_MSG_TYPE_INTERNAL_VRF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_IF, 0xFFFE)

/** IF 内部消息：VRF 模块 DOWN，worker 清接口 VRF 绑定 + 清 cache
 *  category=IF, subtype=0xFFFA */
#define IF_MSG_TYPE_INTERNAL_VRF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_IF, 0xFFFA)

/** IF 内部消息：ROUTE 每次 READY（含初次 + 重启）触发 worker 重刷 connected 路由
 *  category=IF, subtype=0xFFFD */
#define IF_MSG_TYPE_INTERNAL_ROUTE_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_IF, 0xFFFD)

/** IF 内部消息：DB 模块就绪，worker 线程做 db_init + db_restore
 *  category=IF, subtype=0xFFFC */
#define IF_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_IF, 0xFFFC)

/**
 * @brief IF 模块 IPC 线程本地上下文（仅保留 IPC 相关，不包含业务数据）
 *
 * 业务数据（interface_map / subscribers / show_stream）由 worker 线程独占，
 * 定义在 work/if_worker.h 的 if_work_local_t 中。
 */
typedef struct
{
    dev_ipc_context_t *dev_ipc_ctx;
} if_local_t;

extern if_local_t *g_if_local;

/**
 * @brief 获取 IF 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *if_local_ipc_ctx(void)
{
    return g_if_local->dev_ipc_ctx;
}

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
void if_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief IF 模块初始化（由 if_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int if_module_init(void);

/**
 * @brief IF 模块清理（由 if_proc.c main() 退出前调用）
 */
void if_module_cleanup(void);

#endif // IF_MAIN_H
