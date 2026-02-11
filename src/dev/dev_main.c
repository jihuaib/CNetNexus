/**
 * @file   dev_main.c
 * @brief  Dev 模块主入口，模块注册和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "dev_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "dev_cli.h"
#include "dev_module.h"
#include "errcode.h"
#include "path_utils.h"

dev_local_t *g_dev_local = NULL;

// ============================================================================
// IPC 消息处理回调
// ============================================================================

static void dev_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        case CFG_MSG_TYPE_CLI:
            printf("[dev] Received CLI command message\n");
            dev_cli_handle_message(ctx, msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            printf("[dev] Received CLI continue request\n");
            dev_cli_handle_continue(ctx, msg);
            break;

        default:
            break;
    }

    ipc_message_free(msg);
}

static int dev_init_local()
{
    g_dev_local = g_malloc0(sizeof(dev_local_t));

    g_dev_local->ipc_ctx = ipc_init(DEV_MODULE_ID_DEV, "dev", NULL, dev_msg_handler);
    if (!g_dev_local->ipc_ctx)
    {
        fprintf(stderr, "[dev] Failed to initialize IPC\n");
        return ERRCODE_FAIL;
    }

    // 主动连接到 CFG
    ipc_connect(g_dev_local->ipc_ctx, DEV_MODULE_ID_CFG);

    return ERRCODE_SUCCESS;
}

static void dev_cleanup_local()
{
    if (g_dev_local == NULL)
    {
        return;
    }

    printf("[dev] Dev module cleanup\n");

    if (g_dev_local->ipc_ctx)
    {
        ipc_destroy(g_dev_local->ipc_ctx);
    }

    g_free(g_dev_local);
    g_dev_local = NULL;
}

// Module initialization
static int32_t dev_module_init()
{
    int ret = dev_init_local();
    if (ret != ERRCODE_SUCCESS)
    {
        dev_cleanup_local();
        return ERRCODE_FAIL;
    }

    printf("[dev] DEV module initialized (IPC)\n");
    return ERRCODE_SUCCESS;
}

// Module cleanup
static void dev_module_cleanup(void)
{
    dev_cleanup_local();
}

// Register dev module using constructor attribute
static void __attribute__((constructor)) register_dev_module(void)
{
    dev_register_module(DEV_MODULE_ID_DEV, "dev", dev_module_init, dev_module_cleanup);

    char dev_xml_path[256];
    if (resolve_xml_path("dev", dev_xml_path, sizeof(dev_xml_path)) == 0)
    {
        cfg_register_module_xml(DEV_MODULE_ID_DEV, dev_xml_path);
    }
    else
    {
        fprintf(stderr, "[dev] Warning: Could not resolve XML path for dev module\n");
    }
}
