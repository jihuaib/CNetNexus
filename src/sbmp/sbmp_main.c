/**
 * @file   sbmp_main.c
 * @brief  SBMP（BMP Server）模块主入口，三阶段初始化和 IPC 消息处理
 * @author jhb
 * @date   2026/03/08
 */
#include "sbmp_main.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "sbmp_bdr.h"
#include "sbmp_cli.h"
#include "sbmp_db.h"

/** SBMP 模块全局状态 */
sbmp_local_t *g_sbmp_local = NULL;

// ============================================================================
// BMP listen socket 管理
// ============================================================================

void sbmp_listen_start(uint16_t port)
{
    if (!g_sbmp_local)
    {
        return;
    }
    if (g_sbmp_local->listen_fd >= 0)
    {
        /* 已在监听，幂等 */
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        LOG_PERROR("SBMP: 创建 listen socket 失败");
        return;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_PERROR("SBMP: bind 0.0.0.0:%u 失败", port);
        close(fd);
        return;
    }

    if (listen(fd, 16) < 0)
    {
        LOG_PERROR("SBMP: listen 失败");
        close(fd);
        return;
    }

    g_sbmp_local->listen_fd = fd;
    g_sbmp_local->server_port = port;
    LOG_INFO("SBMP: 开始监听 0.0.0.0:%u (fd=%d)", port, fd);
}

void sbmp_listen_stop(void)
{
    if (!g_sbmp_local || g_sbmp_local->listen_fd < 0)
    {
        return;
    }

    close(g_sbmp_local->listen_fd);
    g_sbmp_local->listen_fd = -1;
    LOG_INFO("SBMP: 停止监听端口 %u", g_sbmp_local->server_port);
}

// ============================================================================
// 三阶段回调辅助
// ============================================================================

static void send_phase_response(dev_ipc_context_t *ctx, dev_ipc_message_t *msg, int32_t result)
{
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_SBMP,
                                                     msg->src_module_id, msg->request_id, NULL, 0, NULL);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(msg);
    (void)result;
}

// ============================================================================
// Phase 1: MODULE_START — 建立 IPC 连接到 CFG
// ============================================================================

static void sbmp_on_start(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("SBMP Phase 1: MODULE_START — 建立 IPC 连接");
    dev_ipc_connect(ctx, DEV_MODULE_ID_CFG, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_CFG);
    dev_ipc_connect(ctx, DEV_MODULE_ID_DB, DEV_IPC_HOST_LOCAL, DEV_MODULE_PORT_DB);
    LOG_INFO("SBMP: 已连接到 CFG 和 DB");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 2: MODULE_CONNECT — 预留
// ============================================================================

static void sbmp_on_connect(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("SBMP Phase 2: MODULE_CONNECT (预留)");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Phase 3: MODULE_READY — 建表 + DB 恢复
// ============================================================================

static void sbmp_on_ready(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("SBMP Phase 3: MODULE_READY — 初始化数据库表并恢复状态");

    if (sbmp_db_init(ctx) != 0)
    {
        LOG_ERROR("SBMP: 数据库表初始化失败");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    if (sbmp_db_restore(ctx) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("SBMP: 从数据库恢复状态失败");
        send_phase_response(ctx, msg, ERRCODE_FAIL);
        return;
    }

    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// Shutdown
// ============================================================================

static void sbmp_on_shutdown(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    LOG_INFO("SBMP: 清理模块状态");

    sbmp_listen_stop();

    g_sbmp_local->dev_ipc_ctx = NULL;
    g_free(g_sbmp_local);
    g_sbmp_local = NULL;

    LOG_INFO("SBMP: 模块清理完成");
    send_phase_response(ctx, msg, ERRCODE_SUCCESS);
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void sbmp_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_MODULE_START:
            sbmp_on_start(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT:
            sbmp_on_connect(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_READY:
            sbmp_on_ready(ctx, msg);
            return;
        case DEV_IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN:
            sbmp_on_shutdown(ctx, msg);
            return;
        case CFG_MSG_TYPE_CLI:
            LOG_DEBUG("SBMP: 收到 CLI 命令消息");
            sbmp_cli_handle_message(msg);
            break;
        case CFG_MSG_TYPE_CLI_CONTINUE:
            LOG_DEBUG("SBMP: 收到 CLI continue 请求");
            sbmp_cli_handle_continue(msg);
            break;
        case CFG_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("SBMP: 收到 show current-configuration 请求");
            sbmp_bdr_show_config(msg);
            return;
        default:
            LOG_WARN("SBMP: 未知消息类型: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}

// ============================================================================
// .so constructor（dlopen 时自动触发）
// ============================================================================

__attribute__((constructor)) static void sbmp_so_init(void)
{
    LOG_INFO("SBMP: .so 加载，自初始化");

    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_SBMP, "sbmp", DEV_MODULE_PORT_SBMP, sbmp_msg_handler);
    if (!ctx)
    {
        LOG_ERROR("SBMP: IPC 初始化失败");
        return;
    }

    g_sbmp_local = g_malloc0(sizeof(sbmp_local_t));
    g_sbmp_local->dev_ipc_ctx = ctx;
    g_sbmp_local->server_port = 0;
    g_sbmp_local->listen_fd = -1;
}
