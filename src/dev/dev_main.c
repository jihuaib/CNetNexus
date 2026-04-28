/**
 * @file   dev_main.c
 * @brief  Dev 模块主入口，模块注册和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */

#include "dev_main.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "dev_bdr.h"
#include "dev_cli.h"
#include "dev_module.h"
#include "errcode.h"
#include "log.h"

dev_local_t *g_dev_local = NULL;

// ============================================================================
// IPC 消息处理回调
// ============================================================================

/** 处理模块名称查询请求 */
static void handle_dev_get_module_name(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = dev_get_ipc_ctx();
    char name[DEV_MODULE_NAME_MAX_LEN] = "";

    if (msg->payload && msg->payload_len >= sizeof(uint32_t))
    {
        uint32_t module_id;
        memcpy(&module_id, msg->payload, sizeof(uint32_t));
        dev_get_module_name_inner(module_id, name);
    }

    /* 响应 payload 为模块名称字符串（含终止符） */
    size_t name_len = strlen(name) + 1;
    char *resp_data = g_strdup(name);
    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_RESP, DEV_MODULE_ID_DEV,
                                                     msg->src_module_id, msg->request_id, resp_data, name_len, g_free);
    dev_ipc_send_response(ctx, resp);
    dev_ipc_message_free(resp);
}

void dev_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    (void)ctx;
    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DEV_GET_MODULE_NAME:
            handle_dev_get_module_name(msg);
            break;

        case CLI_MSG_TYPE_QUERY_CANDIDATES:
            dev_cli_handle_query_candidates(msg);
            return; /* msg 由 handler 内部释放 */

        case CLI_MSG_TYPE:
            LOG_DEBUG("Received CLI command message");
            dev_cli_handle_message(msg);
            break;

        case CLI_MSG_TYPE_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            dev_cli_handle_continue(msg);
            break;

        case CLI_MSG_TYPE_SHOW_CONFIG:
            LOG_DEBUG("Received show current-configuration request");
            dev_bdr_show_config(msg);
            break;

        default:
            break;
    }

    dev_ipc_message_free(msg);
}

/**
 * @brief DEV 自身初始化（在三阶段流程开始前调用）
 *
 * 创建 DEV 的 IPC context 并设置到全局上下文和模块注册表中。
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int dev_init_self(void)
{
    LOG_INFO("DEV IPC initialization started========================");
    g_dev_local = g_malloc0(sizeof(dev_local_t));

    g_dev_local->dev_ipc_ctx = dev_ipc_init(DEV_MODULE_ID_DEV, "dev", DEV_MODULE_PORT_DEV, dev_msg_handler);
    if (!g_dev_local->dev_ipc_ctx)
    {
        LOG_ERROR("Failed to initialize IPC");
        g_free(g_dev_local);
        g_dev_local = NULL;
        return ERRCODE_FAIL;
    }

    /* 注册 DEV 模块到 GTree，并补充端口号 */
    dev_module_t *dev_self = dev_add_module_to_registry(DEV_MODULE_ID_DEV, "dev");
    if (dev_self)
    {
        dev_self->port = DEV_MODULE_PORT_DEV;
    }

    LOG_INFO("DEV IPC initialization complete========================");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 广播 log-level 给所有已注册模块（DEV 自身已本地生效，无需再发自己）
 */
static gboolean broadcast_log_level_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    uint32_t level_be = *(uint32_t *)user_data;

    if (module->module_id == DEV_MODULE_ID_DEV || module->phase < DEV_PHASE_IPC_READY)
    {
        return FALSE;
    }

    dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_SET_LOG_LEVEL, DEV_MODULE_ID_DEV,
                                                    module->module_id, 0, &level_be, sizeof(level_be), NULL);
    if (!req)
    {
        return FALSE;
    }
    /* 单向通知：接收端在 IPC 库层透明处理，无需等待响应 */
    dev_ipc_send(g_dev_local->dev_ipc_ctx, module->module_id, req);
    dev_ipc_message_free(req);
    return FALSE;
}

void dev_broadcast_log_level(uint32_t level)
{
    uint32_t level_be = htonl(level);
    dev_module_foreach(broadcast_log_level_cb, &level_be);
}

/**
 * @brief 获取 DEV 的 IPC context
 * @return DEV 的 IPC context
 */
dev_ipc_context_t *dev_get_ipc_ctx(void)
{
    if (g_dev_local)
    {
        return g_dev_local->dev_ipc_ctx;
    }
    return NULL;
}

/**
 * @brief DEV 清理
 */
void dev_cleanup_self(void)
{
    if (g_dev_local == NULL)
    {
        return;
    }

    LOG_INFO("Dev module cleanup");

    dev_cli_cleanup_state();

    /* 注意：IPC context 的销毁由 cleanup_all_modules() 统一处理 */
    g_dev_local->dev_ipc_ctx = NULL;

    g_free(g_dev_local);
    g_dev_local = NULL;
}
