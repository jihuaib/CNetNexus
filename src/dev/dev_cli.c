/**
 * @file   dev_cli.c
 * @brief  Dev 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#define LOG_TAG "dev"

#include "dev_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev_main.h"
#include "dev_module.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 内部辅助函数：show 命令
// ============================================================================

static gboolean show_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_cli_resp_out_t *resp = (dev_cli_resp_out_t *)data;
    dev_module_t *module = (dev_module_t *)value;

    char line[128];
    snprintf(line, sizeof(line), "  %-12u %-15s\r\n", module->module_id, module->name);

    strncat(resp->message, line, sizeof(resp->message) - strlen(resp->message) - 1);

    return FALSE;
}

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void dev_send_cli_response(ipc_context_t *ctx, ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    ipc_message_t *resp_msg = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_DEV, msg->src_module_id,
                                                 msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp_msg)
    {
        ipc_send_response(ctx, resp_msg);
        ipc_message_free(resp_msg);
    }
}

// ============================================================================
// 命令处理函数（按 group_id 分发）
// ============================================================================

static int handle_show_module(ipc_context_t *ctx, ipc_message_t *msg)
{
    dev_cli_resp_out_t resp_out;
    memset(&resp_out, 0, sizeof(resp_out));

    snprintf(resp_out.message, sizeof(resp_out.message),
             "\r\nRegistered Modules:\r\n"
             "  %-12s %-15s\r\n"
             "  ----------------------------\r\n",
             "ID", "Name");

    dev_module_foreach(show_module_callback, &resp_out);

    strncat(resp_out.message, "\r\n", sizeof(resp_out.message) - strlen(resp_out.message) - 1);

    dev_send_cli_response(ctx, msg, resp_out.message);
    return ERRCODE_SUCCESS;
}

static int handle_show_version(ipc_context_t *ctx, ipc_message_t *msg)
{
    char buf[CLI_MAX_RESP_LEN];
    snprintf(buf, sizeof(buf), "NetNexus Version 1.0.0\r\nBuild Time: %s %s\r\n", __DATE__, __TIME__);

    dev_send_cli_response(ctx, msg, buf);
    return ERRCODE_SUCCESS;
}

static int handle_sysname(ipc_context_t *ctx, ipc_message_t *msg)
{
    dev_send_cli_response(ctx, msg, "Command 'sysname' not yet implemented in dev module.\r\n");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int dev_cli_handle_continue(ipc_context_t *ctx, ipc_message_t *msg)
{
    dev_send_cli_response(ctx, msg, "");
    return ERRCODE_SUCCESS;
}

int dev_cli_handle_message(ipc_context_t *ctx, ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        dev_send_cli_response(ctx, msg, "Dev Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case 1:
            result = handle_show_version(ctx, msg);
            break;
        case 3:
            result = handle_show_module(ctx, msg);
            break;
        case 2:
            result = handle_sysname(ctx, msg);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            dev_send_cli_response(ctx, msg, "Dev Error: Unknown command.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
