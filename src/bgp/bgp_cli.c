/**
 * @file   bgp_cli.c
 * @brief  BGP 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#define LOG_TAG "bgp"
#include "bgp_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "bgp_db.h"
#include "bgp_main.h"
#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"
#include "log.h"

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void bgp_send_cli_response(ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                             msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        ipc_send_response(g_bgp_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }
}

// ============================================================================
// 上下文序列化辅助（TLV 格式）
// ============================================================================

static void ctx_write_u8(GByteArray *buf, uint8_t v)
{
    g_byte_array_append(buf, &v, 1);
}

static void ctx_write_u16(GByteArray *buf, uint16_t v)
{
    uint16_t be = htons(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 2);
}

static void ctx_write_u32(GByteArray *buf, uint32_t v)
{
    uint32_t be = htonl(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 4);
}

static void ctx_write_i64(GByteArray *buf, int64_t v)
{
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    g_byte_array_append(buf, (const uint8_t *)&hi, 4);
    g_byte_array_append(buf, (const uint8_t *)&lo, 4);
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理 bgp 配置命令（bgp <as-number> / no bgp [as-number]）
 *
 * group_id=1, cfg_id: 1=no, 2=as_number
 */
static int handle_bgp_protocol(ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = CLI_TLV_IS_NO_CMD(parser);
    uint32_t as_number = 0;
    int has_as_number = 0;

    /* 按 cfg_id 解析 TLV 条目 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 2: /* as_number 参数 */
                as_number = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_as_number = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    char resp_msg_buf[CLI_MAX_RESP_LEN];
    resp_msg_buf[0] = '\0';

    /* 删除场景 */
    if (is_no)
    {
        int rows = bgp_db_del_as(g_bgp_local->ipc_ctx, as_number, has_as_number);
        if (has_as_number)
        {
            snprintf(resp_msg_buf, sizeof(resp_msg_buf), "BGP: AS %u deleted (%d row).\r\n", as_number,
                     rows > 0 ? rows : 0);
        }
        else
        {
            snprintf(resp_msg_buf, sizeof(resp_msg_buf), "BGP: All configuration deleted (%d row).\r\n",
                     rows > 0 ? rows : 0);
        }
        bgp_send_cli_response(msg, resp_msg_buf);
        return ERRCODE_SUCCESS;
    }

    /* 配置场景 */
    if (!has_as_number)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing required AS number parameter.\r\n");
        return ERRCODE_FAIL;
    }

    /* 插入或更新 */
    if (bgp_db_set_as(g_bgp_local->ipc_ctx, as_number) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    /* 发送 VIEW_CHG 响应 */
    char view_name[CFG_CLI_MAX_VIEW_LEN];

    /* 通过 IPC 从 CFG 模块获取 view prompt template */
    if (g_bgp_local->ipc_ctx && ipc_is_connected(g_bgp_local->ipc_ctx, DEV_MODULE_ID_CFG))
    {
        uint32_t view_id_be = htonl(CLI_VIEW_BGP);
        uint32_t *view_id_copy = g_malloc(sizeof(view_id_be));
        memcpy(view_id_copy, &view_id_be, sizeof(view_id_be));
        ipc_message_t *req = ipc_message_create(CFG_MSG_TYPE_CLI_CONTINUE, DEV_MODULE_ID_BGP, DEV_MODULE_ID_CFG,
                                                msg->request_id, view_id_copy, sizeof(view_id_be), g_free);

        if (req)
        {
            req->msg_type = IPC_MSG_TYPE(IPC_CATEGORY_CLI, 0x0010); /* GET_VIEW_PROMPT */
            ipc_message_t *resp = ipc_query(g_bgp_local->ipc_ctx, DEV_MODULE_ID_CFG, req, 1000);
            if (resp && resp->payload && resp->payload_len > 0)
            {
                snprintf(view_name, sizeof(view_name), "%s", (char *)resp->payload);
                ipc_message_free(resp);
            }
            else
            {
                snprintf(view_name, sizeof(view_name), "<NetNexus(bgp-%%u)>");
                if (resp)
                {
                    ipc_message_free(resp);
                }
            }
            ipc_message_free(req);
        }
        else
        {
            snprintf(view_name, sizeof(view_name), "<NetNexus(bgp-%%u)>");
        }
    }
    else
    {
        snprintf(view_name, sizeof(view_name), "<NetNexus(bgp-%%u)>");
    }

    char out_prompt[CLI_CLI_MAX_PROMPT_LEN];
    snprintf(out_prompt, CLI_CLI_MAX_PROMPT_LEN, view_name, as_number);

    /* 构建新格式上下文: [num:u16][cfg_id:u32][type:u8][length:u16][value] */
    GByteArray *ctx_buf = g_byte_array_new();
    ctx_write_u16(ctx_buf, 1); /* num_fields = 1 */
    ctx_write_u32(ctx_buf, 2); /* cfg_id = 2 (as_number) */
    ctx_write_u8(ctx_buf, (uint8_t)DB_TYPE_INTEGER);
    ctx_write_u16(ctx_buf, 8); /* int64 长度 */
    ctx_write_i64(ctx_buf, (int64_t)as_number);

    uint32_t total_len = CLI_CLI_MAX_PROMPT_LEN + ctx_buf->len;
    char *msg_out = g_malloc0(total_len);
    memcpy(msg_out, out_prompt, CLI_CLI_MAX_PROMPT_LEN);
    memcpy(msg_out + CLI_CLI_MAX_PROMPT_LEN, ctx_buf->data, ctx_buf->len);
    g_byte_array_free(ctx_buf, TRUE);

    ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_VIEW_CHG, DEV_MODULE_ID_BGP, msg->src_module_id,
                                             msg->request_id, msg_out, total_len, g_free);
    if (resp)
    {
        ipc_send_response(g_bgp_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 show bgp peer 命令
 *
 * group_id=2, 直接构建格式化文本返回
 */
static int handle_bgp_peer_show(ipc_message_t *msg)
{
    db_result_t *result = NULL;
    int ret = bgp_db_query(g_bgp_local->ipc_ctx, &result);

    if (ret != 0 || !result)
    {
        bgp_send_cli_response(msg, "BGP Error: Database query failed.\r\n");
        return ERRCODE_FAIL;
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    int offset = 0;

    if (result->num_rows == 0)
    {
        offset += snprintf(resp_buf + offset, sizeof(resp_buf) - offset, "No BGP configuration found.\r\n");
    }
    else
    {
        offset += snprintf(resp_buf + offset, sizeof(resp_buf) - offset, "\r\nBGP Information:\r\n");
        offset += snprintf(resp_buf + offset, sizeof(resp_buf) - offset, "============================\r\n");

        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            db_row_t *row = result->rows[i];
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                char value_str[256] = {0};
                switch (row->values[j].type)
                {
                    case DB_TYPE_INTEGER:
                        snprintf(value_str, sizeof(value_str), "%ld", row->values[j].data.i64);
                        break;
                    case DB_TYPE_REAL:
                        snprintf(value_str, sizeof(value_str), "%.6g", row->values[j].data.real);
                        break;
                    case DB_TYPE_TEXT:
                        if (row->values[j].data.text)
                        {
                            snprintf(value_str, sizeof(value_str), "%s", row->values[j].data.text);
                        }
                        break;
                    default:
                        snprintf(value_str, sizeof(value_str), "NULL");
                        break;
                }
                offset += snprintf(resp_buf + offset, sizeof(resp_buf) - offset, "  %-20s: %s\r\n", row->field_names[j],
                                   value_str);
            }
        }
        offset += snprintf(resp_buf + offset, sizeof(resp_buf) - offset, "\r\n");
    }

    db_result_free(result);
    bgp_send_cli_response(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int bgp_cli_handle_continue(ipc_message_t *msg)
{
    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

int bgp_cli_handle_message(ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        bgp_send_cli_response(msg, "BGP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case 1: /* bgp 配置命令 */
            result = handle_bgp_protocol(msg, &parser);
            break;
        case 2: /* show bgp peer */
            result = handle_bgp_peer_show(msg);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            bgp_send_cli_response(msg, "BGP Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
