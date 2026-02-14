/**
 * @file   route_main.c
 * @brief  Route 模块主入口，三阶段初始化和 IPC 消息分发
 * @author jhb
 * @date   2026/02/01
 */
#include "route_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"
#include "route_cli.h"

route_local_t *g_route_local = NULL;

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(ipc_context_t *ctx, ipc_message_t *msg, int32_t result)
{
    ipc_message_t *resp = ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                             msg->request_id, NULL, 0, NULL);
    ipc_send_response(ctx, resp);
    ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START - 创建上下文
// ============================================================================

static void route_on_start(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[route] Phase 1: MODULE_START\n");

    g_route_local = calloc(1, sizeof(route_local_t));
    if (g_route_local == NULL)
    {
        fprintf(stderr, "[route] 分配 route 上下文失败\n");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    g_route_local->ipc_ctx = ctx;
    g_route_local->running = 1;

    printf("[route] Module started\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT - 连接到 DB, CFG
// ============================================================================

static void route_on_connect(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[route] Phase 2: MODULE_CONNECT\n");

    if (ipc_connect(ctx, DEV_MODULE_ID_DB) < 0)
    {
        fprintf(stderr, "[route] 连接 DB 模块失败\n");
    }

    if (ipc_connect(ctx, DEV_MODULE_ID_CFG) != 0)
    {
        fprintf(stderr, "[route] 连接 CFG 模块失败\n");
    }

    printf("[route] 已连接到 DB, CFG\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY
// ============================================================================

static void route_on_ready(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[route] Phase 3: MODULE_READY\n");
    printf("[route] Route 模块初始化完成\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void route_on_shutdown(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[route] 正在关闭 Route 模块...\n");

    if (g_route_local)
    {
        g_route_local->running = 0;
        /* ipc_ctx 由 DEV 管理 */
        g_route_local->ipc_ctx = NULL;

        free(g_route_local);
        g_route_local = NULL;
    }

    printf("[route] Route 模块清理完成\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void route_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    if (msg == NULL)
    {
        return;
    }

    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case IPC_MSG_TYPE_DEV_MODULE_START:
            route_on_start(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            route_on_connect(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_READY:
            route_on_ready(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            route_on_shutdown(ctx, msg);
            return;

        /* ---- CLI 消息 ---- */
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

// ============================================================================
// 入口函数：创建 IPC 上下文
// ============================================================================

