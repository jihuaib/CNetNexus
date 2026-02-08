/**
 * @file   db_serialize.c
 * @brief  数据库操作参数序列化/反序列化实现
 * @author jhb
 * @date   2026/02/02
 *
 * 二进制格式：
 *   字符串: [uint16 len][bytes...]   (len=0xFFFF 表示 NULL)
 *   uint32: [4 bytes, network order]
 *   int32:  [4 bytes, network order]
 *   value:  [uint8 type][type-specific data]
 *     NULL:    type=0
 *     INTEGER: type=1 [int64 network order]
 *     REAL:    type=2 [double, 8 bytes]
 *     TEXT:    type=3 [uint16 len][bytes]
 */

#include "db_serialize.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "errcode.h"

/* 写入辅助函数 */
static void write_u8(GByteArray *buf, uint8_t v)
{
    g_byte_array_append(buf, &v, 1);
}

static void write_u16(GByteArray *buf, uint16_t v)
{
    uint16_t be = htons(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 2);
}

static void write_u32(GByteArray *buf, uint32_t v)
{
    uint32_t be = htonl(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 4);
}

static void write_i32(GByteArray *buf, int32_t v)
{
    uint32_t be = htonl((uint32_t)v);
    g_byte_array_append(buf, (const uint8_t *)&be, 4);
}

static void write_i64(GByteArray *buf, int64_t v)
{
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    g_byte_array_append(buf, (const uint8_t *)&hi, 4);
    g_byte_array_append(buf, (const uint8_t *)&lo, 4);
}

static void write_string(GByteArray *buf, const char *s)
{
    if (!s)
    {
        write_u16(buf, 0xFFFF);
        return;
    }
    uint16_t len = (uint16_t)strlen(s);
    write_u16(buf, len);
    if (len > 0)
    {
        g_byte_array_append(buf, (const uint8_t *)s, len);
    }
}

static void write_value(GByteArray *buf, const db_value_t *val)
{
    write_u8(buf, (uint8_t)val->type);
    switch (val->type)
    {
        case DB_TYPE_NULL:
            break;
        case DB_TYPE_INTEGER:
            write_i64(buf, val->data.i64);
            break;
        case DB_TYPE_REAL:
        {
            double d = val->data.real;
            g_byte_array_append(buf, (const uint8_t *)&d, sizeof(double));
            break;
        }
        case DB_TYPE_TEXT:
            write_string(buf, val->data.text);
            break;
        case DB_TYPE_BLOB:
            write_u32(buf, (uint32_t)val->data.blob.len);
            if (val->data.blob.len > 0 && val->data.blob.data)
            {
                g_byte_array_append(buf, val->data.blob.data, val->data.blob.len);
            }
            break;
    }
}

/* 读取辅助函数 */
typedef struct read_ctx
{
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
} read_ctx_t;

static int read_u8(read_ctx_t *r, uint8_t *out)
{
    if (r->pos + 1 > r->len)
    {
        return -1;
    }
    *out = r->data[r->pos++];
    return 0;
}

static int read_u16(read_ctx_t *r, uint16_t *out)
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

static int read_u32(read_ctx_t *r, uint32_t *out)
{
    if (r->pos + 4 > r->len)
    {
        return -1;
    }
    uint32_t be;
    memcpy(&be, r->data + r->pos, 4);
    *out = ntohl(be);
    r->pos += 4;
    return 0;
}

static int read_i32(read_ctx_t *r, int32_t *out)
{
    uint32_t v;
    if (read_u32(r, &v) < 0)
    {
        return -1;
    }
    *out = (int32_t)v;
    return 0;
}

static int read_i64(read_ctx_t *r, int64_t *out)
{
    uint32_t hi, lo;
    if (read_u32(r, &hi) < 0 || read_u32(r, &lo) < 0)
    {
        return -1;
    }
    *out = ((int64_t)hi << 32) | lo;
    return 0;
}

static char *read_string(read_ctx_t *r)
{
    uint16_t len;
    if (read_u16(r, &len) < 0)
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

static int read_value(read_ctx_t *r, db_value_t *val)
{
    uint8_t type;
    if (read_u8(r, &type) < 0)
    {
        return -1;
    }
    val->type = (db_value_type_t)type;

    switch (val->type)
    {
        case DB_TYPE_NULL:
            break;
        case DB_TYPE_INTEGER:
            if (read_i64(r, &val->data.i64) < 0)
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
            val->data.text = read_string(r);
            break;
        case DB_TYPE_BLOB:
        {
            uint32_t blob_len;
            if (read_u32(r, &blob_len) < 0)
            {
                return -1;
            }
            if (r->pos + blob_len > r->len)
            {
                return -1;
            }
            val->data.blob.data = g_memdup2(r->data + r->pos, blob_len);
            val->data.blob.len = blob_len;
            r->pos += blob_len;
            break;
        }
    }
    return 0;
}

/* 通用格式: [db_name][table_name][num_fields][field_names...][values...][where_clause] */

int db_serialize_insert(const char *db_name, const char *table_name, const char **field_names,
                            const db_value_t *values, uint32_t num_fields, void **out_data, uint32_t *out_len)
{
    GByteArray *buf = g_byte_array_new();
    write_string(buf, db_name);
    write_string(buf, table_name);
    write_u32(buf, num_fields);

    for (uint32_t i = 0; i < num_fields; i++)
    {
        write_string(buf, field_names[i]);
    }
    for (uint32_t i = 0; i < num_fields; i++)
    {
        write_value(buf, &values[i]);
    }

    *out_len = buf->len;
    *out_data = g_byte_array_free(buf, FALSE);
    return ERRCODE_SUCCESS;
}

int db_serialize_update(const char *db_name, const char *table_name, const char **field_names,
                            const db_value_t *values, uint32_t num_fields, const char *where_clause, void **out_data,
                            uint32_t *out_len)
{
    GByteArray *buf = g_byte_array_new();
    write_string(buf, db_name);
    write_string(buf, table_name);
    write_u32(buf, num_fields);

    for (uint32_t i = 0; i < num_fields; i++)
    {
        write_string(buf, field_names[i]);
    }
    for (uint32_t i = 0; i < num_fields; i++)
    {
        write_value(buf, &values[i]);
    }
    write_string(buf, where_clause);

    *out_len = buf->len;
    *out_data = g_byte_array_free(buf, FALSE);
    return ERRCODE_SUCCESS;
}

int db_serialize_delete(const char *db_name, const char *table_name, const char *where_clause, void **out_data,
                            uint32_t *out_len)
{
    GByteArray *buf = g_byte_array_new();
    write_string(buf, db_name);
    write_string(buf, table_name);
    write_string(buf, where_clause);

    *out_len = buf->len;
    *out_data = g_byte_array_free(buf, FALSE);
    return ERRCODE_SUCCESS;
}

int db_serialize_query(const char *db_name, const char *table_name, const char **field_names, uint32_t num_fields,
                           const char *where_clause, void **out_data, uint32_t *out_len)
{
    GByteArray *buf = g_byte_array_new();
    write_string(buf, db_name);
    write_string(buf, table_name);
    write_u32(buf, num_fields);

    for (uint32_t i = 0; i < num_fields; i++)
    {
        write_string(buf, field_names[i]);
    }
    write_string(buf, where_clause);

    *out_len = buf->len;
    *out_data = g_byte_array_free(buf, FALSE);
    return ERRCODE_SUCCESS;
}

int db_serialize_exists(const char *db_name, const char *table_name, const char *where_clause, void **out_data,
                            uint32_t *out_len)
{
    GByteArray *buf = g_byte_array_new();
    write_string(buf, db_name);
    write_string(buf, table_name);
    write_string(buf, where_clause);

    *out_len = buf->len;
    *out_data = g_byte_array_free(buf, FALSE);
    return ERRCODE_SUCCESS;
}

int db_serialize_response(int32_t retval, const db_result_t *result, void **out_data, uint32_t *out_len)
{
    GByteArray *buf = g_byte_array_new();
    write_i32(buf, retval);

    if (result && result->num_rows > 0)
    {
        write_u32(buf, result->num_rows);
        /* 字段数量取第一行 */
        uint32_t num_fields = result->rows[0]->num_fields;
        write_u32(buf, num_fields);

        /* 写入字段名（只写一次） */
        for (uint32_t j = 0; j < num_fields; j++)
        {
            write_string(buf, result->rows[0]->field_names[j]);
        }

        /* 写入所有行的值 */
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            for (uint32_t j = 0; j < num_fields; j++)
            {
                write_value(buf, &result->rows[i]->values[j]);
            }
        }
    }
    else
    {
        write_u32(buf, 0); /* num_rows = 0 */
    }

    *out_len = buf->len;
    *out_data = g_byte_array_free(buf, FALSE);
    return ERRCODE_SUCCESS;
}

int db_deserialize_response(const void *data, uint32_t len, int32_t *out_retval, db_result_t **out_result)
{
    if (!data || len < 4 || !out_retval)
    {
        return ERRCODE_FAIL;
    }

    read_ctx_t r = {.data = data, .len = len, .pos = 0};

    if (read_i32(&r, out_retval) < 0)
    {
        return ERRCODE_FAIL;
    }

    if (out_result)
    {
        uint32_t num_rows;
        if (read_u32(&r, &num_rows) < 0)
        {
            return ERRCODE_FAIL;
        }

        if (num_rows == 0)
        {
            /* 空结果集 */
            db_result_t *res = g_malloc0(sizeof(db_result_t));
            res->rows = NULL;
            res->num_rows = 0;
            *out_result = res;
            return ERRCODE_SUCCESS;
        }

        uint32_t num_fields;
        if (read_u32(&r, &num_fields) < 0)
        {
            return ERRCODE_FAIL;
        }

        /* 读取字段名 */
        char **field_names = g_malloc0(num_fields * sizeof(char *));
        for (uint32_t j = 0; j < num_fields; j++)
        {
            field_names[j] = read_string(&r);
        }

        /* 读取行 */
        db_result_t *res = g_malloc0(sizeof(db_result_t));
        res->num_rows = num_rows;
        res->rows_capacity = num_rows;
        res->rows = g_malloc0(num_rows * sizeof(db_row_t *));

        for (uint32_t i = 0; i < num_rows; i++)
        {
            db_row_t *row = g_malloc0(sizeof(db_row_t));
            row->num_fields = num_fields;
            row->field_names = g_malloc0(num_fields * sizeof(char *));
            row->values = g_malloc0(num_fields * sizeof(db_value_t));

            for (uint32_t j = 0; j < num_fields; j++)
            {
                row->field_names[j] = g_strdup(field_names[j]);
                if (read_value(&r, &row->values[j]) < 0)
                {
                    /* 清理 */
                    for (uint32_t k = 0; k <= j; k++)
                    {
                        g_free(row->field_names[k]);
                    }
                    g_free(row->field_names);
                    g_free(row->values);
                    g_free(row);
                    for (uint32_t k = 0; k < i; k++)
                    {
                        /* 简化清理 */
                    }
                    g_free(res->rows);
                    g_free(res);
                    for (uint32_t k = 0; k < num_fields; k++)
                    {
                        g_free(field_names[k]);
                    }
                    g_free(field_names);
                    return ERRCODE_FAIL;
                }
            }

            res->rows[i] = row;
        }

        for (uint32_t j = 0; j < num_fields; j++)
        {
            g_free(field_names[j]);
        }
        g_free(field_names);

        *out_result = res;
    }

    return ERRCODE_SUCCESS;
}
