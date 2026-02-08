/**
 * @file   if_main.c
 * @brief  接口模块主入口，IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#include "if_main.h"

#include <stdio.h>
#include <string.h>

#include "cfg.h"
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
            db_value_t values[] = {db_value_text(logical_name), db_value_text(""), db_value_text(""),
                                      db_value_int(0)};
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

/**
 * @brief IF IPC 消息处理回调
 *
 * 由 IPC IO 线程调用，处理来自其他模块（主要是 CFG）的消息
 */
static void if_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    (void)ctx;

    if (!msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case CFG_MSG_TYPE_CLI:
            printf("[if] Received CLI command message (%zu bytes)\n", msg->payload_len);
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

int if_init(void)
{
    g_if_local = g_malloc0(sizeof(if_local_t));

    /* 初始化 IPC 上下文 */
    g_if_local->ipc_ctx =
        ipc_init(DEV_MODULE_ID_IF, "if", NULL, if_ipc_msg_handler);
    if (!g_if_local->ipc_ctx)
    {
        fprintf(stderr, "[if] IPC 初始化失败\n");
        g_free(g_if_local);
        g_if_local = NULL;
        return ERRCODE_FAIL;
    }

    /* 连接到 DB 进程 */
    if (ipc_connect(g_if_local->ipc_ctx, DEV_MODULE_ID_DB) < 0)
    {
        fprintf(stderr, "[if] 连接 DB 模块失败\n");
    }

    /* 连接到 CFG 进程 */
    if (ipc_connect(g_if_local->ipc_ctx, DEV_MODULE_ID_CFG) < 0)
    {
        fprintf(stderr, "[if] 连接 CFG 模块失败\n");
    }

    /* 设置 DB 客户端代理使用的 IPC 上下文 */
    db_client_set_ipc(g_if_local->ipc_ctx);

    /* 初始化接口映射 */
    /* 优先级：RESOURCES_DIR（生产/GNS3） > 源码目录（开发） */
    char if_map_path[PATH_MAX];
    const char *resources_dir = getenv("RESOURCES_DIR");
    if (resources_dir != NULL)
    {
        /* 生产/GNS3 环境：使用 RESOURCES_DIR 下的 gns3 配置 */
        snprintf(if_map_path, sizeof(if_map_path), "%s/if/if_map.conf.gns3", resources_dir);
        printf("[if] Using GNS3 interface mapping: %s\n", if_map_path);
    }
    else
    {
        /* 开发环境：使用源码目录下的 local 配置 */
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

    /* 将接口信息写入数据库 */
    if_init_db();

    g_if_local->running = 1;

    printf("[if] IF 模块初始化完成 (IPC)\n");
    return ERRCODE_SUCCESS;
}

void if_cleanup(void)
{
    if (!g_if_local)
    {
        return;
    }

    printf("[if] 正在关闭 IF 模块...\n");

    g_if_local->running = 0;

    if (g_if_local->ipc_ctx)
    {
        ipc_destroy(g_if_local->ipc_ctx);
        g_if_local->ipc_ctx = NULL;
    }

    g_free(g_if_local);
    g_if_local = NULL;

    printf("[if] IF 模块清理完成\n");
}
