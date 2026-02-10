/**
 * @file   dev_cli.c
 * @brief  Dev 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "dev_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "dev_main.h"
#include "dev_module.h"
#include "errcode.h"

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
// 命令处理函数（按 table_name 分发）
// ============================================================================

static int handle_show_module(ipc_context_t *ctx, ipc_message_t *msg, cli_db_payload_parser_t *parser)
{
    (void)parser;

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

static int handle_show_version(ipc_context_t *ctx, ipc_message_t *msg, cli_db_payload_parser_t *parser)
{
    (void)parser;

    char buf[CLI_MAX_RESP_LEN];
    snprintf(buf, sizeof(buf), "NetNexus Version 1.0.0\r\nBuild Time: %s %s\r\n", __DATE__, __TIME__);

    dev_send_cli_response(ctx, msg, buf);
    return ERRCODE_SUCCESS;
}

static int handle_sysname(ipc_context_t *ctx, ipc_message_t *msg, cli_db_payload_parser_t *parser)
{
    (void)parser;

    dev_send_cli_response(ctx, msg, "Command 'sysname' not yet implemented in dev module.\r\n");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 按 table_name 分发
// ============================================================================

static int dev_dispatch_db_payload(ipc_context_t *ctx, ipc_message_t *msg, cli_db_payload_parser_t *parser)
{
    if (strcmp(parser->table_name, "dev_show_version") == 0)
    {
        return handle_show_version(ctx, msg, parser);
    }
    else if (strcmp(parser->table_name, "dev_show_module") == 0)
    {
        return handle_show_module(ctx, msg, parser);
    }
    else if (strcmp(parser->table_name, "dev_sysname") == 0)
    {
        return handle_sysname(ctx, msg, parser);
    }

    printf("[dev_cli] 未知表名: %s\n", parser->table_name);
    dev_send_cli_response(ctx, msg, "Dev Error: Unknown command.\r\n");
    return ERRCODE_FAIL;
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

    cli_db_payload_parser_t parser;
    if (cli_db_payload_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        printf("[dev_cli] 载荷解析失败\n");
        dev_send_cli_response(ctx, msg, "Dev Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    printf("[dev_cli] 收到 DB table 载荷 (db=%s, table=%s)\n", parser.db_name, parser.table_name);
    int result = dev_dispatch_db_payload(ctx, msg, &parser);
    cli_db_payload_cleanup(&parser);
    return result;
}
