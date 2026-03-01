/**
 * @file   cli_dispatch.c
 * @brief  CLI 命令分发，TLV 消息打包和模块路由
 * @author jhb
 * @date   2026/01/22
 */

#include "cli_dispatch.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cfg_cli.h"
#include "cfg_main.h"
#include "cli.h"
#include "cli_param_type.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"
#include "log.h"

/* ========================================================================= */
/* TLV 载荷写入辅助函数                                                       */
/* ========================================================================= */

static void tlv_write_u8(GByteArray *buf, uint8_t v)
{
    g_byte_array_append(buf, &v, 1);
}

static void tlv_write_u16(GByteArray *buf, uint16_t v)
{
    uint16_t be = htons(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 2);
}

static void tlv_write_u32(GByteArray *buf, uint32_t v)
{
    uint32_t be = htonl(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 4);
}

static void tlv_write_i64(GByteArray *buf, int64_t v)
{
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    g_byte_array_append(buf, (const uint8_t *)&hi, 4);
    g_byte_array_append(buf, (const uint8_t *)&lo, 4);
}

/**
 * @brief 写入单个 TLV 条目: [cfg_id:u32][type:u8][length:u16][value:bytes]
 */
static void tlv_write_entry(GByteArray *buf, uint32_t cfg_id, cli_match_element_t *elem)
{
    tlv_write_u32(buf, cfg_id);

    if (!elem->value)
    {
        /* 无值关键字 */
        tlv_write_u8(buf, DB_TYPE_NULL);
        tlv_write_u16(buf, 0);
        return;
    }

    /* 根据 param_type 决定编码方式 */
    if (elem->param_type)
    {
        switch (elem->param_type->type)
        {
            case PARAM_TYPE_UINT:
            case PARAM_TYPE_INT:
            {
                char *endptr;
                long long val = strtoll(elem->value, &endptr, 10);
                if (*endptr == '\0')
                {
                    tlv_write_u8(buf, DB_TYPE_INTEGER);
                    tlv_write_u16(buf, 8);
                    tlv_write_i64(buf, (int64_t)val);
                    return;
                }
                break;
            }
            default:
                break;
        }
    }

    /* 默认作为字符串 */
    uint16_t slen = (uint16_t)strlen(elem->value);
    tlv_write_u8(buf, DB_TYPE_TEXT);
    tlv_write_u16(buf, slen);
    if (slen > 0)
    {
        g_byte_array_append(buf, (const uint8_t *)elem->value, slen);
    }
}

/**
 * @brief 追加上下文 TLV 条目到载荷
 *
 * 上下文格式: [num:u16][cfg_id:u32][type:u8][length:u16][value]...
 * 写入载荷时加 CFG_TLV_CONTEXT_FLAG
 */
static void append_context_tlv(GByteArray *buf, const uint8_t *ctx_data, uint32_t ctx_len)
{
    if (!ctx_data || ctx_len < 2)
    {
        return;
    }

    uint16_t num_be;
    memcpy(&num_be, ctx_data, 2);
    uint16_t num = ntohs(num_be);

    if (num == 0)
    {
        return;
    }

    uint32_t pos = 2;
    for (uint16_t i = 0; i < num && pos < ctx_len; i++)
    {
        /* 读取 cfg_id */
        if (pos + 4 > ctx_len)
        {
            break;
        }
        uint32_t cfg_id_be;
        memcpy(&cfg_id_be, ctx_data + pos, 4);
        uint32_t cfg_id = ntohl(cfg_id_be);
        pos += 4;

        /* 读取 type */
        if (pos >= ctx_len)
        {
            break;
        }
        uint8_t type = ctx_data[pos];
        pos++;

        /* 读取 length */
        if (pos + 2 > ctx_len)
        {
            break;
        }
        uint16_t length_be;
        memcpy(&length_be, ctx_data + pos, 2);
        uint16_t length = ntohs(length_be);
        pos += 2;

        /* 写入条目，加 CONTEXT_FLAG */
        tlv_write_u32(buf, cfg_id | CFG_TLV_CONTEXT_FLAG);
        tlv_write_u8(buf, type);
        tlv_write_u16(buf, length);

        if (length > 0 && pos + length <= ctx_len)
        {
            g_byte_array_append(buf, ctx_data + pos, length);
            pos += length;
        }
    }
}

/**
 * @brief 打包 TLV 载荷
 *
 * 格式: [flags:u8][group_id:u32][TLV条目...]
 */
static uint8_t *pack_tlv_payload(cli_match_result_t *result, const uint8_t *ctx_data, uint32_t ctx_len,
                                 uint32_t *out_len)
{
    GByteArray *buf = g_byte_array_new();

    /* 1. flags（检测 "no" 关键字） */
    uint8_t flags = 0;
    for (uint32_t i = 0; i < result->num_elements; i++)
    {
        if (result->elements[i].type == CLI_NODE_COMMAND && result->elements[i].value != NULL &&
            strcmp(result->elements[i].value, "no") == 0)
        {
            flags |= CLI_PAYLOAD_FLAG_NO_CMD;
            break;
        }
    }
    tlv_write_u8(buf, flags);

    /* 2. group_id */
    tlv_write_u32(buf, result->group_id);

    /* 3. 命令参数 TLV 条目（cfg_id > 0 的元素） */
    for (uint32_t i = 0; i < result->num_elements; i++)
    {
        if (result->elements[i].cfg_id > 0)
        {
            tlv_write_entry(buf, result->elements[i].cfg_id, &result->elements[i]);
        }
    }

    /* 4. 上下文 TLV 条目（加 CONTEXT_FLAG） */
    append_context_tlv(buf, ctx_data, ctx_len);

    *out_len = buf->len;
    return g_byte_array_free(buf, FALSE);
}

/* ========================================================================= */
/* TLV 载荷解析器实现                                                         */
/* ========================================================================= */

int cli_tlv_init(cli_tlv_parser_t *p, const uint8_t *data, uint32_t len)
{
    if (!p || !data || len < 5)
    {
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->_data = data;
    p->_len = len;
    p->_pos = 0;

    /* flags: u8 */
    p->flags = data[p->_pos++];

    /* group_id: u32 */
    if (p->_pos + 4 > p->_len)
    {
        return -1;
    }
    uint32_t be;
    memcpy(&be, data + p->_pos, 4);
    p->group_id = ntohl(be);
    p->_pos += 4;

    return 0;
}

int cli_tlv_next(cli_tlv_parser_t *p, cli_tlv_entry_t *entry)
{
    if (!p || !entry)
    {
        return -1;
    }

    if (p->_pos >= p->_len)
    {
        return 0;
    }

    /* cfg_id: u32 */
    if (p->_pos + 4 > p->_len)
    {
        return 0;
    }
    uint32_t id_be;
    memcpy(&id_be, p->_data + p->_pos, 4);
    entry->cfg_id = ntohl(id_be);
    p->_pos += 4;

    /* type: u8 */
    if (p->_pos >= p->_len)
    {
        return -1;
    }
    entry->type = p->_data[p->_pos++];

    /* length: u16 */
    if (p->_pos + 2 > p->_len)
    {
        return -1;
    }
    uint16_t len_be;
    memcpy(&len_be, p->_data + p->_pos, 2);
    entry->length = ntohs(len_be);
    p->_pos += 2;

    /* value: bytes（多分配 1 字节用于 NUL 终止，方便文本读取） */
    if (entry->length > 0)
    {
        if (p->_pos + entry->length > p->_len)
        {
            return -1;
        }
        entry->value = g_malloc(entry->length + 1);
        memcpy(entry->value, p->_data + p->_pos, entry->length);
        entry->value[entry->length] = '\0';
        p->_pos += entry->length;
    }
    else
    {
        entry->value = NULL;
    }

    return 1;
}

void cli_tlv_entry_free(cli_tlv_entry_t *entry)
{
    if (entry)
    {
        g_free(entry->value);
        entry->value = NULL;
    }
}

void cli_tlv_cleanup(cli_tlv_parser_t *p)
{
    if (p)
    {
        memset(p, 0, sizeof(*p));
    }
}

int64_t cli_tlv_entry_get_int(const cli_tlv_entry_t *entry)
{
    if (!entry || entry->type != DB_TYPE_INTEGER || entry->length != 8 || !entry->value)
    {
        return 0;
    }
    uint32_t hi, lo;
    memcpy(&hi, entry->value, 4);
    memcpy(&lo, entry->value + 4, 4);
    hi = ntohl(hi);
    lo = ntohl(lo);
    return ((int64_t)hi << 32) | lo;
}

const char *cli_tlv_entry_get_text(const cli_tlv_entry_t *entry)
{
    if (!entry || entry->type != DB_TYPE_TEXT || !entry->value)
    {
        return NULL;
    }
    /* value 在 tlv_next 中多分配了 1 字节并追加了 NUL */
    return (const char *)entry->value;
}

/* ========================================================================= */
/* 命令分发                                                                   */
/* ========================================================================= */

int cli_dispatch_to_module(cli_match_result_t *result, cli_session_t *session)
{
    if (!result || result->module_id == 0 || !session)
    {
        return ERRCODE_FAIL;
    }

    /* 获取当前视图上下文 */
    uint32_t ctx_len = 0;
    const uint8_t *ctx_data = cli_context_get(session, &ctx_len);

    uint32_t msg_len = 0;
    uint8_t *msg_data = pack_tlv_payload(result, ctx_data, ctx_len, &msg_len);

    /* 创建 CLI 消息 */
    ipc_message_t *msg =
        ipc_message_create(CFG_MSG_TYPE_CLI, DEV_MODULE_ID_CFG, result->module_id, 0, msg_data, msg_len, g_free);
    if (!msg)
    {
        g_free(msg_data);
        return ERRCODE_FAIL;
    }

    /* CFG 模块本地处理（不走 IPC） */
    if (result->module_id == DEV_MODULE_ID_CFG)
    {
        cfg_cli_handle(msg, session);
        ipc_message_free(msg);
        return ERRCODE_SUCCESS;
    }

    /* 使用同步查询等待响应，支持批量传输 */
    LOG_DEBUG("Sending query to module 0x%08X...", result->module_id);

    GString *full_output = g_string_new("");
    cli_view_node_t *view = NULL;
    int done = 0;

    while (!done)
    {
        ipc_message_t *response = ipc_query(g_cfg_local->ipc_ctx, result->module_id, msg, 5000);

        /* 释放查询消息（原始或 continue） */
        ipc_message_free(msg);
        msg = NULL;

        if (!response)
        {
            if (full_output->len == 0)
            {
                cfg_send_message(session, "Error: Module timed out or failed to respond.\r\n");
            }
            g_string_free(full_output, TRUE);
            return ERRCODE_FAIL;
        }

        if (response->msg_type == CFG_MSG_TYPE_CLI_VIEW_CHG)
        {
            char module_prompt[CLI_CLI_MAX_PROMPT_LEN] = {0};

            if (response->payload && response->payload_len > 0)
            {
                snprintf(module_prompt, sizeof(module_prompt), "%s", (char *)response->payload);
            }

            if (result->final_node != NULL)
            {
                view = cli_view_find_by_id(g_cfg_local->view_tree.root, result->final_node->view_id);
            }

            if (module_prompt[0] != '\0' && view != NULL)
            {
                cli_prompt_push(session);
                session->current_view = view;
                update_prompt_from_template(session, module_prompt);

                /* 提取上下文 TLV（prompt 之后的剩余数据） */
                if (response->payload_len > CLI_CLI_MAX_PROMPT_LEN)
                {
                    uint32_t view_ctx_len = response->payload_len - CLI_CLI_MAX_PROMPT_LEN;
                    const uint8_t *view_ctx_data = (const uint8_t *)response->payload + CLI_CLI_MAX_PROMPT_LEN;
                    cli_context_set(session, view_ctx_data, view_ctx_len);
                    LOG_DEBUG("Saved view context (%u bytes)", view_ctx_len);
                }
            }

            ipc_message_free(response);
            done = 1;
        }
        else if (response->msg_type == CFG_MSG_TYPE_CLI_RESP)
        {
            /* 最终响应块 */
            if (response->payload)
            {
                g_string_append(full_output, response->payload);
            }
            ipc_message_free(response);
            done = 1;
        }
        else if (response->msg_type == CFG_MSG_TYPE_CLI_RESP_MORE)
        {
            /* 部分响应 - 追加并请求更多 */
            if (response->payload)
            {
                g_string_append(full_output, response->payload);
            }
            ipc_message_free(response);

            /* 发送 CONTINUE 请求下一批 */
            msg = ipc_message_create(CFG_MSG_TYPE_CLI_CONTINUE, DEV_MODULE_ID_CFG, result->module_id, 0, NULL, 0, NULL);
            if (!msg)
            {
                done = 1;
            }
        }
        else
        {
            ipc_message_free(response);
            done = 1;
        }
    }

    /* 输出结果 */
    if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }
    else
    {
        cfg_send_message(session, "No data.\r\n");
    }

    g_string_free(full_output, TRUE);

    return ERRCODE_SUCCESS;
}
