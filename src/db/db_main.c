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
// IPC 消息处理回调
// ============================================================================

void db_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    /* DB RPC 消息 */
    uint32_t category = DEV_IPC_MSG_CATEGORY(msg->msg_type);
    if (category == DEV_IPC_CATEGORY_DB)
    {
        db_ipc_msg_handler(msg);
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
            db_cli_handle_query_candidates(msg);
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
    log_set_tag("db");
    LOG_INFO("Module initialization");

    /* 创建 IPC 上下文 */
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_DB, "db", DEV_MODULE_PORT_DB, db_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC initialization failed");
        return -1;
    }

    g_db_local = g_malloc0(sizeof(db_local_t));
    g_db_local->main_conn = NULL;
    g_db_local->dev_ipc_ctx = ctx;

    /* 弱依赖模型 init：
     *   1. 打开 sqlite 文件（本地操作，不依赖其它模块）
     *   2. 等 DEV 控制连接
     *   3. 订阅 CLI 让 CFG 能 dispatch 命令到本模块
     *   4. notify_ready 通知 DEV 模块就绪 */
    if (db_initialize_database() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("DB: unified database initialization failed");
    }

    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("DB: timed out waiting for DEV connection; module may be unusable");
    }

    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("DB: notify_ready to DEV failed");
    }

    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("DB: subscribe(CLI) failed");
    }
    LOG_INFO("DB: module ready");

    return 0;
}

void db_module_cleanup(void)
{
    if (!g_db_local)
    {
        return;
    }

    /* 向 DEV 发 PRE_EXIT 通知，等 DEV 同步完成 phase/broadcast/drop 后再 ACK。
     * 必须在 dev_ipc_destroy 之前；超时/失败不阻塞退出，SIGCHLD 路径仍会兜底清理。 */
    dev_ipc_context_t *ctx = g_db_local->dev_ipc_ctx;
    if (ctx)
    {
        dev_ipc_pre_exit_notify(ctx, 3000);
    }

    g_db_local->dev_ipc_ctx = NULL;
    if (ctx)
    {
        dev_ipc_destroy(ctx);
    }

    if (!g_db_local)
    {
        return;
    }

    db_cli_cleanup_state();

    if (g_db_local->main_conn)
    {
        db_connection_free(g_db_local->main_conn);
        g_db_local->main_conn = NULL;
    }

    g_free(g_db_local);
    g_db_local = NULL;
}
