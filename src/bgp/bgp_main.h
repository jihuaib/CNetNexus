/**
 * @file   bgp_main.h
 * @brief  BGP 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef BGP_MAIN_H
#define BGP_MAIN_H

#include "ipc.h"

typedef struct bgp_local
{
    ipc_context_t *ipc_ctx; /**< IPC 上下文 */
    volatile int running;      /**< 运行标志 */
} bgp_local_t;

extern bgp_local_t *g_bgp_local;

/**
 * @brief 初始化 BGP 模块
 * @return 成功返回 0，失败返回 -1
 */
int bgp_init(void);

/**
 * @brief 清理 BGP 模块
 */
void bgp_cleanup(void);

#endif // BGP_MAIN_H
