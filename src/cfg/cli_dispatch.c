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
#include "cfg_template_renderer.h"
#include "cli.h"
#include "cli_param_type.h"
#include "config_template.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

/* ========================================================================= */
/* Show 模板关联表清理                                                      */
/* ========================================================================= */

static void collect_template_tables(config_template_t *tmpl, GHashTable *table_set)
{
    if (!tmpl || !table_set)
    {
        return;
    }

    if (tmpl->body && tmpl->body->db_names)
    {
        for (uint32_t i = 0; i < tmpl->body->num_dbs; i++)
        {
            const char *ref = tmpl->body->db_names[i];
            if (ref && ref[0] != '\0' && !g_hash_table_lookup(table_set, ref))
            {
                g_hash_table_insert(table_set, g_strdup(ref), GINT_TO_POINTER(1));
            }
        }
    }

    for (uint32_t i = 0; i < tmpl->num_children; i++)
    {
        const char *child_name = tmpl->child_template_names[i];
        if (child_name)
        {
            config_template_t *child = config_template_find_by_name(child_name);
            collect_template_tables(child, table_set);
        }
    }
}

static void clear_tables_for_template(const char *template_name)
{
    if (!template_name || template_name[0] == '\0')
    {
        return;
    }

    config_template_t *tmpl = config_template_find_by_name(template_name);
    if (!tmpl)
    {
        return;
    }

    GHashTable *table_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    collect_template_tables(tmpl, table_set);

    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, table_set);
    while (g_hash_table_iter_next(&iter, &key, NULL))
    {
        const char *table_ref = (const char *)key;
        if (!table_ref)
        {
            continue;
        }

        gchar **parts = g_strsplit(table_ref, ".", 2);
        if (parts && parts[0] && parts[1])
        {
            db_delete(parts[0], parts[1], NULL);
        }
        g_strfreev(parts);
    }

    g_hash_table_destroy(table_set);
}

/* ========================================================================= */
/* DB table 载荷写入辅助函数                                                  */
/* ========================================================================= */

static void db_write_u8(GByteArray *buf, uint8_t v)
{
    g_byte_array_append(buf, &v, 1);
}

static void db_write_u16(GByteArray *buf, uint16_t v)
{
    uint16_t be = htons(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 2);
}

static void db_write_i64(GByteArray *buf, int64_t v)
{
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    g_byte_array_append(buf, (const uint8_t *)&hi, 4);
    g_byte_array_append(buf, (const uint8_t *)&lo, 4);
}

static void db_write_string(GByteArray *buf, const char *s)
{
    if (!s)
    {
        db_write_u16(buf, 0xFFFF);
        return;
    }
    uint16_t len = (uint16_t)strlen(s);
    db_write_u16(buf, len);
    if (len > 0)
    {
        g_byte_array_append(buf, (const uint8_t *)s, len);
    }
}

static void db_write_value(GByteArray *buf, const db_value_t *val)
{
    db_write_u8(buf, (uint8_t)val->type);
    switch (val->type)
    {
        case DB_TYPE_NULL:
            break;
        case DB_TYPE_INTEGER:
            db_write_i64(buf, val->data.i64);
            break;
        case DB_TYPE_REAL:
        {
            double d = val->data.real;
            g_byte_array_append(buf, (const uint8_t *)&d, sizeof(double));
            break;
        }
        case DB_TYPE_TEXT:
            db_write_string(buf, val->data.text);
            break;
        default:
            break;
    }
}

// 将参数元素的字符串值转换为 db_value_t
static db_value_t convert_elem_to_db_value(cli_match_element_t *elem)
{
    if (!elem->value)
    {
        return db_value_null();
    }

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
                    return db_value_int((int64_t)val);
                }
                break;
            }
            default:
                break;
        }
    }

    /* 默认作为字符串 */
    return db_value_text(elem->value);
}

// 统计有 field_name 的参数字段数
static uint16_t count_param_fields(cli_match_result_t *result)
{
    uint16_t count = 0;
    for (uint32_t i = 0; i < result->num_elements; i++)
    {
        if (result->elements[i].field_name)
        {
            count++;
        }
    }
    return count;
}

// 统计上下文中的字段数（新格式上下文）
static uint16_t count_ctx_fields(const uint8_t *ctx_data, uint32_t ctx_len)
{
    if (!ctx_data || ctx_len < 2)
    {
        return 0;
    }
    uint16_t be;
    memcpy(&be, ctx_data, 2);
    return ntohs(be);
}

// 追加上下文字段到载荷（上下文格式: [num_fields:u16][field entries...]）
static void append_context_fields(GByteArray *buf, const uint8_t *ctx_data, uint32_t ctx_len)
{
    if (!ctx_data || ctx_len < 2)
    {
        return;
    }

    /* 上下文数据格式与载荷字段格式相同，但不含 flags/db/table 头部 */
    /* 格式: [num_ctx_fields:u16] [field_flags:u8][field_name:string][value_type:u8][value]... */
    uint16_t num_ctx;
    uint16_t be;
    memcpy(&be, ctx_data, 2);
    num_ctx = ntohs(be);

    if (num_ctx == 0)
    {
        return;
    }

    /* 跳过 num_fields，直接复制字段条目并标记为 CONTEXT */
    uint32_t pos = 2;
    for (uint16_t i = 0; i < num_ctx && pos < ctx_len; i++)
    {
        /* 读取原始 field_flags */
        if (pos >= ctx_len)
        {
            break;
        }
        uint8_t orig_flags = ctx_data[pos];
        pos++;

        /* 写入带 CONTEXT 标记的 flags */
        db_write_u8(buf, orig_flags | CLI_FIELD_FLAG_CONTEXT);

        /* 复制 field_name（uint16 len + bytes） */
        if (pos + 2 > ctx_len)
        {
            break;
        }
        uint16_t name_len_be;
        memcpy(&name_len_be, ctx_data + pos, 2);
        uint16_t name_len = ntohs(name_len_be);
        g_byte_array_append(buf, ctx_data + pos, 2); /* len */
        pos += 2;

        if (name_len != 0xFFFF && name_len > 0)
        {
            if (pos + name_len > ctx_len)
            {
                break;
            }
            g_byte_array_append(buf, ctx_data + pos, name_len);
            pos += name_len;
        }

        /* 复制 value_type + value 数据 */
        if (pos >= ctx_len)
        {
            break;
        }
        uint8_t vtype = ctx_data[pos];
        db_write_u8(buf, vtype);
        pos++;

        /* 根据类型跳过对应字节 */
        switch (vtype)
        {
            case DB_TYPE_NULL:
                break;
            case DB_TYPE_INTEGER:
                if (pos + 8 <= ctx_len)
                {
                    g_byte_array_append(buf, ctx_data + pos, 8);
                    pos += 8;
                }
                break;
            case DB_TYPE_REAL:
                if (pos + 8 <= ctx_len)
                {
                    g_byte_array_append(buf, ctx_data + pos, 8);
                    pos += 8;
                }
                break;
            case DB_TYPE_TEXT:
            {
                if (pos + 2 > ctx_len)
                {
                    break;
                }
                uint16_t slen_be;
                memcpy(&slen_be, ctx_data + pos, 2);
                uint16_t slen = ntohs(slen_be);
                g_byte_array_append(buf, ctx_data + pos, 2);
                pos += 2;
                if (slen != 0xFFFF && slen > 0 && pos + slen <= ctx_len)
                {
                    g_byte_array_append(buf, ctx_data + pos, slen);
                    pos += slen;
                }
                break;
            }
            default:
                break;
        }
    }
}

// 打包 DB table 载荷
static uint8_t *pack_db_payload(cli_match_result_t *result, const uint8_t *ctx_data, uint32_t ctx_len,
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
    db_write_u8(buf, flags);

    /* 2. db_name + table_name */
    db_write_string(buf, result->db_name);
    db_write_string(buf, result->table_name);

    /* 3. 统计字段数 */
    uint16_t param_count = count_param_fields(result);
    uint16_t ctx_count = count_ctx_fields(ctx_data, ctx_len);
    db_write_u16(buf, param_count + ctx_count);

    /* 4. 命令参数字段 */
    for (uint32_t i = 0; i < result->num_elements; i++)
    {
        if (!result->elements[i].field_name)
        {
            continue;
        }
        db_write_u8(buf, 0); /* field_flags = 0（非上下文） */
        db_write_string(buf, result->elements[i].field_name);
        db_value_t val = convert_elem_to_db_value(&result->elements[i]);
        db_write_value(buf, &val);
        db_value_free(&val);
    }

    /* 5. 上下文字段（标记 CONTEXT flag） */
    append_context_fields(buf, ctx_data, ctx_len);

    *out_len = buf->len;
    return g_byte_array_free(buf, FALSE);
}

/* ========================================================================= */
/* DB table 载荷读取辅助函数                                                  */
/* ========================================================================= */

static int payload_read_u8(cli_db_payload_parser_t *p, uint8_t *out)
{
    if (p->_reader_pos + 1 > p->_reader_len)
    {
        return -1;
    }
    *out = p->_reader_data[p->_reader_pos++];
    return 0;
}

static int payload_read_u16(cli_db_payload_parser_t *p, uint16_t *out)
{
    if (p->_reader_pos + 2 > p->_reader_len)
    {
        return -1;
    }
    uint16_t be;
    memcpy(&be, p->_reader_data + p->_reader_pos, 2);
    *out = ntohs(be);
    p->_reader_pos += 2;
    return 0;
}

static int payload_read_i64(cli_db_payload_parser_t *p, int64_t *out)
{
    if (p->_reader_pos + 8 > p->_reader_len)
    {
        return -1;
    }
    uint32_t hi, lo;
    memcpy(&hi, p->_reader_data + p->_reader_pos, 4);
    memcpy(&lo, p->_reader_data + p->_reader_pos + 4, 4);
    hi = ntohl(hi);
    lo = ntohl(lo);
    *out = ((int64_t)hi << 32) | lo;
    p->_reader_pos += 8;
    return 0;
}

static char *payload_read_string(cli_db_payload_parser_t *p)
{
    uint16_t len;
    if (payload_read_u16(p, &len) < 0)
    {
        return NULL;
    }
    if (len == 0xFFFF)
    {
        return NULL;
    }
    if (p->_reader_pos + len > p->_reader_len)
    {
        return NULL;
    }
    char *s = g_malloc(len + 1);
    memcpy(s, p->_reader_data + p->_reader_pos, len);
    s[len] = '\0';
    p->_reader_pos += len;
    return s;
}

static int payload_read_value(cli_db_payload_parser_t *p, db_value_t *val)
{
    uint8_t type;
    if (payload_read_u8(p, &type) < 0)
    {
        return -1;
    }
    val->type = (db_value_type_t)type;

    switch (val->type)
    {
        case DB_TYPE_NULL:
            break;
        case DB_TYPE_INTEGER:
            if (payload_read_i64(p, &val->data.i64) < 0)
            {
                return -1;
            }
            break;
        case DB_TYPE_REAL:
            if (p->_reader_pos + sizeof(double) > p->_reader_len)
            {
                return -1;
            }
            memcpy(&val->data.real, p->_reader_data + p->_reader_pos, sizeof(double));
            p->_reader_pos += sizeof(double);
            break;
        case DB_TYPE_TEXT:
            val->data.text = payload_read_string(p);
            break;
        default:
            return -1;
    }
    return 0;
}

int cli_db_payload_init(cli_db_payload_parser_t *p, const uint8_t *data, uint32_t len)
{
    if (!p || !data || len < 1)
    {
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->_reader_data = data;
    p->_reader_len = len;
    p->_reader_pos = 0;

    /* flags */
    if (payload_read_u8(p, &p->flags) < 0)
    {
        return -1;
    }

    /* db_name（可为 NULL，无 db/table 关联的命令） */
    p->db_name = payload_read_string(p);
    if (!p->db_name)
    {
        p->db_name = g_strdup("");
    }

    /* table_name（可为 NULL） */
    p->table_name = payload_read_string(p);
    if (!p->table_name)
    {
        p->table_name = g_strdup("");
    }

    /* num_fields */
    if (payload_read_u16(p, &p->num_fields) < 0)
    {
        g_free(p->db_name);
        g_free(p->table_name);
        p->db_name = NULL;
        p->table_name = NULL;
        return -1;
    }

    p->fields_read = 0;
    return 0;
}

int cli_db_payload_next(cli_db_payload_parser_t *p, uint8_t *out_flags, char **out_field_name, void *out_value)
{
    if (!p || p->fields_read >= p->num_fields)
    {
        return 0;
    }

    /* field_flags */
    uint8_t ff;
    if (payload_read_u8(p, &ff) < 0)
    {
        return -1;
    }
    if (out_flags)
    {
        *out_flags = ff;
    }

    /* field_name */
    char *fn = payload_read_string(p);
    if (!fn)
    {
        return -1;
    }
    if (out_field_name)
    {
        *out_field_name = fn;
    }
    else
    {
        g_free(fn);
    }

    /* value */
    db_value_t *val = (db_value_t *)out_value;
    if (val)
    {
        if (payload_read_value(p, val) < 0)
        {
            if (out_field_name && *out_field_name)
            {
                g_free(*out_field_name);
                *out_field_name = NULL;
            }
            return -1;
        }
    }
    else
    {
        /* 跳过 value（读取并丢弃） */
        db_value_t tmp;
        if (payload_read_value(p, &tmp) < 0)
        {
            return -1;
        }
        db_value_free(&tmp);
    }

    p->fields_read++;
    return 1;
}

void cli_db_payload_cleanup(cli_db_payload_parser_t *p)
{
    if (!p)
    {
        return;
    }
    g_free(p->db_name);
    g_free(p->table_name);
    p->db_name = NULL;
    p->table_name = NULL;
}

// Dispatch command to target module via pub/sub (synchronous)
int cli_dispatch_to_module(cli_match_result_t *result, cli_session_t *session)
{
    if (!result || result->module_id == 0 || !session)
    {
        return ERRCODE_FAIL;
    }

    gboolean is_show_cmd = (result->show_template && result->show_template[0] != '\0');

    // 获取当前视图上下文
    uint32_t ctx_len = 0;
    const uint8_t *ctx_data = cli_context_get(session, &ctx_len);

    if (is_show_cmd)
    {
        clear_tables_for_template(result->show_template);
    }

    uint8_t *msg_data = NULL;
    uint32_t msg_len = 0;

    /* 新 DB table 载荷格式 */
    msg_data = pack_db_payload(result, ctx_data, ctx_len, &msg_len);

    // 创建 CLI 消息
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

        if (is_show_cmd)
        {
            clear_tables_for_template(result->show_template);
        }
        return ERRCODE_SUCCESS;
    }

    // 使用同步查询等待响应，支持批量传输
    printf("[dispatch] Sending query to module 0x%08X...\n", result->module_id);

    GString *full_output = g_string_new("");
    cli_view_node_t *view = NULL;
    int done = 0;

    while (!done)
    {
        ipc_message_t *response = ipc_query(g_cfg_local->ipc_ctx, result->module_id, msg, 5000);

        // 释放查询消息（原始或 continue）
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

                // 提取上下文 TLV（prompt 之后的剩余数据）
                if (response->payload_len > CLI_CLI_MAX_PROMPT_LEN)
                {
                    uint32_t ctx_len = response->payload_len - CLI_CLI_MAX_PROMPT_LEN;
                    const uint8_t *ctx_data = (const uint8_t *)response->payload + CLI_CLI_MAX_PROMPT_LEN;
                    cli_context_set(session, ctx_data, ctx_len);
                    printf("[dispatch] Saved view context (%u bytes)\n", ctx_len);
                }
            }

            ipc_message_free(response);
            done = 1;
        }
        else if (response->msg_type == CFG_MSG_TYPE_CLI_RESP)
        {
            // 最终响应块
            if (response->payload)
            {
                g_string_append(full_output, response->payload);
            }
            ipc_message_free(response);
            done = 1;
        }
        else if (response->msg_type == CFG_MSG_TYPE_CLI_RESP_MORE)
        {
            // 部分响应 - 追加并请求更多
            if (response->payload)
            {
                g_string_append(full_output, response->payload);
            }
            ipc_message_free(response);

            // 发送 CONTINUE 请求下一批
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

    // Send accumulated output through pager
    if (is_show_cmd)
    {
        if (full_output->len > 0)
        {
            cli_pager_output(session, full_output->str);
        }
        else
        {
            char *rendered = cfg_template_renderer_render_by_name(result->show_template);
            if (rendered && rendered[0] != '\0')
            {
                cli_pager_output(session, rendered);
            }
            else
            {
                cfg_send_message(session, "No data.\r\n");
            }
            g_free(rendered);
        }

        clear_tables_for_template(result->show_template);
    }
    else if (full_output->len > 0)
    {
        cli_pager_output(session, full_output->str);
    }
    g_string_free(full_output, TRUE);

    return ERRCODE_SUCCESS;
}
