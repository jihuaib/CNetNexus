/**
 * @file   cli_tlv.c
 * @brief  CLI TLV 载荷解析工具函数
 * @author jhb
 * @date   2026/03/08
 */

#include <arpa/inet.h>
#include <glib.h>
#include <string.h>

#include "cli.h"
#include "db.h"

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

uint32_t cli_tlv_entry_get_ctx_uint32(const cli_tlv_entry_t *entry)
{
    if (!entry || entry->type != CLI_TLV_TYPE_CTX || entry->length != 4 || !entry->value)
    {
        return 0;
    }
    uint32_t v;
    memcpy(&v, entry->value, 4);
    return ntohl(v);
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

int cli_ctx_lookup_uint32(const uint8_t *ctx_data, uint32_t ctx_len, uint32_t ctx_id, uint32_t *out_value)
{
    if (!ctx_data || ctx_len < 2)
    {
        return -1;
    }

    uint16_t num_be;
    memcpy(&num_be, ctx_data, 2);
    uint16_t num = ntohs(num_be);
    uint32_t pos = 2;

    for (uint16_t i = 0; i < num && pos < ctx_len; i++)
    {
        if (pos + 7 > ctx_len)
        {
            return -1;
        }

        uint32_t id_be;
        memcpy(&id_be, ctx_data + pos, 4);
        uint32_t id = ntohl(id_be);
        pos += 4;

        uint8_t type = ctx_data[pos++];

        uint16_t len_be;
        memcpy(&len_be, ctx_data + pos, 2);
        uint16_t value_len = ntohs(len_be);
        pos += 2;

        if (pos + value_len > ctx_len)
        {
            return -1;
        }

        if (id == ctx_id && type == CLI_TLV_TYPE_CTX && value_len == 4)
        {
            uint32_t value_be;
            memcpy(&value_be, ctx_data + pos, 4);
            if (out_value)
            {
                *out_value = ntohl(value_be);
            }
            return 0;
        }

        pos += value_len;
    }

    return -1;
}

int cli_ctx_lookup_text(const uint8_t *ctx_data, uint32_t ctx_len, uint32_t ctx_id, char *out_buf, size_t out_cap)
{
    if (!ctx_data || ctx_len < 2 || !out_buf || out_cap == 0)
    {
        return -1;
    }

    uint16_t num_be;
    memcpy(&num_be, ctx_data, 2);
    uint16_t num = ntohs(num_be);
    uint32_t pos = 2;

    out_buf[0] = '\0';

    for (uint16_t i = 0; i < num && pos < ctx_len; i++)
    {
        if (pos + 7 > ctx_len)
        {
            return -1;
        }

        uint32_t id_be;
        memcpy(&id_be, ctx_data + pos, 4);
        uint32_t id = ntohl(id_be);
        pos += 4;

        uint8_t type = ctx_data[pos++];

        uint16_t len_be;
        memcpy(&len_be, ctx_data + pos, 2);
        uint16_t value_len = ntohs(len_be);
        pos += 2;

        if (pos + value_len > ctx_len)
        {
            return -1;
        }

        if (id == ctx_id && type == CLI_TLV_TYPE_CTX_STR)
        {
            gsize copy_len = MIN((gsize)value_len, out_cap - 1);
            if (copy_len > 0)
            {
                memcpy(out_buf, ctx_data + pos, copy_len);
            }
            out_buf[copy_len] = '\0';
            return 0;
        }

        pos += value_len;
    }

    return -1;
}

uint8_t *cli_show_scope_payload_build(const cli_show_scope_t *scope, uint32_t *out_len)
{
    if (out_len)
    {
        *out_len = 0;
    }

    if (!scope)
    {
        return NULL;
    }

    uint16_t view_len = 0;
    if (scope->view_name[0] != '\0')
    {
        view_len = (uint16_t)strlen(scope->view_name);
    }

    if (scope->mode == CLI_SHOW_SCOPE_MODE_FULL && view_len == 0 && (!scope->ctx_data || scope->ctx_len == 0))
    {
        return NULL;
    }

    gsize total_len = 1 + 2 + view_len + 4 + scope->ctx_len;
    GByteArray *buf = g_byte_array_sized_new((guint)total_len);
    if (!buf)
    {
        return NULL;
    }

    uint8_t mode = (uint8_t)scope->mode;
    uint16_t view_len_be = htons(view_len);
    uint32_t ctx_len_be = htonl(scope->ctx_len);

    g_byte_array_append(buf, &mode, 1);
    g_byte_array_append(buf, (const guint8 *)&view_len_be, sizeof(view_len_be));
    if (view_len > 0)
    {
        g_byte_array_append(buf, (const guint8 *)scope->view_name, view_len);
    }
    g_byte_array_append(buf, (const guint8 *)&ctx_len_be, sizeof(ctx_len_be));
    if (scope->ctx_data && scope->ctx_len > 0)
    {
        g_byte_array_append(buf, scope->ctx_data, scope->ctx_len);
    }

    if (out_len)
    {
        *out_len = buf->len;
    }
    return g_byte_array_free(buf, FALSE);
}

int cli_show_scope_payload_parse(const uint8_t *payload, uint32_t payload_len, cli_show_scope_t *scope_out)
{
    if (!scope_out)
    {
        return -1;
    }

    memset(scope_out, 0, sizeof(*scope_out));
    scope_out->mode = CLI_SHOW_SCOPE_MODE_FULL;

    if (!payload || payload_len == 0)
    {
        return 0;
    }

    if (payload_len < 7)
    {
        return -1;
    }

    uint32_t pos = 0;
    uint8_t mode = payload[pos++];
    if (mode != CLI_SHOW_SCOPE_MODE_FULL && mode != CLI_SHOW_SCOPE_MODE_THIS)
    {
        return -1;
    }
    scope_out->mode = (cli_show_scope_mode_t)mode;

    uint16_t view_len_be;
    memcpy(&view_len_be, payload + pos, sizeof(view_len_be));
    uint16_t view_len = ntohs(view_len_be);
    pos += sizeof(view_len_be);

    if (view_len >= CLI_MAX_VIEW_LEN || pos + view_len + 4 > payload_len)
    {
        return -1;
    }

    if (view_len > 0)
    {
        memcpy(scope_out->view_name, payload + pos, view_len);
        scope_out->view_name[view_len] = '\0';
        pos += view_len;
    }

    uint32_t ctx_len_be;
    memcpy(&ctx_len_be, payload + pos, sizeof(ctx_len_be));
    scope_out->ctx_len = ntohl(ctx_len_be);
    pos += sizeof(ctx_len_be);

    if (pos + scope_out->ctx_len != payload_len)
    {
        return -1;
    }

    if (scope_out->ctx_len > 0)
    {
        scope_out->ctx_data = payload + pos;
    }

    return 0;
}
