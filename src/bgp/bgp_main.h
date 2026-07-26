/**
 * @file   bgp_main.h
 * @brief  BGP 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef BGP_MAIN_H
#define BGP_MAIN_H

#include <glib.h>
#include <signal.h>

#include "bgp_conn.h"
#include "dev.h"

/**
 * @brief BGP 模块本地状态
 *
 * epoll 事件通过 data.ptr 直接携带 bgp_conn_t*（连接 fd）或 VRF listener fd 字段地址，
 * 无需额外的 fd → conn 映射表。
 */
typedef struct bgp_local
{
    dev_ipc_context_t *dev_ipc_ctx;
    volatile sig_atomic_t shutting_down; /**< 进入 cleanup 阶段:msg_handler 全部丢弃,避免 worker 已销毁仍被投递 */
} bgp_local_t;

extern bgp_local_t *g_bgp_local;

/**
 * @brief 获取 BGP 模块本地 IPC 上下文（架构保证非空）
 */
static inline dev_ipc_context_t *bgp_local_ipc_ctx(void)
{
    return g_bgp_local->dev_ipc_ctx;
}

/**
 * @brief IPC 消息处理回调（供 API 层引用）
 */
/** BGP 内部消息：IF 模块就绪（含初次 + 重启后），worker 线程做 if_api_subscribe_all */
#define BGP_MSG_TYPE_INTERNAL_IF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_BGP, 0xFFFE)

/** BGP 内部消息：VRF 模块就绪（含初次 + 重启后），worker 线程做 vrf_api_subscribe */
#define BGP_MSG_TYPE_INTERNAL_VRF_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_BGP, 0xFFFD)

/** BGP 内部消息：ROUTE 模块就绪（含初次 + 重启后），worker 线程重订阅/重注册/重下刷 */
#define BGP_MSG_TYPE_INTERNAL_ROUTE_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_BGP, 0xFFFC)

/** BGP 内部消息：IF 模块下线（DEV_MODULE_EVENT_DOWN），worker 线程清 IF 缓存、
 *  对所有 source-if 绑定的会话发 NOTIFICATION 并拆 TCP，触发 nexthop 重注册。
 *  category=BGP, subtype=0xFFFB */
#define BGP_MSG_TYPE_INTERNAL_IF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_BGP, 0xFFFB)

/** BGP 内部消息：DB 模块就绪，worker 线程做 db_init + db_restore */
#define BGP_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_BGP, 0xFFFA)

/** BGP 内部消息：VRF 模块 DOWN，worker 线程拆非 public VRF 业务 + 清 cache
 *  category=BGP, subtype=0xFFF9 */
#define BGP_MSG_TYPE_INTERNAL_VRF_DOWN DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_BGP, 0xFFF9)

/** BGP 内部消息：RPM 模块 READY，重新订阅 BGP 出口策略 */
#define BGP_MSG_TYPE_INTERNAL_RPM_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_BGP, 0xFFF8)

void bgp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg);

/**
 * @brief BGP 模块初始化（由 bgp_proc.c main() 显式调用）
 * @return 0 成功，-1 失败
 */
int bgp_module_init(void);

/**
 * @brief BGP 模块清理（由 bgp_proc.c main() 退出前调用）
 */
void bgp_module_cleanup(void);

#endif /* BGP_MAIN_H */
