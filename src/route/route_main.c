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
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "route_cli.h"

route_local_t *g_route_local = NULL;

/* route_ipv4 表结构定义 */
static const db_column_def_t ROUTE_IPV4_COLS[] = {
    {"destination", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"mask", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"next_hop", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"metric", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

static const db_table_def_t ROUTE_IPV4_TABLE = {
    .table_name = "route_ipv4",
    .cols = ROUTE_IPV4_COLS,
    .num_cols = G_N_ELEMENTS(ROUTE_IPV4_COLS),
};

/* route_ipv6 表结构定义 */
static const db_column_def_t ROUTE_IPV6_COLS[] = {
    {"destination", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"prefix_length", DB_TYPE_INTEGER, DB_COL_NOT_NULL, NULL},
    {"next_hop", DB_TYPE_TEXT, DB_COL_NOT_NULL, NULL},
    {"metric", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

static const db_table_def_t ROUTE_IPV6_TABLE = {
    .table_name = "route_ipv6",
    .cols = ROUTE_IPV6_COLS,
    .num_cols = G_N_ELEMENTS(ROUTE_IPV6_COLS),
};

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_ROUTE,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START — 建立 IPC 连接到 DB, CFG
// ============================================================================

static void route_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START — 建立 IPC 连接");

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB) < 0)
    {
        LOG_ERROR("连接 DB 模块失败");
    }

    if (dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI) != 0)
    {
        LOG_ERROR("连接 CFG 模块失败");
    }

    LOG_INFO("已连接到 DB, CFG");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void route_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 初始化 Route 数据库
// ============================================================================

static void route_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY — 初始化 Route 数据库");

    int ret = db_rpc_create_table_from_def(ctx, &ROUTE_IPV4_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route 建表失败: route_ipv4");
        send_phase_response(ctx, msg, ERRCODE_SUCCESS);
        return;
    }

    ret = db_rpc_create_table_from_def(ctx, &ROUTE_IPV6_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("Route 建表失败: route_ipv6");
        send_phase_response(ctx, msg, ERRCODE_SUCCESS);
        return;
    }

    LOG_INFO("Route 数据库表已就绪");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void route_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("正在关闭 Route 模块...");

    if (g_route_local)
    {
        g_route_local->running = 0;
        /* dev_ipc_ctx 由 DEV 管理 */
        g_route_local->dev_ipc_ctx = NULL;

        free(g_route_local);
        g_route_local = NULL;
    }

    LOG_INFO("Route 模块清理完成");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void route_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (msg == NULL)
    {
        return;
    }

    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            route_on_start(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            route_on_connect(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            route_on_ready(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            route_on_shutdown(ctx, msg);
            return;

        /* ---- CLI 消息 ---- */
        case CLI_MSG_TYPE:
            LOG_DEBUG("收到 CLI 命令消息 (%u bytes)", msg->payload_len);
            route_cli_handle_message(msg);
            break;

        case CLI_MSG_TYPE_CONTINUE:
            LOG_DEBUG("收到 CLI 继续请求");
            route_cli_handle_continue(msg);
            break;

        default:
            LOG_WARN("收到未知消息类型: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// 模块初始化
// ============================================================================

int route_module_init(void)
{
    LOG_INFO("模块初始化");

    /* 创建 IPC 上下文 */
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_ROUTE, "route", DEV_MODULE_PORT_ROUTE, route_ipc_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC 初始化失败");
        return -1;
    }

    /* 初始化本地状态（原 route_on_start 逻辑） */
    g_route_local = g_malloc0(sizeof(route_local_t));
    if (!g_route_local)
    {
        LOG_ERROR("分配 route 上下文失败");
        return -1;
    }
    g_route_local->dev_ipc_ctx = ctx;
    g_route_local->running = 1;
    return 0;
}
