/**
 * @file   route_api.c
 * @brief  Route 模块对外 API 实现
 * @author jhb
 * @date   2026/02/01
 */
#include <stdio.h>

#include "route_main.h"
#include "dev.h"
#include "ipc.h"

// ============================================================================
// Module Entry Function（动态加载入口）
// ============================================================================

ipc_context_t *route_module_init(void)
{
    printf("[route] 模块入口函数执行\n");
    return ipc_init(DEV_MODULE_ID_ROUTE, "route", NULL, route_ipc_msg_handler);
}
