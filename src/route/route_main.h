/**
 * @file   route_main.h
 * @brief  Route 模块主入口头文件
 * @author jhb
 * @date   2026/02/01
 */
#ifndef ROUTE_MAIN_H
#define ROUTE_MAIN_H

#include "ipc.h"

typedef struct route_local
{
    ipc_context_t *ipc_ctx;
    volatile int running;
} route_local_t;

extern route_local_t *g_route_local;

int route_init(void);
void route_cleanup(void);

#endif // ROUTE_MAIN_H
