/**
 * @file   bgp_api.c
 * @brief  BGP 模块对外 API 实现
 * @author jhb
 * @date   2026/01/22
 */
#include <stdio.h>

#include "bgp_main.h"
#include "dev.h"
#include "ipc.h"

// ============================================================================
// Module Entry Function（动态加载入口）
// ============================================================================

ipc_context_t *bgp_module_init(void)
{
    printf("[bgp] 模块入口函数执行\n");
    return ipc_init(DEV_MODULE_ID_BGP, "bgp", NULL, bgp_msg_handler);
}
