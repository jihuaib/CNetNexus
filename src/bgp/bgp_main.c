/**
 * @file   bgp_main.c
 * @brief  BGP 模块主入口，IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_main.h"

#include <stdio.h>
#include <string.h>

#include "bgp_cli.h"
#include "cfg.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

bgp_local_t *g_bgp_local = NULL;

/**
 * @brief BGP IPC 消息处理回调
 *
 * 由 IPC IO 线程调用，处理来自其他模块（主要是 CFG）的消息
 */
static void bgp_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    (void)ctx;

    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case CFG_MSG_TYPE_CLI:
            printf("[bgp] Received CLI command message (%zu bytes)\n", msg->payload_len);
            bgp_cli_handle_message(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            printf("[bgp] Received CLI continue request\n");
            bgp_cli_handle_continue(msg);
            break;

        default:
            printf("[bgp] Received unknown message type: 0x%08X\n", msg->msg_type);
            break;
    }

    ipc_message_free(msg);
}

int bgp_init(void)
{
    g_bgp_local = g_malloc0(sizeof(bgp_local_t));

    /* 初始化 IPC 上下文 */
    g_bgp_local->ipc_ctx =
        ipc_init(DEV_MODULE_ID_BGP, "bgp", NULL, bgp_ipc_msg_handler);
    if (!g_bgp_local->ipc_ctx)
    {
        fprintf(stderr, "[bgp] IPC 初始化失败\n");
        g_free(g_bgp_local);
        g_bgp_local = NULL;
        return ERRCODE_FAIL;
    }

    /* 连接到 DB 进程 */
    if (ipc_connect(g_bgp_local->ipc_ctx, DEV_MODULE_ID_DB) < 0)
    {
        fprintf(stderr, "[bgp] 连接 DB 模块失败\n");
    }

    /* 连接到 CFG 进程 */
    if (ipc_connect(g_bgp_local->ipc_ctx, DEV_MODULE_ID_CFG) < 0)
    {
        fprintf(stderr, "[bgp] 连接 CFG 模块失败\n");
    }

    /* 设置 DB 客户端代理使用的 IPC 上下文 */
    db_client_set_ipc(g_bgp_local->ipc_ctx);

    g_bgp_local->running = 1;

    printf("[bgp] BGP 模块初始化完成 (IPC)\n");
    return ERRCODE_SUCCESS;
}

void bgp_cleanup(void)
{
    if (!g_bgp_local)
    {
        return;
    }

    printf("[bgp] 正在关闭 BGP 模块...\n");

    g_bgp_local->running = 0;

    if (g_bgp_local->ipc_ctx)
    {
        ipc_destroy(g_bgp_local->ipc_ctx);
        g_bgp_local->ipc_ctx = NULL;
    }

    g_free(g_bgp_local);
    g_bgp_local = NULL;

    printf("[bgp] BGP 模块清理完成\n");
}
