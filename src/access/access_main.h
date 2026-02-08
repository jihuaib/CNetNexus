/**
 * @file   access_main.h
 * @brief  Access 模块主入口头文件
 * @author jhb
 * @date   2026/02/08
 */
#ifndef ACCESS_MAIN_H
#define ACCESS_MAIN_H

#include "ipc.h"
#include "cli_engine.h"

typedef struct access_local
{
    ipc_context_t *ipc_ctx;
    cli_engine_context_t *cli_ctx;
    volatile int running;
} access_local_t;

extern access_local_t *g_access_local;

int access_init(void);
void access_cleanup(void);

#endif // ACCESS_MAIN_H
