/**
 * @file   if_main.h
 * @brief  接口模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_MAIN_H
#define IF_MAIN_H

#include "ipc.h"

typedef struct if_local
{
    ipc_context_t *ipc_ctx; /**< IPC 上下文 */
    volatile int running;      /**< 运行标志 */
} if_local_t;

extern if_local_t *g_if_local;

/**
 * @brief 初始化 IF 模块
 * @return 成功返回 0，失败返回 -1
 */
int if_init(void);

/**
 * @brief 清理 IF 模块
 */
void if_cleanup(void);

#endif // IF_MAIN_H
