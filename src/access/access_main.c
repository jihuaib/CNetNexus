/**
 * @file   access_main.c
 * @brief  Access 模块主入口，IPC 初始化与模块管理
 * @author jhb
 * @date   2026/02/08
 */
#include "access_main.h"

#include <dirent.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "access_module.h"
#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"
#include "telnet/telnet_access.h"

access_local_t *g_access_local = NULL;

/**
 * @brief IPC 消息处理回调
 * @param ctx IPC 上下文
 * @param msg 接收到的消息
 */
static void access_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    (void)ctx;

    if (msg == NULL)
    {
        return;
    }

    // Access 模块目前主要处理 CLI 请求转发？
    // 或者它只是作为一个容器，具体的业务逻辑在各个接入模块中。
    // 目前暂时打印日志。
    printf("[access] 收到 IPC 消息 (type=0x%08X)\n", msg->msg_type);

    ipc_message_free(msg);
}

// 扫描并加载所有模块的 XML 配置文件
static int load_all_module_xmls(cli_engine_context_t *cli_ctx)
{
    if (!cli_ctx)
    {
        fprintf(stderr, "[access] Invalid CLI context\n");
        return -1;
    }

    // 获取可执行文件路径
    char exe_path[4096];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1)
    {
        perror("[access] Failed to get executable path");
        return -1;
    }
    exe_path[len] = '\0';

    // 获取 bin 目录路径
    char *exe_dir = dirname(exe_path);
    
    // 构建 src 目录路径 (假设 bin/../src)
    char src_dir[4096];
    snprintf(src_dir, sizeof(src_dir), "%s/../../src", exe_dir);

    printf("[access] Scanning for XML files in: %s\n", src_dir);

    // 打开 src 目录
    DIR *dir = opendir(src_dir);
    if (!dir)
    {
        perror("[access] Failed to open src directory");
        return -1;
    }

    int loaded_count = 0;
    struct dirent *entry;

    // 遍历 src 目录下的所有子目录
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type != DT_DIR || entry->d_name[0] == '.')
        {
            continue;
        }

        // 构建 commands.xml 路径: src/<module>/resources/commands.xml
        char xml_path[4096];
        snprintf(xml_path, sizeof(xml_path), "%s/%s/resources/commands.xml", 
                 src_dir, entry->d_name);

        // 检查文件是否存在
        if (access(xml_path, F_OK) == 0)
        {
            printf("[access] Loading XML: %s\n", xml_path);
            
            // 加载 XML 到视图树
            if (cli_xml_load_view_tree(xml_path, cli_engine_get_view_tree(cli_ctx)) == ERRCODE_SUCCESS)
            {
                printf("[access] Successfully loaded: %s\n", xml_path);
                loaded_count++;
            }
            else
            {
                fprintf(stderr, "[access] Failed to load: %s\n", xml_path);
            }
        }
    }

    closedir(dir);

    printf("[access] Loaded %d XML configuration files\n", loaded_count);
    return loaded_count > 0 ? 0 : -1;
}

int access_init(void)
{
    g_access_local = calloc(1, sizeof(access_local_t));
    if (g_access_local == NULL)
    {
        fprintf(stderr, "[access] 分配 access 上下文失败\n");
        return ERRCODE_FAIL;
    }

    /* 初始化 IPC (使用 DEV_MODULE_ID_ACCESS，假设在 dev.h 中定义或暂时使用新 ID) */
    /* 检查 dev.h 中是否有 DEV_MODULE_ID_ACCESS */
    /* 如果没有，暂时使用 DEV_MODULE_ID_CFG + 100 或者复用现有的？ */
    /* 通常需要 updating dev.h to add DEV_MODULE_ID_ACCESS */
    /* 暂时假设它存在，或者我需要去 dev.h 添加它。 */
    /* 让我们先检查 dev.h */
    
    // 假设 ID 为 0x00050000 (示例，需要确认)
    // 既然是独立的 Access 进程，应该有自己的 ID。
    // 但是等等，telnet_access.c 中初始化 CLI 引擎时使用了 DEV_MODULE_ID_CFG ("access")
    //  g_telnet_local->cli_ctx = cli_engine_init(DEV_MODULE_ID_CFG, "access");
    //  Access 进程作为一个整体，应该有一个 IPC ID。
    
    // 这里我们先用一个临时的 ID 或者检查 dev.h
    // 暂时用 0x00060000 ? 
    // 为了稳妥，我应该先检查 dev.h。
    
    // 先写代码，稍后由 user 确认 ID
    g_access_local->ipc_ctx =
        ipc_init(DEV_MODULE_ID_ACCESS, "access", NULL, access_ipc_msg_handler);
        
    if (g_access_local->ipc_ctx == NULL)
    {
        fprintf(stderr, "[access] IPC 初始化失败\n");
        free(g_access_local);
        g_access_local = NULL;
        return ERRCODE_FAIL;
    }
    // Initialize CLI engine (Module ID: CLI/CFG = 0x00000003)
    // Note: The access daemon hosts the CLI engine (CFG/CLI module functionality)
    g_access_local->cli_ctx = cli_engine_init(DEV_MODULE_ID_CFG, "access");
    if (!g_access_local->cli_ctx)
    {
        fprintf(stderr, "[access] CLI 引擎初始化失败\n");
        ipc_destroy(g_access_local->ipc_ctx);
        free(g_access_local);
        return ERRCODE_FAIL;
    }

    // Set CLI context for Telnet module (and others if needed)
    telnet_set_cli_context(g_access_local->cli_ctx);

    // Load XMLs
    load_all_module_xmls(g_access_local->cli_ctx);

    /* 注册接入模块 */
    if (access_module_register(&telnet_access_ops) != 0)
    {
        fprintf(stderr, "[access] 注册 Telnet 模块失败\n");
    }

    /* 启动所有接入模块 */
    access_module_start_all();

    g_access_local->running = 1;

    printf("[access] Access 模块初始化完成\n");
    return ERRCODE_SUCCESS;
}

void access_cleanup(void)
{
    if (g_access_local == NULL)
    {
        return;
    }

    printf("[access] 正在关闭 Access 模块...\n");

    access_module_stop_all();
    access_module_cleanup_all();

    g_access_local->running = 0;

    if (g_access_local->ipc_ctx != NULL)
    {
        ipc_destroy(g_access_local->ipc_ctx);
        g_access_local->ipc_ctx = NULL;
    }

    if (g_access_local->cli_ctx != NULL)
    {
        cli_engine_cleanup(g_access_local->cli_ctx);
        g_access_local->cli_ctx = NULL;
    }

    free(g_access_local);
    g_access_local = NULL;

    printf("[access] Access 模块清理完成\n");
}
