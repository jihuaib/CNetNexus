/**
 * @file   bgp_cli.c
 * @brief  BGP 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "bgp_main.h"
#include "cli.h"
#include "db.h"
#include "db_rpc.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

// ============================================================================
// Show 输出写入临时 DB
// ============================================================================

#define BGP_SHOW_DB "bgp_show_db"
#define BGP_SHOW_META "bgp_show_meta"
#define BGP_SHOW_ROW "bgp_peer_show"

static int bgp_insert_show_meta(int has_rows)
{
    const char *fields[] = {"has_rows"};
    db_value_t values[] = {db_value_int(has_rows ? 1 : 0)};
    return db_rpc_insert(g_bgp_local->ipc_ctx, BGP_SHOW_DB, BGP_SHOW_META, fields, values, 1);
}

static int bgp_insert_show_row(int peer_index, const char *field, const char *value)
{
    const char *fields[] = {"peer_index", "field", "value"};
    db_value_t values[] = {db_value_int(peer_index), db_value_text(field), db_value_text(value)};
    int ret = db_rpc_insert(g_bgp_local->ipc_ctx, BGP_SHOW_DB, BGP_SHOW_ROW, fields, values, 3);
    db_value_free(&values[1]);
    db_value_free(&values[2]);
    return ret;
}

// ============================================================================
// 上下文序列化辅助（新字段格式）
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

static void ctx_write_i64(GByteArray *buf, int64_t v)
{
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    g_byte_array_append(buf, (const uint8_t *)&hi, 4);
    g_byte_array_append(buf, (const uint8_t *)&lo, 4);
}

static void ctx_write_string(GByteArray *buf, const char *s)
{
    if (!s)
    {
        ctx_write_u16(buf, 0xFFFF);
        return;
    }
    uint16_t len = (uint16_t)strlen(s);
    ctx_write_u16(buf, len);
    if (len > 0)
    {
        g_byte_array_append(buf, (const uint8_t *)s, len);
    }
}

// ============================================================================
// DB table 载荷处理函数
// ============================================================================

/**
 * @brief 处理 bgp_protocol 表命令（bgp <as-number> / no bgp [as-number]）
 */
static int handle_bgp_protocol_db(ipc_message_t *msg, cli_db_payload_parser_t *parser)
{
    gboolean is_no = CFG_DB_IS_NO_CMD(parser);
    uint32_t as_number = 0;
    int has_as_number = 0;

    /* 解析字段 */
    uint8_t ff;
    char *fn;
    db_value_t val;
    while (cfg_db_payload_next(parser, &ff, &fn, &val) == 1)
    {
        if (strcmp(fn, "as_number") == 0 && val.type == DB_TYPE_INTEGER)
        {
            as_number = (uint32_t)val.data.i64;
            has_as_number = 1;
        }
        g_free(fn);
        db_value_free(&val);
    }

    char resp_msg_buf[CLI_MAX_RESP_LEN];
    resp_msg_buf[0] = '\0';

    /* 删除场景 */
    if (is_no)
    {
        if (has_as_number)
        {
            char where[64];
            snprintf(where, sizeof(where), "as_number = %u", as_number);
            int rows = db_rpc_delete(g_bgp_local->ipc_ctx, parser->db_name, parser->table_name, where);
            snprintf(resp_msg_buf, sizeof(resp_msg_buf), "BGP: AS %u deleted (%d row).\r\n", as_number,
                     rows > 0 ? rows : 0);
        }
        else
        {
            int rows = db_rpc_delete(g_bgp_local->ipc_ctx, parser->db_name, parser->table_name, NULL);
            snprintf(resp_msg_buf, sizeof(resp_msg_buf), "BGP: All configuration deleted (%d row).\r\n",
                     rows > 0 ? rows : 0);
        }

        char *resp_data = g_strdup(resp_msg_buf);
        ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                 msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
        if (resp)
        {
            ipc_send_response(g_bgp_local->ipc_ctx, resp);
            ipc_message_free(resp);
        }
        return ERRCODE_SUCCESS;
    }

    /* 配置场景 */
    if (!has_as_number)
    {
        char *resp_data = g_strdup("BGP Error: Missing required AS number parameter.\r\n");
        ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                 msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
        if (resp)
        {
            ipc_send_response(g_bgp_local->ipc_ctx, resp);
            ipc_message_free(resp);
        }
        return ERRCODE_FAIL;
    }

    /* 插入或更新 */
    gboolean exists = FALSE;
    int ret = db_rpc_exists(g_bgp_local->ipc_ctx, parser->db_name, parser->table_name, NULL, &exists);
    if (ret != ERRCODE_SUCCESS)
    {
        char *resp_data = g_strdup("BGP Error: Database query failed.\r\n");
        ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                 msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
        if (resp)
        {
            ipc_send_response(g_bgp_local->ipc_ctx, resp);
            ipc_message_free(resp);
        }
        return ERRCODE_FAIL;
    }

    const char *field_names[] = {"as_number"};
    db_value_t values[] = {db_value_int((int64_t)as_number)};

    if (exists)
    {
        db_rpc_update(g_bgp_local->ipc_ctx, parser->db_name, parser->table_name, field_names, values, 1, NULL);
        printf("[bgp_cli] Updated BGP AS number to %u\n", as_number);
    }
    else
    {
        db_rpc_insert(g_bgp_local->ipc_ctx, parser->db_name, parser->table_name, field_names, values, 1);
        printf("[bgp_cli] Inserted BGP AS number %u\n", as_number);
    }

    /* 发送 VIEW_CHG 响应 */
    char view_name[CFG_CLI_MAX_VIEW_LEN];

    // 通过IPC从CFG模块获取view prompt template
    if (g_bgp_local->ipc_ctx && ipc_is_connected(g_bgp_local->ipc_ctx, DEV_MODULE_ID_CFG))
    {
        // 构建请求：view_id (4字节)
        uint32_t view_id_be = htonl(CLI_VIEW_BGP);
        ipc_message_t *req =
            ipc_message_create(CFG_MSG_TYPE_GET_VIEW_PROMPT, DEV_MODULE_ID_BGP, DEV_MODULE_ID_CFG, msg->request_id,
                               g_memdup(&view_id_be, sizeof(view_id_be)), sizeof(view_id_be), g_free);

        if (req)
        {
            ipc_message_t *resp = ipc_query(g_bgp_local->ipc_ctx, DEV_MODULE_ID_CFG, req, 1000);
            if (resp && resp->payload && resp->payload_len > 0)
            {
                snprintf(view_name, sizeof(view_name), "%s", (char *)resp->payload);
                ipc_message_free(resp);
                ret = ERRCODE_SUCCESS;
            }
            else
            {
                // IPC失败，使用默认模板
                snprintf(view_name, sizeof(view_name), "<NetNexus(bgp-%u)>");
                if (resp)
                {
                    ipc_message_free(resp);
                }
                ret = ERRCODE_SUCCESS;
            }
            ipc_message_free(req);
        }
        else
        {
            snprintf(view_name, sizeof(view_name), "<NetNexus(bgp-%u)>");
            ret = ERRCODE_SUCCESS;
        }
    }
    else
    {
        // CFG未连接，使用默认模板
        snprintf(view_name, sizeof(view_name), "<NetNexus(bgp-%u)>");
        ret = ERRCODE_SUCCESS;
    }

    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    char out_prompt[CLI_CLI_MAX_PROMPT_LEN];
    snprintf(out_prompt, CLI_CLI_MAX_PROMPT_LEN, view_name, as_number);

    /* 构建新格式上下文: [num_fields:u16][field_flags:u8][field_name:string][value_type:u8][value] */
    GByteArray *ctx_buf = g_byte_array_new();
    ctx_write_u16(ctx_buf, 1); /* num_fields = 1 */
    ctx_write_u8(ctx_buf, 0);  /* field_flags = 0 */
    ctx_write_string(ctx_buf, "as_number");
    ctx_write_u8(ctx_buf, (uint8_t)DB_TYPE_INTEGER);
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
 * @brief 处理 bgp_peer 表 show 命令
 */
static int handle_bgp_peer_show_db(ipc_message_t *msg, cli_db_payload_parser_t *parser)
{
    db_result_t *result = NULL;
    int ret = db_rpc_query(g_bgp_local->ipc_ctx, parser->db_name, parser->table_name, NULL, 0, NULL, &result);

    if (ret != ERRCODE_SUCCESS || !result)
    {
        char *resp_data = g_strdup("BGP Error: Database query failed.\r\n");
        ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                 msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
        if (resp)
        {
            ipc_send_response(g_bgp_local->ipc_ctx, resp);
            ipc_message_free(resp);
        }
        return ERRCODE_FAIL;
    }

    if (bgp_insert_show_meta(result->num_rows > 0) != ERRCODE_SUCCESS)
    {
        db_result_free(result);
        char *resp_data = g_strdup("BGP Error: Failed to write show output.\r\n");
        ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                 msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
        if (resp)
        {
            ipc_send_response(g_bgp_local->ipc_ctx, resp);
            ipc_message_free(resp);
        }
        return ERRCODE_FAIL;
    }

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

            if (bgp_insert_show_row((int)i + 1, row->field_names[j], value_str) != ERRCODE_SUCCESS)
            {
                db_result_free(result);
                char *resp_data = g_strdup("BGP Error: Failed to write show output.\r\n");
                ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                         msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
                if (resp)
                {
                    ipc_send_response(g_bgp_local->ipc_ctx, resp);
                    ipc_message_free(resp);
                }
                return ERRCODE_FAIL;
            }
        }
    }

    db_result_free(result);

    char *resp_data = g_strdup("");
    ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                             msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        ipc_send_response(g_bgp_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 使用 DB table 载荷格式分发命令
 */
static int bgp_dispatch_db_payload(ipc_message_t *msg, cli_db_payload_parser_t *parser)
{
    if (strcmp(parser->table_name, "bgp_protocol") == 0)
    {
        return handle_bgp_protocol_db(msg, parser);
    }
    else if (strcmp(parser->table_name, "bgp_peer") == 0)
    {
        return handle_bgp_peer_show_db(msg, parser);
    }

    printf("[bgp_cfg] 未知表名: %s\n", parser->table_name);
    char *resp_data = g_strdup("BGP Error: Unknown table.\r\n");
    ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                             msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        ipc_send_response(g_bgp_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }
    return ERRCODE_FAIL;
}

// ============================================================================
// 主入口
// ============================================================================

int bgp_cli_handle_continue(ipc_message_t *msg)
{
    char *resp_data = g_strdup("");
    ipc_message_t *resp_msg = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                 msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp_msg)
    {
        ipc_send_response(g_bgp_local->ipc_ctx, resp_msg);
        ipc_message_free(resp_msg);
    }
    return ERRCODE_SUCCESS;
}

int bgp_cli_handle_message(ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    /* 尝试新 DB table 载荷格式 */
    cli_db_payload_parser_t parser;
    if (cfg_db_payload_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) == 0)
    {
        printf("[bgp_cfg] 收到 DB table 载荷 (db=%s, table=%s)\n", parser.db_name, parser.table_name);
        int result = bgp_dispatch_db_payload(msg, &parser);
        cfg_db_payload_cleanup(&parser);
        return result;
    }

    /* 回退到旧 TLV 格式（兼容） */
    printf("[bgp_cfg] 回退到旧 TLV 格式\n");
    char *resp_data = g_strdup("BGP Error: Unsupported message format.\r\n");
    ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                             msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        ipc_send_response(g_bgp_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }
    return ERRCODE_FAIL;
}
