/**
 * @file   if_main.c
 * @brief  接口模块主入口，模块注册和 IPC 消息处理
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
#include "dev.h"
#include "errcode.h"
#include "if_cli.h"
#include "if_map.h"
#include "ipc.h"
#include "path_utils.h"

if_local_t *g_if_local = NULL;

/**
 * @brief 启动时将映射表中的接口写入数据库
 *
 * 遍历接口映射表，对于每个逻辑接口：
 * - 不存在则插入默认记录
 * - 已存在则保留现有配置（重启不丢失）
 */
static void if_init_db(void)
{
    extern if_map_t g_interface_map;

    for (int i = 0; i < g_interface_map.count; i++)
    {
        const char *logical_name = g_interface_map.entries[i].logical_name;

        // 检查数据库中是否已存在该接口
        char where[64];
        snprintf(where, sizeof(where), "name = '%s'", logical_name);
        gboolean exists = FALSE;
        int ret = db_exists("if_db", "if_interface", where, &exists);

        if (ret == ERRCODE_SUCCESS && !exists)
        {
            // 插入默认记录
            const char *field_names[] = {"name", "ip_address", "netmask", "shutdown"};
            db_value_t values[] = {db_value_text(logical_name), db_value_text(""), db_value_text(""), db_value_int(0)};
            db_insert("if_db", "if_interface", field_names, values, 4);
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
// IPC 消息处理回调
// ============================================================================

static void if_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
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

static int if_init_local()
{
    g_if_local = g_malloc0(sizeof(if_local_t));

    // 初始化 IPC
    g_if_local->ipc_ctx = ipc_init(DEV_MODULE_ID_IF, "if", NULL, if_msg_handler);
    if (!g_if_local->ipc_ctx)
    {
        fprintf(stderr, "[if] Failed to initialize IPC\n");
        return ERRCODE_FAIL;
    }

    /* CFG 会主动连接到 IF，IF 只需监听 */

    // 初始化接口映射
    // 优先级：RESOURCES_DIR（生产/GNS3） > 源码目录（开发）
    char if_map_path[PATH_MAX];
    const char *resources_dir = getenv("RESOURCES_DIR");
    if (resources_dir != NULL)
    {
        // 生产/GNS3 环境：使用 RESOURCES_DIR 下的 gns3 配置
        snprintf(if_map_path, sizeof(if_map_path), "%s/if/if_map.conf.gns3", resources_dir);
        printf("[if] Using GNS3 interface mapping: %s\n", if_map_path);
    }
    else
    {
        // 开发环境：使用源码目录下的 local 配置
        char exe_dir[PATH_MAX];
        if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
        {
            fprintf(stderr, "[if] Failed to get exe directory\n");
            return ERRCODE_FAIL;
        }
        snprintf(if_map_path, sizeof(if_map_path), "%s/../../src/if/resources/if_map.conf.local", exe_dir);
        printf("[if] Using local interface mapping: %s\n", if_map_path);
    }
    if (if_map_init(if_map_path) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    // 将接口信息写入数据库
    if_init_db();

    return ERRCODE_SUCCESS;
}

static void if_cleanup_local()
{
    if (g_if_local == NULL)
    {
        return;
    }

    printf("[if] Shutting down if module...\n");

    if (g_if_local->ipc_ctx)
    {
        ipc_destroy(g_if_local->ipc_ctx);
    }

    g_free(g_if_local);
    g_if_local = NULL;

    printf("[if] if module cleanup complete\n");
}

static int32_t if_module_init()
{
    int ret = if_init_local();
    if (ret != ERRCODE_SUCCESS)
    {
        if_cleanup_local();
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

static void if_module_cleanup(void)
{
    if_cleanup_local();
}

static void __attribute__((constructor)) register_if_module(void)
{
    dev_register_module(DEV_MODULE_ID_IF, "if", if_module_init, if_module_cleanup);

    char if_xml_path[256];
    if (resolve_xml_path("if", if_xml_path, sizeof(if_xml_path)) == 0)
    {
        cfg_register_module_xml(DEV_MODULE_ID_IF, if_xml_path);
    }
}
