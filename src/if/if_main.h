/**
 * @file   if_main.h
 * @brief  接口模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef IF_MAIN_H
#define IF_MAIN_H

#include "dev.h"
#include "ipc.h"

typedef struct
{
    ipc_context_t *ipc_ctx;
} if_local_t;

extern if_local_t *g_if_local;

#endif // IF_MAIN_H
