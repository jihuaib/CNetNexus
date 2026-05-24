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

#include "cli.h"
#include "cli_cfg.h"
#include "cli_main.h"
#include "cli_param_type.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"

/* 与 src/dev/dev_cli.h 中 DEV_CLI_GROUP_ID_PING 保持一致。 */
#define DEV_CLI_GROUP_ID_PING_COMPAT 5

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
 * 格式: [flags:u8][group_id:u32][TLV条目...][视图上下文条目（非 show 命令可选）]
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

    /* 1. flags（从 match result 标志检测 no/show 前缀） */
    uint8_t flags = 0;
    if (result->has_no_prefix)
    {
        flags |= CLI_PAYLOAD_FLAG_NO_CMD;
    }
    if (result->has_show_prefix)
    {
        flags |= CLI_PAYLOAD_FLAG_SHOW_CMD;
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

    /* 4. 上下文 TLV 条目
     * show/display 类命令应只由显式参数决定，不能继承当前视图上下文。
     */
    if (!result->has_show_prefix)
    {
        append_context_tlv(buf, ctx_data, ctx_len);
    }

    *out_len = buf->len;
    return g_byte_array_free(buf, FALSE);
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
 * @param ctx      合并后的上下文 TLV 字节（格式: [num:u16][ctx_id:u32][CLI_TLV_TYPE_CTX:u8][4:u16][u32]...）
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
                int64_t found_int = 0;
                const uint8_t *found_str_ptr = NULL;
                uint16_t found_str_len = 0;
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
                        if (id == ctx_id)
                        {
                            if (type == CLI_TLV_TYPE_CTX && elen == 4 && pos + 4 <= ctx_len)
                            {
                                uint32_t v;
                                memcpy(&v, ctx + pos, 4);
                                found_int = (int64_t)(uint32_t)ntohl(v);
                                found_str_ptr = NULL;
                            }
                            else if (type == CLI_TLV_TYPE_CTX_STR && pos + elen <= ctx_len)
                            {
                                found_str_ptr = ctx + pos;
                                found_str_len = elen;
                            }
                        }
                        pos += elen;
                    }
                }

                /* 写入值：字符串或整数 */
                if (found_str_ptr)
                {
                    for (uint16_t k = 0; k < found_str_len && out_pos < out_size - 1; k++)
                    {
                        out[out_pos++] = (char)found_str_ptr[k];
                    }
                }
                else
                {
                    char num_str[32];
                    int n = snprintf(num_str, sizeof(num_str), "%lld", (long long)found_int);
                    for (int k = 0; k < n && out_pos < out_size - 1; k++)
                    {
                        out[out_pos++] = num_str[k];
                    }
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

    /* 追加新条目：整数用 CLI_TLV_TYPE_CTX（4 字节 u32），字符串用 CLI_TLV_TYPE_CTX_STR（变长） */
    for (uint32_t i = 0; i < num_new; i++)
    {
        const cli_ctx_out_entry_t *e = &new_entries[i];
        uint32_t ctx_id_be = htonl(e->ctx_id);

        if (e->from_param != 0xFFFFFFFFU)
        {
            /* 从命令匹配参数中按 cfg_id 取值 */
            const char *param_val = NULL;
            for (uint32_t j = 0; j < result->num_elements; j++)
            {
                if (result->elements[j].cfg_id == e->from_param && result->elements[j].value)
                {
                    param_val = result->elements[j].value;
                    break;
                }
            }

            if (param_val)
            {
                char *endptr;
                unsigned long ival = strtoul(param_val, &endptr, 10);
                if (*endptr == '\0')
                {
                    /* 纯整数：存为 4 字节 u32 */
                    g_byte_array_append(buf, (const uint8_t *)&ctx_id_be, 4);
                    uint8_t type = CLI_TLV_TYPE_CTX;
                    g_byte_array_append(buf, &type, 1);
                    uint16_t len_be = htons(4);
                    g_byte_array_append(buf, (const uint8_t *)&len_be, 2);
                    uint32_t val_be = htonl((uint32_t)ival);
                    g_byte_array_append(buf, (const uint8_t *)&val_be, 4);
                }
                else
                {
                    /* 字符串：存为变长 CLI_TLV_TYPE_CTX_STR */
                    uint16_t slen = (uint16_t)strlen(param_val);
                    g_byte_array_append(buf, (const uint8_t *)&ctx_id_be, 4);
                    uint8_t type = CLI_TLV_TYPE_CTX_STR;
                    g_byte_array_append(buf, &type, 1);
                    uint16_t len_be = htons(slen);
                    g_byte_array_append(buf, (const uint8_t *)&len_be, 2);
                    if (slen > 0)
                    {
                        g_byte_array_append(buf, (const uint8_t *)param_val, slen);
                    }
                }
            }
            else
            {
                /* 参数未找到，用固定值回退 */
                g_byte_array_append(buf, (const uint8_t *)&ctx_id_be, 4);
                uint8_t type = CLI_TLV_TYPE_CTX;
                g_byte_array_append(buf, &type, 1);
                uint16_t len_be = htons(4);
                g_byte_array_append(buf, (const uint8_t *)&len_be, 2);
                uint32_t val_be = htonl(e->fixed_value);
                g_byte_array_append(buf, (const uint8_t *)&val_be, 4);
            }
        }
        else
        {
            /* 固定值：存为 4 字节 u32 */
            g_byte_array_append(buf, (const uint8_t *)&ctx_id_be, 4);
            uint8_t type = CLI_TLV_TYPE_CTX;
            g_byte_array_append(buf, &type, 1);
            uint16_t len_be = htons(4);
            g_byte_array_append(buf, (const uint8_t *)&len_be, 2);
            uint32_t val_be = htonl(e->fixed_value);
            g_byte_array_append(buf, (const uint8_t *)&val_be, 4);
        }
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

    /* CFG 自身命令在创建 IPC 消息之前直接本地处理（无目标模块概念，也不需要按需启动）。 */
    /* 按需启动触发：若目标模块未连接，先订阅 + auto_start=1 等其就绪
     *   - 配置命令（含 no）：触发按需 spawn；调用方等待几百毫秒后正常分发
     *   - show 命令：read-only，不应有副作用——目标不在跑就直接返回提示，不拉起进程
     * 这样像 TUNNEL 这种纯基础设施模块只会被业务模块（LDP/BGP-MPLS）显式拉起，
     * 用户输入 show 不会意外把它启动。 */
    if (result->module_id != DEV_MODULE_ID_CLI && !dev_ipc_is_connected(g_cli_local->dev_ipc_ctx, result->module_id))
    {
        /* CFG 被动等待业务模块主动建联（业务模块在自己 init 中 subscribe(CLI)）。
         * 此处不主动连任何模块。
         *   - show 命令：is_connected=false 就意味着模块没在跑，直接返回，零副作用
         *   - no 命令：目标既然不在跑，业务也不存在，没东西可"取消"，直接返回提示，
         *              避免 spawn → 立刻 self-exit 的无谓往返（如 `no bgp` / `no ldp` 时 on-demand 模块未起）
         *   - 配置命令：调 wait_module_ready 让 DEV 把按需模块拉起；目标 init 中会 subscribe(CLI)，
         *               届时 CFG 自然收到 inbound 连接，is_connected 变 true */
        if (result->has_show_prefix)
        {
            cli_send_message(session, "Info: target module is not running; no data to show.\r\n");
            return ERRCODE_SUCCESS;
        }
        if (result->has_no_prefix)
        {
            cli_send_message(session, "Info: target module is not running; nothing to undo.\r\n");
            return ERRCODE_SUCCESS;
        }
        cli_send_message(session, "[Starting module, please wait...]\r\n");
        if (dev_ipc_wait_module_ready(g_cli_local->dev_ipc_ctx, result->module_id, 15000) != ERRCODE_SUCCESS)
        {
            cli_send_message(session, "Error: Required module failed to start.\r\n");
            return ERRCODE_FAIL;
        }
    }

    /* 获取当前视图上下文 */
    uint32_t ctx_len = 0;
    const uint8_t *ctx_data = cli_context_get(session, &ctx_len);

    uint32_t msg_len = 0;
    uint8_t *msg_data = pack_tlv_payload(result, ctx_data, ctx_len, &msg_len);

    /* 创建 CLI 消息 */
    dev_ipc_message_t *msg =
        dev_ipc_message_create(CLI_MSG_TYPE, DEV_MODULE_ID_CLI, result->module_id, 0, msg_data, msg_len, g_free);
    if (!msg)
    {
        g_free(msg_data);
        return ERRCODE_FAIL;
    }

    /* CFG 模块本地处理（不走 IPC） */
    if (result->module_id == DEV_MODULE_ID_CLI)
    {
        cli_handle(msg, session);
        dev_ipc_message_free(msg);
        return ERRCODE_SUCCESS;
    }

    /* 使用同步查询等待响应，支持批量传输 */
    LOG_DEBUG("Sending query to module 0x%08X...", result->module_id);

    GString *full_output = g_string_new("");
    gboolean stream_live_output =
        (result->module_id == DEV_MODULE_ID_DEV && result->group_id == DEV_CLI_GROUP_ID_PING_COMPAT);
    int done = 0;

    while (!done)
    {
        /* 超时给到 60s：覆盖 process reboot/start 之类需要等模块 READY 才回响应的长命令。
         * 普通命令响应在毫秒级，长超时只有在对端真的失联或卡死时才影响 CLI 体验。 */
        dev_ipc_message_t *response = dev_ipc_query(g_cli_local->dev_ipc_ctx, result->module_id, msg, 60000);

        /* 释放查询消息（原始或 continue） */
        dev_ipc_message_free(msg);
        msg = NULL;

        if (!response)
        {
            if (full_output->len == 0)
            {
                cli_send_message(session, "Error: Module timed out or failed to respond.\r\n");
            }
            g_string_free(full_output, TRUE);
            return ERRCODE_FAIL;
        }

        if (response->msg_type == CLI_MSG_TYPE_RESP)
        {
            /* 最终响应块 */
            if (response->payload)
            {
                if (stream_live_output)
                {
                    cli_send_message(session, response->payload);
                }
                else
                {
                    g_string_append(full_output, response->payload);
                }
            }

            /* 自动视图切换：响应为空 + 命令有目标视图名（context-out 可选） */
            gboolean payload_empty = (!response->payload || strlen(response->payload) == 0);
            if (payload_empty && result->final_node && result->final_node->target_view_name != NULL)
            {
                cli_view_node_t *tgt_view =
                    cli_view_find_by_name(g_cli_local->view_tree.root, result->final_node->target_view_name);
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
                    LOG_DEBUG("Framework auto-switched to view %s, context %u bytes", tgt_view->view_name, new_ctx_len);
                }
            }

            dev_ipc_message_free(response);
            done = 1;
        }
        else if (response->msg_type == CLI_MSG_TYPE_RESP_MORE)
        {
            /* 部分响应 - 追加并请求更多 */
            if (response->payload)
            {
                if (stream_live_output)
                {
                    cli_send_message(session, response->payload);
                }
                else
                {
                    g_string_append(full_output, response->payload);
                }
            }
            dev_ipc_message_free(response);

            /* 发送 CONTINUE 请求下一批 */
            msg = dev_ipc_message_create(CLI_MSG_TYPE_CONTINUE, DEV_MODULE_ID_CLI, result->module_id, 0, NULL, 0, NULL);
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
    if (!stream_live_output && full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }

    g_string_free(full_output, TRUE);

    return ERRCODE_SUCCESS;
}
