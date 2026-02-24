/**
 * @file   db_serialize.c
 * @brief  数据库序列化/反序列化实现
 * @author jhb
 * @date   2026/02/10
 */

#include "db_serialize.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// 写入辅助函数
// ============================================================================

typedef struct
{
    uint8_t *data;
    uint32_t len;
    uint32_t capacity;
} write_ctx_t;

static void w_init(write_ctx_t *w)
{
    w->len = 0;
    w->capacity = 1024;
    w->data = g_malloc(w->capacity);
}

static void w_ensure(write_ctx_t *w, uint32_t size)
{
    if (w->len + size > w->capacity)
    {
        while (w->len + size > w->capacity)
        {
            w->capacity *= 2;
        }
        w->data = g_realloc(w->data, w->capacity);
    }
}

static void w_u32(write_ctx_t *w, uint32_t v)
{
    w_ensure(w, 4);
    uint32_t be = htonl(v);
    memcpy(w->data + w->len, &be, 4);
    w->len += 4;
}

static void w_i64(write_ctx_t *w, int64_t v)
{
    w_ensure(w, 8);
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    memcpy(w->data + w->len, &hi, 4);
    memcpy(w->data + w->len + 4, &lo, 4);
    w->len += 8;
}

static void w_double(write_ctx_t *w, double v)
{
    w_ensure(w, sizeof(double));
    memcpy(w->data + w->len, &v, sizeof(double));
    w->len += sizeof(double);
}

static void w_string(write_ctx_t *w, const char *s)
{
    if (!s)
    {
        w_u32(w, 0xFFFFFFFF);
        return;
    }
    uint32_t len = (uint32_t)strlen(s);
    w_u32(w, len);
    w_ensure(w, len);
    memcpy(w->data + w->len, s, len);
    w->len += len;
}

static void w_blob(write_ctx_t *w, const void *data, uint32_t len)
{
    w_u32(w, len);
    if (len > 0)
    {
        w_ensure(w, len);
        memcpy(w->data + w->len, data, len);
        w->len += len;
    }
}

static void w_value(write_ctx_t *w, const db_value_t *val)
{
    w_u32(w, (uint32_t)val->type);
    switch (val->type)
    {
        case DB_TYPE_NULL:
            break;
        case DB_TYPE_INTEGER:
            w_i64(w, val->data.i64);
            break;
        case DB_TYPE_REAL:
            w_double(w, val->data.real);
            break;
        case DB_TYPE_TEXT:
            w_string(w, val->data.text);
            break;
        case DB_TYPE_BLOB:
            w_blob(w, val->data.blob.data, val->data.blob.len);
            break;
    }
}

// ============================================================================
// 读取辅助函数
// ============================================================================

typedef struct
{
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
} read_ctx_t;

static int r_u32(read_ctx_t *r, uint32_t *out)
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

static int r_i64(read_ctx_t *r, int64_t *out)
{
    uint32_t hi, lo;
    if (r_u32(r, &hi) < 0 || r_u32(r, &lo) < 0)
    {
        return -1;
    }
    *out = ((int64_t)hi << 32) | lo;
    return 0;
}

static int r_double(read_ctx_t *r, double *out)
{
    if (r->pos + sizeof(double) > r->len)
    {
        return -1;
    }
    memcpy(out, r->data + r->pos, sizeof(double));
    r->pos += sizeof(double);
    return 0;
}

static char *r_string(read_ctx_t *r)
{
    uint32_t len;
    if (r_u32(r, &len) < 0)
    {
        return NULL;
    }
    if (len == 0xFFFFFFFF)
    {
        return NULL;
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

static int r_value(read_ctx_t *r, db_value_t *val)
{
    uint32_t type;
    if (r_u32(r, &type) < 0)
    {
        return -1;
    }
    val->type = (db_value_type_t)type;

    switch (val->type)
    {
        case DB_TYPE_NULL:
            break;
        case DB_TYPE_INTEGER:
            if (r_i64(r, &val->data.i64) < 0)
            {
                return -1;
            }
            break;
        case DB_TYPE_REAL:
            if (r_double(r, &val->data.real) < 0)
            {
                return -1;
            }
            break;
        case DB_TYPE_TEXT:
            val->data.text = r_string(r);
            break;
        case DB_TYPE_BLOB:
        {
            uint32_t blob_len;
            if (r_u32(r, &blob_len) < 0)
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

// ============================================================================
// 请求序列化/反序列化（统一 SQL 字符串模型）
// ============================================================================

void db_serialize_request_sql(const char *db_name, const char *sql, void **out_data, uint32_t *out_len)
{
    write_ctx_t w;
    w_init(&w);
    w_string(&w, db_name);
    w_string(&w, sql);
    *out_data = w.data;
    *out_len = w.len;
}

int db_deserialize_request_sql(const void *data, uint32_t len, char **db_name, char **sql)
{
    read_ctx_t r = {.data = data, .len = len, .pos = 0};
    *db_name = r_string(&r);
    *sql = r_string(&r);
    return 0;
}

// ============================================================================
// 响应序列化/反序列化
// ============================================================================

void db_serialize_response(int32_t retval, const db_result_t *result, void **out_data, uint32_t *out_len)
{
    write_ctx_t w;
    w_init(&w);
    w_u32(&w, (uint32_t)retval);

    if (result && result->num_rows > 0 && result->rows[0])
    {
        uint32_t num_cols = result->rows[0]->num_fields;
        w_u32(&w, result->num_rows);
        w_u32(&w, num_cols);

        /* 写列名（取第一行） */
        for (uint32_t j = 0; j < num_cols; j++)
        {
            w_string(&w, result->rows[0]->field_names[j]);
        }

        /* 写所有行数据 */
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            for (uint32_t j = 0; j < num_cols; j++)
            {
                w_value(&w, &result->rows[i]->values[j]);
            }
        }
    }
    else
    {
        w_u32(&w, 0); /* num_rows */
        w_u32(&w, 0); /* num_cols */
    }

    *out_data = w.data;
    *out_len = w.len;
}

int db_deserialize_response(const void *data, uint32_t len, int32_t *retval, db_result_t **result)
{
    read_ctx_t r = {.data = data, .len = len, .pos = 0};

    uint32_t ret_u32;
    if (r_u32(&r, &ret_u32) < 0)
    {
        return -1;
    }
    *retval = (int32_t)ret_u32;

    uint32_t num_rows, num_cols;
    if (r_u32(&r, &num_rows) < 0 || r_u32(&r, &num_cols) < 0)
    {
        return -1;
    }

    if (result)
    {
        if (num_rows == 0 || num_cols == 0)
        {
            *result = NULL;
        }
        else
        {
            *result = g_malloc0(sizeof(db_result_t));
            (*result)->num_rows = num_rows;
            (*result)->rows = g_malloc0(num_rows * sizeof(db_row_t *));

            /* 读列名 */
            char **cols = g_malloc0(num_cols * sizeof(char *));
            for (uint32_t j = 0; j < num_cols; j++)
            {
                cols[j] = r_string(&r);
            }

            /* 读行数据 */
            for (uint32_t i = 0; i < num_rows; i++)
            {
                db_row_t *row = g_malloc0(sizeof(db_row_t));
                row->num_fields = num_cols;
                row->field_names = g_malloc0(num_cols * sizeof(char *));
                row->values = g_malloc0(num_cols * sizeof(db_value_t));

                for (uint32_t j = 0; j < num_cols; j++)
                {
                    row->field_names[j] = g_strdup(cols[j]);
                    r_value(&r, &row->values[j]);
                }
                (*result)->rows[i] = row;
            }

            for (uint32_t j = 0; j < num_cols; j++)
            {
                g_free(cols[j]);
            }
            g_free(cols);
        }
    }

    return 0;
}
