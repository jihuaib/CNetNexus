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
 * 存储格式: [num:u16][ctx_id:u32][CLI_TLV_TYPE_CTX:u8][len:u16][value...]...
 * payload 格式与存储格式相同（ctx_id 不与 cfg_id 共享命名空间，无需 flag 操作）
 * 直接将条目字节追加到 buf，跳过开头 num:u16 头部。
 */
static void append_context_tlv(GByteArray *buf, const uint8_t *ctx_data, uint32_t ctx_len)
{
    if (!ctx_data || ctx_len < 2)
    {
        return;
    }

    uint16_t num_be;
    memcpy(&num_be, ctx_data, 2);
    if (ntohs(num_be) == 0)
    {
        return;
    }

    /* 存储格式与 payload 格式完全一致，直接追加条目字节 */
    if (ctx_len > 2)
    {
        g_byte_array_append(buf, ctx_data + 2, ctx_len - 2);
    }
}

/**
 * @brief 打包 TLV 载荷
 *
 * 格式: [flags:u8][group_id:u32][TLV条目...][视图模板条目（可选）]
 *
 * @param result        命令匹配结果
 * @param ctx_data      上下文 TLV 数据
 * @param ctx_len       上下文数据长度
 * @param out_len       输出载荷长度
 */
static uint8_t *pack_tlv_payload(cli_match_result_t *result, const uint8_t *ctx_data, uint32_t ctx_len,
                                 uint32_t *out_len)
{
    GByteArray *buf = g_byte_array_new();

    /* 1. flags（从 match result 的 has_no_prefix 标志检测 "no" 前缀） */
    uint8_t flags = result->has_no_prefix ? CLI_PAYLOAD_FLAG_NO_CMD : 0;
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
/* 提示符占位符格式化                                                         */
/* ========================================================================= */

/**
 * @brief 格式化视图提示符模板，替换 {ctx:N} 为上下文变量整数值
 *
 * 示例: "<NetNexus(config-bgp-{ctx:1})>" + ctx_id=1=65000 → "<NetNexus(config-bgp-65000)>"
 *
 * @param tmpl     视图 prompt_template 字符串
 * @param ctx      合并后的上下文 TLV 字节（格式: [num:u16][ctx_id:u32][CLI_TLV_TYPE_CTX:u8][8:u16][i64]...）
 * @param ctx_len  上下文字节长度
 * @param out      输出缓冲区
 * @param out_size 缓冲区大小
 */
static void format_prompt_with_ctx(const char *tmpl, const uint8_t *ctx, uint32_t ctx_len, char *out, size_t out_size)
{
    if (!tmpl || !out || out_size == 0)
    {
        return;
    }

    size_t out_pos = 0;
    const char *p = tmpl;

    while (*p && out_pos < out_size - 1)
    {
        /* 匹配 {ctx:N} 模式 */
        if (p[0] == '{' && p[1] == 'c' && p[2] == 't' && p[3] == 'x' && p[4] == ':')
        {
            char *endp = NULL;
            uint32_t ctx_id = (uint32_t)strtoul(p + 5, &endp, 10);
            if (endp && *endp == '}')
            {
                /* 在上下文中查找 ctx_id */
                int64_t found_val = 0;
                if (ctx && ctx_len >= 2)
                {
                    uint16_t num_be;
                    memcpy(&num_be, ctx, 2);
                    uint16_t num = ntohs(num_be);
                    uint32_t pos = 2;
                    for (uint16_t i = 0; i < num && pos < ctx_len; i++)
                    {
                        if (pos + 4 > ctx_len)
                        {
                            break;
                        }
                        uint32_t id_be;
                        memcpy(&id_be, ctx + pos, 4);
                        uint32_t id = ntohl(id_be);
                        pos += 4;
                        if (pos >= ctx_len)
                        {
                            break;
                        }
                        uint8_t type = ctx[pos++];
                        if (pos + 2 > ctx_len)
                        {
                            break;
                        }
                        uint16_t len_be;
                        memcpy(&len_be, ctx + pos, 2);
                        uint16_t elen = ntohs(len_be);
                        pos += 2;
                        if (id == ctx_id && type == CLI_TLV_TYPE_CTX && elen == 8 && pos + 8 <= ctx_len)
                        {
                            uint32_t hi, lo;
                            memcpy(&hi, ctx + pos, 4);
                            memcpy(&lo, ctx + pos + 4, 4);
                            found_val = ((int64_t)ntohl(hi) << 32) | (int64_t)(uint32_t)ntohl(lo);
                        }
                        pos += elen;
                    }
                }

                /* 写入整数值字符串 */
                char num_str[32];
                int n = snprintf(num_str, sizeof(num_str), "%lld", (long long)found_val);
                for (int k = 0; k < n && out_pos < out_size - 1; k++)
                {
                    out[out_pos++] = num_str[k];
                }
                p = endp + 1; /* 跳过 '}' */
                continue;
            }
        }
        out[out_pos++] = *p++;
    }
    out[out_pos] = '\0';
}

/* ========================================================================= */
/* 上下文自动积累辅助                                                         */
/* ========================================================================= */

/**
 * @brief 合并父视图上下文 + 新增条目，生成新层完整上下文 TLV
 *
 * 格式: [num:u16][cfg_id:u32][type:u8][len:u16][value...]...
 * 仅支持整数型值（DB_TYPE_INTEGER, 8 字节 i64）。
 *
 * @param session    当前会话（从中读取父层上下文）
 * @param result     命令匹配结果（用于 from_param 取值）
 * @param new_entries context_out 条目数组
 * @param num_new    条目数量
 * @param out_ctx    输出分配的 TLV 字节（调用者负责 g_free）
 * @param out_len    输出长度
 */
static void cli_context_build_merged(cli_session_t *session, cli_match_result_t *result,
                                     const cli_ctx_out_entry_t *new_entries, uint32_t num_new, uint8_t **out_ctx,
                                     uint32_t *out_len)
{
    /* 读取父层上下文 */
    uint32_t parent_len = 0;
    const uint8_t *parent = cli_context_get(session, &parent_len);

    /* 解析父层条目数 */
    uint16_t parent_num = 0;
    if (parent && parent_len >= 2)
    {
        uint16_t be;
        memcpy(&be, parent, 2);
        parent_num = ntohs(be);
    }

    GByteArray *buf = g_byte_array_new();

    /* 写入总条目数 */
    uint16_t total_be = htons(parent_num + (uint16_t)num_new);
    g_byte_array_append(buf, (const uint8_t *)&total_be, 2);

    /* 复制父层条目原始字节（跳过开头 u16 num） */
    if (parent && parent_len > 2)
    {
        g_byte_array_append(buf, parent + 2, parent_len - 2);
    }

    /* 追加新条目：[ctx_id:u32][CLI_TLV_TYPE_CTX:u8][8:u16][i64_value] */
    for (uint32_t i = 0; i < num_new; i++)
    {
        const cli_ctx_out_entry_t *e = &new_entries[i];

        int64_t val = e->fixed_value;
        if (e->from_param >= 0)
        {
            /* 从命令匹配参数中按 cfg_id（XML cfg-id）取值 */
            for (uint32_t j = 0; j < result->num_elements; j++)
            {
                if ((int32_t)result->elements[j].cfg_id == e->from_param && result->elements[j].value)
                {
                    char *endptr;
                    val = strtoll(result->elements[j].value, &endptr, 10);
                    break;
                }
            }
        }

        /* ctx_id 独立命名空间，type 用 CLI_TLV_TYPE_CTX 与命令参数区分 */
        uint32_t ctx_id_be = htonl(e->ctx_id);
        g_byte_array_append(buf, (const uint8_t *)&ctx_id_be, 4);
        uint8_t type = CLI_TLV_TYPE_CTX;
        g_byte_array_append(buf, &type, 1);
        uint16_t len_be = htons(8);
        g_byte_array_append(buf, (const uint8_t *)&len_be, 2);
        uint32_t hi = htonl((uint32_t)((uint64_t)val >> 32));
        uint32_t lo = htonl((uint32_t)(val & 0xFFFFFFFFu));
        g_byte_array_append(buf, (const uint8_t *)&hi, 4);
        g_byte_array_append(buf, (const uint8_t *)&lo, 4);
    }

    *out_len = buf->len;
    *out_ctx = g_byte_array_free(buf, FALSE);
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
    dev_ipc_message_t *msg =
        dev_ipc_message_create(CFG_MSG_TYPE_CLI, DEV_MODULE_ID_CFG, result->module_id, 0, msg_data, msg_len, g_free);
    if (!msg)
    {
        g_free(msg_data);
        return ERRCODE_FAIL;
    }

    /* CFG 模块本地处理（不走 IPC） */
    if (result->module_id == DEV_MODULE_ID_CFG)
    {
        cfg_cli_handle(msg, session);
        dev_ipc_message_free(msg);
        return ERRCODE_SUCCESS;
    }

    /* 使用同步查询等待响应，支持批量传输 */
    LOG_DEBUG("Sending query to module 0x%08X...", result->module_id);

    GString *full_output = g_string_new("");
    int done = 0;

    while (!done)
    {
        dev_ipc_message_t *response = dev_ipc_query(g_cfg_local->dev_ipc_ctx, result->module_id, msg, 5000);

        /* 释放查询消息（原始或 continue） */
        dev_ipc_message_free(msg);
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

        if (response->msg_type == CFG_MSG_TYPE_CLI_RESP)
        {
            /* 最终响应块 */
            if (response->payload)
            {
                g_string_append(full_output, response->payload);
            }

            /* 自动视图切换：响应为空 + 命令有 view_id + XML 定义了 context-out */
            gboolean payload_empty = (!response->payload || strlen(response->payload) == 0);
            if (payload_empty && result->final_node && result->final_node->view_id != 0 &&
                result->final_node->num_context_out > 0)
            {
                cli_view_node_t *tgt_view =
                    cli_view_find_by_id(g_cfg_local->view_tree.root, result->final_node->view_id);
                if (tgt_view)
                {
                    uint8_t *new_ctx = NULL;
                    uint32_t new_ctx_len = 0;
                    cli_context_build_merged(session, result, result->final_node->context_out,
                                             result->final_node->num_context_out, &new_ctx, &new_ctx_len);

                    cli_prompt_push(session);
                    session->current_view = tgt_view;
                    /* 支持 {ctx:N} 占位符，用合并后的上下文值格式化提示符 */
                    format_prompt_with_ctx(tgt_view->prompt_template, new_ctx, new_ctx_len, session->prompt,
                                           sizeof(session->prompt));

                    if (new_ctx)
                    {
                        cli_context_set(session, new_ctx, new_ctx_len);
                        g_free(new_ctx);
                    }
                    LOG_DEBUG("框架自动切换到视图 %u，上下文 %u 字节", tgt_view->view_id, new_ctx_len);
                }
            }

            dev_ipc_message_free(response);
            done = 1;
        }
        else if (response->msg_type == CFG_MSG_TYPE_CLI_RESP_MORE)
        {
            /* 部分响应 - 追加并请求更多 */
            if (response->payload)
            {
                g_string_append(full_output, response->payload);
            }
            dev_ipc_message_free(response);

            /* 发送 CONTINUE 请求下一批 */
            msg = dev_ipc_message_create(CFG_MSG_TYPE_CLI_CONTINUE, DEV_MODULE_ID_CFG, result->module_id, 0, NULL, 0,
                                         NULL);
            if (!msg)
            {
                done = 1;
            }
        }
        else
        {
            dev_ipc_message_free(response);
            done = 1;
        }
    }

    /* 输出结果 */
    if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }

    g_string_free(full_output, TRUE);

    return ERRCODE_SUCCESS;
}
