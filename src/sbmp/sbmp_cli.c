/**
 * @file   sbmp_cli.c
 * @brief  SBMP 模块 CLI 命令处理实现
 * @author jhb
 * @date   2026/03/08
 */
#include "sbmp_cli.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "sbmp_db.h"
#include "sbmp_main.h"

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void sbmp_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_SBMP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_sbmp_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理 "bmp-server" / "no bmp-server" 命令
 *
 * group_id=1：仅控制视图切换，no 命令清除 server port 配置
 */
static int handle_bmp_server(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;

    /* 消费 TLV 条目（本命令无参数，仅清空解析器） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    if (!is_no)
    {
        /* bmp-server：进入视图由 CLI 框架处理，此处只需回复 OK */
        sbmp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* no bmp-server：清除 server port 配置并停止监听 */
    sbmp_listen_stop();
    sbmp_db_del_server_port(g_sbmp_local->dev_ipc_ctx);
    g_sbmp_local->server_port = 0;

    sbmp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 "server port <port>" / "no server port" 命令
 *
 * group_id=2, cfg_id: 2=port-number
 */
static int handle_server_port(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    uint32_t port = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 2)
        {
            port = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    if (is_no)
    {
        /* no server port：停止监听并删除配置 */
        if (g_sbmp_local->server_port == 0)
        {
            sbmp_send_cli_response(msg, "");
            return ERRCODE_SUCCESS;
        }
        sbmp_listen_stop();
        sbmp_db_del_server_port(g_sbmp_local->dev_ipc_ctx);
        g_sbmp_local->server_port = 0;
        sbmp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* server port <port>：参数校验 */
    if (port == 0 || port > 65535)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Invalid port number (1-65535).\r\n");
        return ERRCODE_FAIL;
    }

    if (g_sbmp_local->server_port == (uint16_t)port)
    {
        /* 同配置短路 */
        sbmp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    /* 若已在其他端口监听，先停止 */
    if (g_sbmp_local->server_port != 0)
    {
        sbmp_listen_stop();
    }

    /* 启动新端口监听 */
    sbmp_listen_start((uint16_t)port);

    /* 写入数据库 */
    if (sbmp_db_set_server_port(g_sbmp_local->dev_ipc_ctx, (uint16_t)port) != 0)
    {
        sbmp_send_cli_response(msg, "SBMP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "SBMP: BMP server listening on port %u.\r\n", port);
    sbmp_send_cli_response(msg, resp);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 "show bmp server" 命令
 *
 * group_id=3：展示当前 BMP server 运行状态
 */
static int handle_show_bmp_server(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* 消费 TLV 条目 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    char buf[CLI_MAX_RESP_LEN];
    size_t off = 0;

    CLI_BUF_APPEND(buf, sizeof(buf), off, "BMP Server Status\r\n");
    CLI_BUF_APPEND(buf, sizeof(buf), off, "-----------------\r\n");

    if (g_sbmp_local->server_port == 0)
    {
        CLI_BUF_APPEND(buf, sizeof(buf), off, "  State : not configured\r\n");
    }
    else
    {
        const char *state = (g_sbmp_local->listen_fd >= 0) ? "running" : "stopped";
        CLI_BUF_APPEND(buf, sizeof(buf), off, "  Port  : %u\r\n", g_sbmp_local->server_port);
        CLI_BUF_APPEND(buf, sizeof(buf), off, "  State : %s\r\n", state);
    }

    sbmp_send_cli_response(msg, buf);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 消息分发入口
// ============================================================================

int sbmp_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("SBMP: 载荷解析失败");
        sbmp_send_cli_response(msg, "SBMP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("SBMP: 收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case SBMP_CLI_GROUP_ID_BMP_SERVER:
            result = handle_bmp_server(msg, &parser);
            break;
        case SBMP_CLI_GROUP_ID_SERVER_PORT:
            result = handle_server_port(msg, &parser);
            break;
        case SBMP_CLI_GROUP_ID_SHOW:
            result = handle_show_bmp_server(msg, &parser);
            break;
        default:
            LOG_WARN("SBMP: 未知 group_id: %u", parser.group_id);
            sbmp_send_cli_response(msg, "SBMP Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}

int sbmp_cli_handle_continue(dev_ipc_message_t *msg)
{
    /* SBMP 无分页输出，忽略 continue 请求 */
    dev_ipc_message_free(msg);
    return ERRCODE_SUCCESS;
}
