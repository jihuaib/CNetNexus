/**
 * @file   if_main.c
 * @brief  接口模块主入口，三阶段初始化和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "if_main.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "db_rpc.h"
#include "dev.h"
#include "errcode.h"
#include "if_cli.h"
#include "if_map.h"
#include "ipc.h"
#include "path_utils.h"

if_local_t *g_if_local = NULL;

/**
 * @brief 启动时将映射表中的接口写入数据库
 */
static void if_init_db(void)
{
    extern if_map_t g_interface_map;

    for (int i = 0; i < g_interface_map.count; i++)
    {
        const char *logical_name = g_interface_map.entries[i].logical_name;

        char where[64];
        snprintf(where, sizeof(where), "name = '%s'", logical_name);
        gboolean exists = FALSE;
        int ret = db_rpc_exists(g_if_local->ipc_ctx, "if_db", "if_interface", where, &exists);

        if (ret == ERRCODE_SUCCESS && !exists)
        {
            const char *field_names[] = {"name", "ip_address", "netmask", "shutdown"};
            db_value_t values[] = {db_value_text(logical_name), db_value_text(""), db_value_text(""), db_value_int(0)};
            db_rpc_insert(g_if_local->ipc_ctx, "if_db", "if_interface", field_names, values, 4);
            db_value_free(&values[0]);
            db_value_free(&values[1]);
            db_value_free(&values[2]);
            printf("[if] Inserted interface %s into database\n", logical_name);
        }
        else if (ret == ERRCODE_SUCCESS && exists)
        {
            printf("[if] Interface %s already exists in database, preserving config\n", logical_name);
        }
    }
}

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(ipc_context_t *ctx, ipc_message_t *msg, int32_t result)
{
    ipc_message_t *resp = ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_IF, msg->src_module_id,
                                             msg->request_id, NULL, 0, NULL);
    ipc_send_response(ctx, resp);
    ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START - 创建上下文，加载 if_map
// ============================================================================

static void if_on_start(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[if] Phase 1: MODULE_START\n");

    g_if_local = g_malloc0(sizeof(if_local_t));
    g_if_local->ipc_ctx = ctx;

    /* 初始化接口映射 */
    char if_map_path[PATH_MAX];
    const char *resources_dir = getenv("RESOURCES_DIR");
    if (resources_dir != NULL)
    {
        snprintf(if_map_path, sizeof(if_map_path), "%s/if/if_map.conf.gns3", resources_dir);
        printf("[if] Using GNS3 interface mapping: %s\n", if_map_path);
    }
    else
    {
        char exe_dir[PATH_MAX];
        if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
        {
            fprintf(stderr, "[if] Failed to get exe directory\n");
            send_phase_response(ctx, msg, ERRCODE_FAIL);
            return;
        }
        snprintf(if_map_path, sizeof(if_map_path), "%s/../../src/if/resources/if_map.conf.local", exe_dir);
        printf("[if] Using local interface mapping: %s\n", if_map_path);
    }

    if (if_map_init(if_map_path) != ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[if] Failed to load interface mapping\n");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    printf("[if] Module started\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT - 连接到 CFG
// ============================================================================

static void if_on_connect(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[if] Phase 2: MODULE_CONNECT\n");

    ipc_connect(ctx, DEV_MODULE_ID_CFG);

    printf("[if] 已连接到 CFG\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY - 将接口写入数据库
// ============================================================================

static void if_on_ready(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[if] Phase 3: MODULE_READY\n");

    if_init_db();

    printf("[if] IF module ready\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void if_on_shutdown(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[if] Shutting down if module...\n");

    /* ipc_ctx 由 DEV 管理 */
    g_if_local->ipc_ctx = NULL;

    g_free(g_if_local);
    g_if_local = NULL;

    printf("[if] if module cleanup complete\n");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void if_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case IPC_MSG_TYPE_DEV_MODULE_START:
            if_on_start(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            if_on_connect(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_READY:
            if_on_ready(ctx, msg);
            return;
        case IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            if_on_shutdown(ctx, msg);
            return;

        /* ---- CLI 消息 ---- */
        case CFG_MSG_TYPE_CLI:
            printf("[if] Received CLI command message\n");
            if_cli_handle_message(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            printf("[if] Received CLI continue request\n");
            if_cli_handle_continue(msg);
            break;

        default:
            printf("[if] Received unknown message type: 0x%08X\n", msg->msg_type);
            break;
    }

    ipc_message_free(msg);
}

// ============================================================================
// 入口函数：创建 IPC 上下文
// ============================================================================

