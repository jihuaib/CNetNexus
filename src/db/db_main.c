/**
 * @file   db_main.c
 * @brief  数据库模块主入口，IPC 初始化和数据库连接管理
 * @author jhb
 * @date   2026/01/22
 */
#include "db_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db_ipc_handler.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

// 全局上下文
db_local_t *g_db_local = NULL;

// ============================================================================
// 连接管理
// ============================================================================

static void free_connection(db_connection_t *conn)
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
// 模块生命周期
// ============================================================================

int32_t db_module_init()
{
    g_db_local = g_malloc0(sizeof(db_local_t));
    g_db_local->connections =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_connection);

    // 初始化 schema 注册表
    g_db_local->registry = db_registry_get_instance();

    // 初始化 IPC 上下文，接受来自远程模块的 DB RPC 请求和 CLI 命令
    g_db_local->ipc_ctx =
        ipc_init(DEV_MODULE_ID_DB, "db", NULL, db_ipc_msg_handler);
    if (g_db_local->ipc_ctx)
    {
        printf("[db] IPC 服务初始化完成\n");
    }
    else
    {
        fprintf(stderr, "[db] IPC 初始化失败\n");
        g_hash_table_destroy(g_db_local->connections);
        g_free(g_db_local);
        g_db_local = NULL;
        return ERRCODE_FAIL;
    }

    printf("[db] 数据库模块初始化完成\n");
    return ERRCODE_SUCCESS;
}

void db_module_cleanup(void)
{
    if (!g_db_local)
    {
        return;
    }

    printf("[db] 正在关闭数据库模块...\n");

    // 销毁 IPC 上下文
    if (g_db_local->ipc_ctx)
    {
        ipc_destroy(g_db_local->ipc_ctx);
        g_db_local->ipc_ctx = NULL;
    }

    // 关闭所有数据库连接
    if (g_db_local->connections)
    {
        g_hash_table_destroy(g_db_local->connections);
    }

    // 销毁 schema 注册表
    db_registry_destroy();

    g_free(g_db_local);
    g_db_local = NULL;

    printf("[db] 数据库模块清理完成\n");
}
