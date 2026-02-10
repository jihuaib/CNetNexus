/**
 * @file   bgp_main.c
 * @brief  BGP 模块主入口，模块注册和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_cli.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "path_utils.h"

bgp_local_t *g_bgp_local = NULL;

// ============================================================================
// IPC 消息处理回调
// ============================================================================

static void bgp_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        case CFG_MSG_TYPE_CLI:
            printf("[bgp] Received CLI command message\n");
            bgp_cli_handle_message(ctx, msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            printf("[bgp] Received CLI continue request\n");
            bgp_cli_handle_continue(ctx, msg);
            break;

        default:
            break;
    }

    ipc_message_free(msg);
}

static int bgp_init_local()
{
    g_bgp_local = g_malloc0(sizeof(bgp_local_t));

    g_bgp_local->ipc_ctx = ipc_init(DEV_MODULE_ID_BGP, "bgp", NULL, bgp_msg_handler);
    if (!g_bgp_local->ipc_ctx)
    {
        fprintf(stderr, "[bgp] Failed to initialize IPC\n");
        return ERRCODE_FAIL;
    }

    /* CFG 会主动连接到 BGP，BGP 只需监听 */

    return ERRCODE_SUCCESS;
}

static void bgp_cleanup_local()
{
    if (g_bgp_local == NULL)
    {
        return;
    }

    printf("[bgp] BGP module cleanup\n");

    if (g_bgp_local->ipc_ctx)
    {
        ipc_destroy(g_bgp_local->ipc_ctx);
    }

    g_free(g_bgp_local);
    g_bgp_local = NULL;
}

// 模块初始化
static int32_t bgp_module_init()
{
    int ret = bgp_init_local();
    if (ret != ERRCODE_SUCCESS)
    {
        bgp_cleanup_local();
        return ERRCODE_FAIL;
    }

    printf("[bgp] BGP module initialized (IPC)\n");
    return ERRCODE_SUCCESS;
}

// 模块清理
static void bgp_module_cleanup(void)
{
    bgp_cleanup_local();
}

// 构造函数注册 BGP 模块
static void __attribute__((constructor)) register_bgp_module(void)
{
    dev_register_module(DEV_MODULE_ID_BGP, "bgp", bgp_module_init, bgp_module_cleanup);

    char bgp_xml_path[256];
    if (resolve_xml_path("bgp", bgp_xml_path, sizeof(bgp_xml_path)) == 0)
    {
        cfg_register_module_xml(DEV_MODULE_ID_BGP, bgp_xml_path);
    }
    else
    {
        fprintf(stderr, "[bgp] Warning: Could not resolve XML path for bgp module\n");
    }
}
