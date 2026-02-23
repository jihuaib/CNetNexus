/**
 * @file   db_main.c
 * @brief  数据库模块主入口，三阶段初始化和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#define LOG_TAG "db"
#include "db_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cli.h"
#include "db_cli.h"
#include "db_ipc_handler.h"
#include "db_registry.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "module_ports.h"

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
    if (!db_name || !g_db_local)
    {
        return NULL;
    }

    return g_hash_table_lookup(g_db_local->connections, db_name);
}

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(ipc_context_t *ctx, ipc_message_t *msg, int32_t result)
{
    ipc_message_t *resp = ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_DB, msg->src_module_id,
                                             msg->request_id, NULL, 0, NULL);
    ipc_send_response(ctx, resp);
    ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START — 建立 IPC 连接到 CFG
// ============================================================================

static void db_on_start(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START — 建立 IPC 连接");

    /* 提取 DEV 下发的模块名称表 */
    if (msg->payload && msg->payload_len >= sizeof(ipc_module_table_t))
    {
        const ipc_module_table_t *table = (const ipc_module_table_t *)msg->payload;
        ipc_set_module_table(ctx, table);

        for (uint32_t i = 0; i < table->count; i++)
        {
            if (table->entries[i].module_id == table->self_module_id)
            {
                strlcpy(g_db_local->name, table->entries[i].name, DEV_MODULE_NAME_MAX_LEN);
                LOG_INFO("本模块名称: %s", g_db_local->name);
                break;
            }
        }
    }

    ipc_connect(ctx, DEV_MODULE_ID_CFG, IPC_HOST_LOCAL, MODULE_PORT_CFG);

    LOG_INFO("已连接到 CFG");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void db_on_connect(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 预留（直接回复 OK）
// ============================================================================

static void db_on_ready(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void db_on_shutdown(ipc_context_t *ctx, ipc_message_t *msg)
{
    LOG_INFO("Cleaning up database module local state");

    /* 关闭所有数据库连接 */
    if (g_db_local->connections)
    {
        g_hash_table_destroy(g_db_local->connections);
        g_db_local->connections = NULL;
    }

    /* 销毁 registry */
    db_registry_destroy();

    /* ipc_ctx 由 DEV 管理 */
    g_db_local->ipc_ctx = NULL;

    g_free(g_db_local);
    g_db_local = NULL;

    LOG_INFO("Database module cleaned up");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void db_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    /* DEV 生命周期消息 */
    switch (msg->msg_type)
    {
        case IPC_MSG_TYPE_DEV_MODULE_START:
            db_on_start(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            db_on_connect(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_READY:
            db_on_ready(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            db_on_shutdown(ctx, msg);
            return;
        default:
            break;
    }

    /* DB RPC 消息 */
    uint32_t category = IPC_MSG_CATEGORY(msg->msg_type);
    if (category == IPC_CATEGORY_DB)
    {
        db_ipc_msg_handler(ctx, msg);
        return;
    }

    /* CLI 消息 */
    switch (msg->msg_type)
    {
        case CFG_MSG_TYPE_CLI:
            LOG_DEBUG("Received CLI command message");
            db_cli_process_command(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            db_cli_handle_continue(msg);
            break;

        default:
            LOG_WARN("Received unknown message type: 0x%08X", msg->msg_type);
            break;
    }

    ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================

#include "dev.h"

__attribute__((constructor)) static void db_so_init(void)
{
    LOG_INFO(".so 加载，自初始化");

    /* 创建 IPC 上下文 */
    ipc_context_t *ctx = ipc_init(DEV_MODULE_ID_DB, "db", MODULE_PORT_DB, db_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC 初始化失败");
        return;
    }

    /* 初始化本地状态（原 db_on_start 逻辑） */
    g_db_local = g_malloc0(sizeof(db_local_t));
    g_db_local->connections =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)db_connection_free);
    g_db_local->registry = db_registry_get_instance();
    g_db_local->ipc_ctx = ctx;
}