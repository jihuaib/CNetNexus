/**
 * @file   db_main.c
 * @brief  数据库模块主入口，三阶段初始化和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "db_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cli.h"
#include "db_cli.h"
#include "db_ipc_handler.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

/* 全局上下文 */
db_local_t *g_db_local = NULL;

// ============================================================================
// 连接管理
// ============================================================================

void db_connection_free(db_connection_t *conn)
{
    if (!conn)
    {
        return;
    }

    if (conn->handle)
    {
        sqlite3_close(conn->handle);
    }

    g_mutex_clear(&conn->db_mutex);
    g_free(conn->db_path);
    g_free(conn);
}

db_connection_t *db_get_connection(const char *db_name)
{
    (void)db_name;
    if (!g_db_local)
    {
        return NULL;
    }

    return g_db_local->main_conn;
}

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_DB,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START - Establishing IPC connections到 CFG
// ============================================================================

static void db_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START - Establishing IPC connections");

    dev_ipc_connect(ctx, DEV_MODULE_ID_CLI, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CLI);

    /* 打开统一数据库文件 */
    if (db_initialize_database() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Unified database initialization failed");
    }

    LOG_INFO("Connected to CFG");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void db_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 预留（直接回复 OK）
// ============================================================================

static void db_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY (reserved)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void db_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Cleaning up database module local state");

    db_cli_cleanup_state();

    /* 释放统一数据库连接 */
    if (g_db_local->main_conn)
    {
        db_connection_free(g_db_local->main_conn);
        g_db_local->main_conn = NULL;
    }

    /* dev_ipc_ctx 由 DEV 管理 */
    g_db_local->dev_ipc_ctx = NULL;

    g_free(g_db_local);
    g_db_local = NULL;

    LOG_INFO("Database module cleaned up");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void db_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    /* DEV 生命周期消息 */
    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            db_on_start(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            db_on_connect(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            db_on_ready(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            db_on_shutdown(ctx, msg);
            return;
        default:
            break;
    }

    /* DB RPC 消息 */
    uint32_t category = DEV_IPC_MSG_CATEGORY(msg->msg_type);
    if (category == DEV_IPC_CATEGORY_DB)
    {
        db_ipc_msg_handler(ctx, msg);
        return;
    }

    /* CLI 消息 */
    switch (msg->msg_type)
    {
        case CLI_MSG_TYPE:
            LOG_DEBUG("Received CLI command message");
            db_cli_process_command(msg);
            break;

        case CLI_MSG_TYPE_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            db_cli_handle_continue(msg);
            break;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            db_cli_handle_show_config(msg);
            break;

        case CLI_MSG_TYPE_QUERY_CANDIDATES:
            LOG_DEBUG("Received query candidates request");
            db_cli_handle_query_candidates(ctx, msg);
            return; /* msg 由被调函数释放 */

        default:
            LOG_WARN("Received unknown message type: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================

int db_module_init(void)
{
    LOG_INFO("Module initialization");

    /* 创建 IPC 上下文 */
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_DB, "db", DEV_MODULE_PORT_DB, db_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    /* 初始化本地状态（原 db_on_start 逻辑） */
    g_db_local = g_malloc0(sizeof(db_local_t));
    g_db_local->main_conn = NULL;
    g_db_local->dev_ipc_ctx = ctx;
    return 0;
}
