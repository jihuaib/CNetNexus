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

#endif // DEV_MAIN_H
