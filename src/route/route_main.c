/**
 * @file   route_main.c
 * @brief  Route 模块主入口，IPC 初始化与消息分发
 * @author jhb
 * @date   2026/02/01
 */
#include "route_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "route_cli.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

route_local_t *g_route_local = NULL;

/**
 * @brief IPC 消息处理回调
 * @param ctx IPC 上下文
 * @param msg 接收到的消息
 */
static void route_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    (void)ctx;

    if (msg == NULL)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case CFG_MSG_TYPE_CLI:
            printf("[route] 收到 CLI 命令消息 (%u bytes)\n", msg->payload_len);
            route_cli_handle_message(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            printf("[route] 收到 CLI 继续请求\n");
            route_cli_handle_continue(msg);
            break;

        default:
            printf("[route] 收到未知消息类型: 0x%08X\n", msg->msg_type);
            break;
    }

    ipc_message_free(msg);
}

int route_init(void)
{
    g_route_local = calloc(1, sizeof(route_local_t));
    if (g_route_local == NULL)
    {
        fprintf(stderr, "[route] 分配 route 上下文失败\n");
        return ERRCODE_FAIL;
    }

    /* 初始化 IPC */
    g_route_local->ipc_ctx =
        ipc_init(DEV_MODULE_ID_ROUTE, "route", NULL, route_ipc_msg_handler);
    if (g_route_local->ipc_ctx == NULL)
    {
        fprintf(stderr, "[route] IPC 初始化失败\n");
        free(g_route_local);
        g_route_local = NULL;
        return ERRCODE_FAIL;
    }

    /* 连接到 DB 进程 */
    if (ipc_connect(g_route_local->ipc_ctx, DEV_MODULE_ID_DB) < 0)
    {
        fprintf(stderr, "[route] 连接 DB 模块失败\n");
    }

    /* 连接到 CFG 模块 */
    if (ipc_connect(g_route_local->ipc_ctx, DEV_MODULE_ID_CFG) != 0)
    {
        fprintf(stderr, "[route] 连接 CFG 模块失败\n");
        ipc_destroy(g_route_local->ipc_ctx);
        free(g_route_local);
        g_route_local = NULL;
        return ERRCODE_FAIL;
    }

    /* 设置 DB 客户端代理使用的 IPC 上下文 */
    db_client_set_ipc(g_route_local->ipc_ctx);

    g_route_local->running = 1;

    printf("[route] Route 模块初始化完成\n");
    return ERRCODE_SUCCESS;
}

void route_cleanup(void)
{
    if (g_route_local == NULL)
    {
        return;
    }

    printf("[route] 正在关闭 Route 模块...\n");

    g_route_local->running = 0;

    if (g_route_local->ipc_ctx != NULL)
    {
        ipc_destroy(g_route_local->ipc_ctx);
        g_route_local->ipc_ctx = NULL;
    }

    free(g_route_local);
    g_route_local = NULL;

    printf("[route] Route 模块清理完成\n");
}
