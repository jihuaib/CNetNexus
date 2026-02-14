/**
 * @file   dev_main.h
 * @brief  Dev 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef DEV_MAIN_H
#define DEV_MAIN_H

#include "dev.h"
#include "ipc.h"

typedef struct dev_local
{
    ipc_context_t *ipc_ctx;
} dev_local_t;

extern dev_local_t *g_dev_local;

/**
 * @brief DEV 自身初始化（创建 IPC context）
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int dev_init_self(void);

/**
 * @brief 获取 DEV 的 IPC context
 * @return DEV 的 IPC context
 */
ipc_context_t *dev_get_ipc_ctx(void);

/**
 * @brief DEV 清理
 */
void dev_cleanup_self(void);

#endif // DEV_MAIN_H
