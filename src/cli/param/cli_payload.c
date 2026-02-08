/**
 * @file   cfg_payload.c
 * @brief  DB table 载荷序列化/反序列化实现
 * @author jhb
 * @date   2026/02/04
 *
 * 新载荷格式（DB table 结构）:
 *   [flags: 1 字节]
 *     bit0: is_no_cmd (1 = "no" 前缀命令)
 *   [db_name: uint16 长度 + 字符串]
 *   [table_name: uint16 长度 + 字符串]
 *   [num_fields: uint16]
 *   对每个字段:
 *     [field_flags: 1 字节]
 *       bit0: is_context (1 = 上下文字段)
 *     [field_name: uint16 长度 + 字符串]
 *     [value_type: 1 字节]  (DB_TYPE_*)
 *     [value: 类型相关]
 *       NULL: 无数据
 *       INTEGER: 8 字节 int64 网络字节序
 *       TEXT: uint16 长度 + 字符串
 *       REAL: 8 字节 double
 */

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "db.h"

/* ========================================================================= */
/* 读取辅助函数                                                              */
/* ========================================================================= */

typedef struct
{
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
} payload_reader_t;

static int payload_read_u8(payload_reader_t *r, uint8_t *out)
{
    if (r->pos + 1 > r->len)
    {
        return -1;
    }
    *out = r->data[r->pos++];
    return 0;
}

static int payload_read_u16(payload_reader_t *r, uint16_t *out)
{
    if (r->pos + 2 > r->len)
    {
        return -1;
    }
    uint16_t be;
    memcpy(&be, r->data + r->pos, 2);
    *out = ntohs(be);
    r->pos += 2;
    return 0;
}

static int payload_read_i64(payload_reader_t *r, int64_t *out)
{
    if (r->pos + 8 > r->len)
    {
        return -1;
    }
    uint32_t hi_be, lo_be;
    memcpy(&hi_be, r->data + r->pos, 4);
    memcpy(&lo_be, r->data + r->pos + 4, 4);
    uint32_t hi = ntohl(hi_be);
    uint32_t lo = ntohl(lo_be);
    *out = ((int64_t)hi << 32) | lo;
    r->pos += 8;
    return 0;
}

static char *payload_read_string(payload_reader_t *r)
{
    uint16_t len;
    if (payload_read_u16(r, &len) < 0)
    {
        return NULL;
    }
    if (len == 0xFFFF)
    {
        return NULL; /* NULL 字符串 */
    }
    if (r->pos + len > r->len)
    {
        return NULL;
    }
    char *s = g_malloc(len + 1);
    memcpy(s, r->data + r->pos, len);
    s[len] = '\0';
    r->pos += len;
    return s;
}

static int payload_read_value(payload_reader_t *r, db_value_t *val)
{
    uint8_t type;
    if (payload_read_u8(r, &type) < 0)
    {
        return -1;
    }
    val->type = (db_value_type_t)type;

    switch (val->type)
    {
        case DB_TYPE_NULL:
            break;
        case DB_TYPE_INTEGER:
            if (payload_read_i64(r, &val->data.i64) < 0)
            {
                return -1;
            }
            break;
        case DB_TYPE_REAL:
            if (r->pos + sizeof(double) > r->len)
            {
                return -1;
            }
            memcpy(&val->data.real, r->data + r->pos, sizeof(double));
            r->pos += sizeof(double);
            break;
        case DB_TYPE_TEXT:
            val->data.text = payload_read_string(r);
            break;
        default:
            return -1;
    }
    return 0;
}

/* ========================================================================= */
/* 载荷解析器公共 API                                                        */
/* ========================================================================= */

int cfg_db_payload_init(cfg_db_payload_parser_t *p, const uint8_t *data, uint32_t len)
{
    if (!p || !data || len < 1)
    {
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->_reader_data = data;
    p->_reader_len = len;
    p->_reader_pos = 0;

    payload_reader_t r = {.data = data, .len = len, .pos = 0};

    /* 读取 flags */
    if (payload_read_u8(&r, &p->flags) < 0)
    {
        return -1;
    }

    /* 读取 db_name */
    p->db_name = payload_read_string(&r);
    if (!p->db_name)
    {
        return -1;
    }

    /* 读取 table_name */
    p->table_name = payload_read_string(&r);
    if (!p->table_name)
    {
        g_free(p->db_name);
        p->db_name = NULL;
        return -1;
    }

    /* 读取 num_fields */
    if (payload_read_u16(&r, &p->num_fields) < 0)
    {
        g_free(p->db_name);
        g_free(p->table_name);
        p->db_name = NULL;
        p->table_name = NULL;
        return -1;
    }

    p->fields_read = 0;
    /* 保存当前读取位置，后续 next 从此处继续 */
    p->_reader_pos = r.pos;

    return 0;
}

int cfg_db_payload_next(cfg_db_payload_parser_t *p, uint8_t *out_flags, char **out_field_name,
                           void *out_value_ptr)
{
    db_value_t *out_value = (db_value_t *)out_value_ptr;
    if (!p || !out_flags || !out_field_name || !out_value)
    {
        return -1;
    }

    if (p->fields_read >= p->num_fields)
    {
        return 0; /* 无更多字段 */
    }

    payload_reader_t r = {.data = p->_reader_data, .len = p->_reader_len, .pos = p->_reader_pos};

    /* 读取 field_flags */
    if (payload_read_u8(&r, out_flags) < 0)
    {
        return -1;
    }

    /* 读取 field_name */
    *out_field_name = payload_read_string(&r);
    if (!*out_field_name)
    {
        return -1;
    }

    /* 读取 value */
    if (payload_read_value(&r, out_value) < 0)
    {
        g_free(*out_field_name);
        *out_field_name = NULL;
        return -1;
    }

    p->_reader_pos = r.pos;
    p->fields_read++;

    return 1; /* 成功读取一个字段 */
}

void cfg_db_payload_cleanup(cfg_db_payload_parser_t *p)
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
