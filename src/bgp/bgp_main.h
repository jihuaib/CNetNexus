/**
 * @file   bgp_main.h
 * @brief  BGP 模块主入口头文件
 * @author jhb
 * @date   2026/01/22
 */
#ifndef BGP_MAIN_H
#define BGP_MAIN_H

#include "dev.h"
#include "ipc.h"

typedef struct bgp_local
{
    ipc_context_t *ipc_ctx;
} bgp_local_t;

extern bgp_local_t *g_bgp_local;

#endif // BGP_MAIN_H
