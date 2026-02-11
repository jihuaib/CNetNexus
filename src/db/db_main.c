/**
 * @file   db_main.c
 * @brief  数据库模块主入口，模块注册和 IPC 消息处理
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
#include "db_registry.h"
#include "dev.h"
#include "errcode.h"
#include "path_utils.h"

// Global context instance
db_local_t *g_db_local = NULL;

// ============================================================================
// Connection Management
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
// IPC 消息处理回调
// ============================================================================

static void db_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    uint32_t category = IPC_MSG_CATEGORY(msg->msg_type);

    if (category == IPC_CATEGORY_DB)
    {
        db_ipc_msg_handler(ctx, msg);
        return;
    }

    switch (msg->msg_type)
    {
        case CFG_MSG_TYPE_CLI:
            printf("[db] Received CLI command message\n");
            db_cli_process_command(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            printf("[db] Received CLI continue request\n");
            db_cli_handle_continue(msg);
            break;

        default:
            fprintf(stderr, "[db] Received unknown message type: 0x%08X\n", msg->msg_type);
            break;
    }

    ipc_message_free(msg);
}

static int db_init_local()
{
    // Create context
    g_db_local = g_malloc0(sizeof(db_local_t));
    g_db_local->connections =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)db_connection_free);

    // Initialize registry
    g_db_local->registry = db_registry_get_instance();

    // 初始化 IPC
    g_db_local->ipc_ctx = ipc_init(DEV_MODULE_ID_DB, "db", NULL, db_msg_handler);
    if (!g_db_local->ipc_ctx)
    {
        fprintf(stderr, "[db] Failed to initialize IPC\n");
        return ERRCODE_FAIL;
    }

    // 主动连接到 CFG
    ipc_connect(g_db_local->ipc_ctx, DEV_MODULE_ID_CFG);

    return ERRCODE_SUCCESS;
}

static void db_cleanup_local()
{
    if (!g_db_local)
    {
        return;
    }

    printf("[db] Cleaning up database module local state\n");

    // 销毁 IPC
    if (g_db_local->ipc_ctx)
    {
        ipc_destroy(g_db_local->ipc_ctx);
    }

    // Close all database connections
    if (g_db_local->connections)
    {
        g_hash_table_destroy(g_db_local->connections);
    }

    // Destroy registry
    db_registry_destroy();

    g_free(g_db_local);
    g_db_local = NULL;
}

// ============================================================================
// Module Lifecycle Functions
// ============================================================================

int32_t db_module_init()
{
    int ret = db_init_local();
    if (ret != ERRCODE_SUCCESS)
    {
        db_cleanup_local();
        return ERRCODE_FAIL;
    }

    printf("[db] Database module initialized (IPC)\n");
    return ERRCODE_SUCCESS;
}

void db_module_cleanup(void)
{
    db_cleanup_local();
    printf("[db] Database module cleaned up\n");
}

// ============================================================================
// Module Registration (Constructor)
// ============================================================================

static void __attribute__((constructor)) register_db_module(void)
{
    // Register module with init/cleanup callbacks
    dev_register_module(DEV_MODULE_ID_DB, "db", db_module_init, db_module_cleanup);

    char cfg_xml_path[256];
    if (resolve_xml_path("db", cfg_xml_path, sizeof(cfg_xml_path)) == 0)
    {
        cfg_register_module_xml(DEV_MODULE_ID_DB, cfg_xml_path);
    }
    else
    {
        fprintf(stderr, "[db] Warning: Could not resolve XML path for cfg module\n");
    }
}
