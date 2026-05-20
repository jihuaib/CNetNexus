/**
 * @file   bgp_main.h
 * @brief  BGP 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef BGP_MAIN_H
#define BGP_MAIN_H

#include <glib.h>

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
