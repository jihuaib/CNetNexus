/**
 * @file   bgp_main.c
 * @brief  BGP 模块主入口，三阶段初始化和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bgp_cli.h"
#include "bgp_db.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "module_ports.h"

bgp_local_t *g_bgp_local = NULL;

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(ipc_context_t *ctx, ipc_message_t *msg, int32_t result)
{
    ipc_message_t *resp = ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                             msg->request_id, NULL, 0, NULL);
    ipc_send_response(ctx, resp);
    ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START — 建立 IPC 连接到 CFG
// ============================================================================

static void bgp_on_start(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START — 建立 IPC 连接");

    ipc_connect(ctx, DEV_MODULE_ID_CFG, IPC_HOST_LOCAL, MODULE_PORT_CFG);
    ipc_connect(ctx, DEV_MODULE_ID_DB, IPC_HOST_LOCAL, MODULE_PORT_DB);

    LOG_INFO("已连接到 CFG 和 DB");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void bgp_on_connect(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 预留（直接回复 OK）
// ============================================================================

static void bgp_on_ready(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY — 初始化 BGP 数据库");

    /* 建立 BGP 数据库表（IF NOT EXISTS，幂等操作） */
    if (bgp_db_init(ctx) != 0)
    {
        LOG_WARN("BGP 数据库初始化失败，继续启动");
    }

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void bgp_on_shutdown(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("BGP module cleanup");

    /* ipc_ctx 由 DEV 管理 */
    g_bgp_local->ipc_ctx = NULL;

    g_free(g_bgp_local);
    g_bgp_local = NULL;

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void bgp_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case IPC_MSG_TYPE_DEV_MODULE_START:
            bgp_on_start(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            bgp_on_connect(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_READY:
            bgp_on_ready(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            bgp_on_shutdown(ctx, msg);
            return;

        /* ---- CLI 消息 ---- */
        case CFG_MSG_TYPE_CLI:
            LOG_DEBUG("Received CLI command message");
            bgp_cli_handle_message(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            bgp_cli_handle_continue(msg);
            break;

        default:
            break;
    }

    ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================
__attribute__((constructor)) static void bgp_so_init(void)
{
    LOG_INFO(".so 加载，自初始化");

    /* 创建 IPC 上下文 */
    ipc_context_t *ctx = ipc_init(DEV_MODULE_ID_BGP, "bgp", MODULE_PORT_BGP, bgp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC 初始化失败");
        return;
    }

    /* 初始化本地状态（原 bgp_on_start 逻辑） */
    g_bgp_local = g_malloc0(sizeof(bgp_local_t));
    g_bgp_local->ipc_ctx = ctx;
}