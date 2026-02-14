/**
 * @file   if_api.c
 * @brief  IF 模块对外 API 实现
 * @author jhb
 * @date   2026/02/14
 */
#include <stdio.h>

#include "if_main.h"
#include "dev.h"
#include "ipc.h"

// ============================================================================
// Module Entry Function（动态加载入口）
// ============================================================================

ipc_context_t *if_module_init(void)
{
    printf("[if] 模块入口函数执行\n");
    return ipc_init(DEV_MODULE_ID_IF, "if", NULL, if_msg_handler);
}
