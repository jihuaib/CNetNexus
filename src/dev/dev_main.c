/**
 * @file   dev_main.c
 * @brief  Dev 模块主入口，模块注册和 IPC 消息处理
 * @author jhb
 * @date   2026/01/22
 */
#define LOG_TAG "dev"

#include "dev_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "dev_cli.h"
#include "dev_module.h"
#include "errcode.h"
#include "log.h"
#include "module_ports.h"

dev_local_t *g_dev_local = NULL;

static void dev_handle_get_module_name(ipc_context_t *ctx, ipc_message_t *msg)
{
    ipc_dev_get_module_name_resp_t *resp_payload = g_malloc0(sizeof(ipc_dev_get_module_name_resp_t));
    resp_payload->result = ERRCODE_FAIL;

    if (msg->payload && msg->payload_len == sizeof(ipc_dev_get_module_name_req_t))
    {
        const ipc_dev_get_module_name_req_t *req = (const ipc_dev_get_module_name_req_t *)msg->payload;
        if (dev_get_module_name_inner(req->module_id, resp_payload->name) == ERRCODE_SUCCESS)
        {
            resp_payload->result = ERRCODE_SUCCESS;
        }
    }

    ipc_message_t *resp = ipc_message_create(IPC_MSG_TYPE_DEV_GET_MODULE_NAME_RESP, DEV_MODULE_ID_DEV, msg->src_module_id,
                                             msg->request_id, resp_payload, sizeof(*resp_payload), g_free);
    if (resp)
    {
        ipc_send_response(ctx, resp);
        ipc_message_free(resp);
    }
    else
    {
        g_free(resp_payload);
    }
}

// ============================================================================
// IPC 消息处理回调
// ============================================================================

void dev_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        case CFG_MSG_TYPE_CLI:
            LOG_DEBUG("Received CLI command message");
            dev_cli_handle_message(ctx, msg);
            break;

        case CFG_MSG_TYPE_CLI_CONTINUE:
            LOG_DEBUG("Received CLI continue request");
            dev_cli_handle_continue(ctx, msg);
            break;
        case IPC_MSG_TYPE_DEV_GET_MODULE_NAME:
            dev_handle_get_module_name(ctx, msg);
            break;

        default:
            break;
    }

    ipc_message_free(msg);
}

/**
 * @brief DEV 自身初始化（在三阶段流程开始前调用）
 *
 * 创建 DEV 的 IPC context 并设置到全局上下文和模块注册表中。
 * @return ERRCODE_SUCCESS 或 ERRCODE_FAIL
 */
int dev_init_self(void)
{
    g_dev_local = g_malloc0(sizeof(dev_local_t));

    g_dev_local->ipc_ctx = ipc_init(DEV_MODULE_ID_DEV, "dev", MODULE_PORT_DEV, dev_msg_handler);
    if (!g_dev_local->ipc_ctx)
    {
        LOG_ERROR("Failed to initialize IPC");
        g_free(g_dev_local);
        g_dev_local = NULL;
        return ERRCODE_FAIL;
    }

    /* 注册 DEV 模块到 GTree */
    dev_add_module_to_registry(DEV_MODULE_ID_DEV, "dev");

    LOG_INFO("DEV 自身 IPC 初始化完成");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 获取 DEV 的 IPC context
 * @return DEV 的 IPC context
 */
ipc_context_t *dev_get_ipc_ctx(void)
{
    if (g_dev_local)
    {
        return g_dev_local->ipc_ctx;
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

    /* 注意：IPC context 的销毁由 cleanup_all_modules() 统一处理 */
    g_dev_local->ipc_ctx = NULL;

    g_free(g_dev_local);
    g_dev_local = NULL;
}
