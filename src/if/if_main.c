/**
 * @file   if_main.c
 * @brief  接口模块主入口，三阶段初始化和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "if_main.h"

#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if_cli.h"
#include "if_map.h"
#include "log.h"
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

        db_condition_t conditions[] = {
            {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(logical_name)},
        };
        db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};
        gboolean exists = FALSE;
        int ret = db_rpc_exists(g_if_local->dev_ipc_ctx, "if_interface", &filter, &exists);
        db_value_free(&conditions[0].value);

        if (ret == ERRCODE_SUCCESS && !exists)
        {
            const char *field_names[] = {"name", "ip_address", "netmask", "shutdown"};
            db_value_t values[] = {db_value_text(logical_name), db_value_text(""), db_value_text(""), db_value_int(0)};
            db_rpc_insert(g_if_local->dev_ipc_ctx, "if_interface", field_names, values, 4);
            db_value_free(&values[0]);
            db_value_free(&values[1]);
            db_value_free(&values[2]);
            LOG_INFO("Inserted interface %s into database", logical_name);
        }
        else if (ret == ERRCODE_SUCCESS && exists)
        {
            LOG_DEBUG("Interface %s already exists in database, preserving config", logical_name);
        }
    }
}

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_IF,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START — 建立 IPC 连接到 CFG
// ============================================================================

static void if_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 1: MODULE_START — 建立 IPC 连接");

    dev_ipc_connect(ctx, DEV_MODULE_ID_CFG, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CFG);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);

    LOG_INFO("已连接到 CFG 和 DB");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留（直接回复 OK）
// ============================================================================

static void if_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 将接口写入数据库
// ============================================================================

/* if_interface 表结构定义 */
static const db_column_def_t IF_INTERFACE_COLS[] = {
    {"name", DB_TYPE_TEXT, DB_COL_PRIMARY_KEY, NULL},
    {"ip_address", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"netmask", DB_TYPE_TEXT, DB_COL_NOT_NULL, "''"},
    {"shutdown", DB_TYPE_INTEGER, DB_COL_NOT_NULL, "0"},
};

static const db_table_def_t IF_INTERFACE_TABLE = {
    .table_name = "if_interface",
    .cols = IF_INTERFACE_COLS,
    .num_cols = G_N_ELEMENTS(IF_INTERFACE_COLS),
};

static void if_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Phase 3: MODULE_READY — 初始化 IF 数据库");

    /* 建表（IF NOT EXISTS，幂等操作） */
    int ret = db_rpc_create_table_from_def(ctx, &IF_INTERFACE_TABLE);
    if (ret != ERRCODE_SUCCESS)
    {
        LOG_WARN("IF 建表失败，跳过数据库初始化");
        send_phase_response(ctx, msg, ERRCODE_SUCCESS);
        return;
    }

    /* 将接口映射表写入数据库 */
    if_init_db();

    LOG_INFO("IF module ready");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void if_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("Shutting down if module...");

    /* dev_ipc_ctx 由 DEV 管理 */
    g_if_local->dev_ipc_ctx = NULL;

    g_free(g_if_local);
    g_if_local = NULL;

    LOG_INFO("if module cleanup complete");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void if_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        /* ---- DEV 生命周期消息 ---- */
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            if_on_start(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            if_on_connect(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            if_on_ready(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            if_on_shutdown(ctx, msg);
            return;

        /* ---- CLI 消息 ---- */
        case CFG_MSG_TYPE_CLI:
            LOG_DEBUG("Received CLI command message");
            if_cli_handle_message(msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            if_cli_handle_continue(msg);
            break;

        default:
            LOG_WARN("Received unknown message type: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================

__attribute__((constructor)) static void if_so_init(void)
{
    LOG_INFO(".so 加载，自初始化");

    /* 创建 IPC 上下文 */
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_IF, "if", DEV_MODULE_PORT_IF, if_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("IPC 初始化失败");
        return;
    }

    /* 初始化本地状态（原 if_on_start 逻辑） */
    g_if_local = g_malloc0(sizeof(if_local_t));
    g_if_local->dev_ipc_ctx = ctx;

    /* 初始化接口映射
     * 查找顺序：
     *   1. 生产环境：NN_WORK_DIR/resources/if/if_map.conf.gns3
     *   2. 开发环境：可执行文件相对路径 ../src 和 ../../src */
    char *if_map_path = NULL;

    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir != NULL)
    {
        if_map_path = g_build_filename(work_dir, "resources", "if", "if_map.conf.gns3", NULL);
        LOG_INFO("Using GNS3 interface mapping: %s", if_map_path);
    }
    else
    {
        char exe_dir[PATH_MAX];
        if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
        {
            LOG_ERROR("Failed to get exe directory");
            return;
        }

        if_map_path = g_build_filename(exe_dir, "..", "..", "src", "if", "resources", "if_map.conf.local", NULL);
        LOG_INFO("Using local interface mapping: %s", if_map_path);
    }

    if (if_map_init(if_map_path) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Failed to load interface mapping");
    }
    g_free(if_map_path);
}
