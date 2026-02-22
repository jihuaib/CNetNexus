/**
 * @file   route_main.c
 * @brief  Route 模块主入口，三阶段初始化和 IPC 消息分发
 * @author jhb
 * @date   2026/02/01
 */
#define LOG_TAG "route"
#include "route_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"
#include "log.h"
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
// Phase 1: MODULE_START — 建立 IPC 连接到 DB, CFG
// ============================================================================

static void route_on_start(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START — 建立 IPC 连接");

    if (ipc_connect(ctx, DEV_MODULE_ID_DB) < 0)
    {
        LOG_ERROR("连接 DB 模块失败");
    }

    if (ipc_connect(ctx, DEV_MODULE_ID_CFG) != 0)
    {
        LOG_ERROR("连接 CFG 模块失败");
    }

    LOG_INFO("已连接到 DB, CFG");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void route_on_connect(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 预留（直接回复 OK）
// ============================================================================

static void route_on_ready(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void route_on_shutdown(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("正在关闭 Route 模块...");

    if (g_route_local)
    {
        g_route_local->running = 0;
        /* ipc_ctx 由 DEV 管理 */
        g_route_local->ipc_ctx = NULL;

        free(g_route_local);
        g_route_local = NULL;
    }

    LOG_INFO("Route 模块清理完成");
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
            LOG_DEBUG("收到 CLI 命令消息 (%u bytes)", msg->payload_len);
            route_cli_handle_message(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            LOG_DEBUG("收到 CLI 继续请求");
            route_cli_handle_continue(msg);
            break;

        default:
            LOG_WARN("收到未知消息类型: 0x%08X", msg->msg_type);
            break;
    }

    ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================

__attribute__((constructor)) static void route_so_init(void)
{
    LOG_INFO(".so 加载，自初始化");

    /* 创建 IPC 上下文 */
    ipc_context_t *ctx = ipc_init(DEV_MODULE_ID_ROUTE, "route", NULL, route_ipc_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC 初始化失败");
        return;
    }

    /* 初始化本地状态（原 route_on_start 逻辑） */
    g_route_local = calloc(1, sizeof(route_local_t));
    if (!g_route_local)
    {
        LOG_ERROR("分配 route 上下文失败");
        return;
    }
    g_route_local->ipc_ctx = ctx;
    g_route_local->running = 1;
}