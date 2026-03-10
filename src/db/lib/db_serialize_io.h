/**
 * @file   db_serialize_io.h
 * @brief  序列化/反序列化内部读写帮助函数（仅供 db_serialize*.c 包含）
 * @author jhb
 * @date   2026/03/10
 */
#ifndef DB_SERIALIZE_IO_H
#define DB_SERIALIZE_IO_H

#include <arpa/inet.h>
#include <glib.h>
#include <string.h>

#include "db.h"

// ============================================================================
// 写入辅助
// ============================================================================

typedef struct
{
    uint8_t *data;
    uint32_t len;
    uint32_t capacity;
} write_ctx_t;

static inline void w_init(write_ctx_t *w)
{
    w->len = 0;
    w->capacity = 1024;
    w->data = g_malloc(w->capacity);
}

static inline void w_ensure(write_ctx_t *w, uint32_t size)
{
    while (w->len + size > w->capacity)
    {
        w->capacity *= 2;
    }
    w->data = g_realloc(w->data, w->capacity);
}

static inline void w_u32(write_ctx_t *w, uint32_t v)
{
    w_ensure(w, 4);
    uint32_t be = htonl(v);
    memcpy(w->data + w->len, &be, 4);
    w->len += 4;
}

static inline void w_i64(write_ctx_t *w, int64_t v)
{
    w_ensure(w, 8);
    uint32_t hi = htonl((uint32_t)((uint64_t)v >> 32));
    uint32_t lo = htonl((uint32_t)((uint64_t)v & 0xFFFFFFFF));
    memcpy(w->data + w->len, &hi, 4);
    memcpy(w->data + w->len + 4, &lo, 4);
    w->len += 8;
}

static inline void w_double(write_ctx_t *w, double v)
{
    w_ensure(w, sizeof(double));
    memcpy(w->data + w->len, &v, sizeof(double));
    w->len += sizeof(double);
}

static inline void w_string(write_ctx_t *w, const char *s)
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

static inline void w_blob(write_ctx_t *w, const void *data, uint32_t len)
{
    w_u32(w, len);
    if (len > 0)
    {
        w_ensure(w, len);
        memcpy(w->data + w->len, data, len);
        w->len += len;
    }
}

static inline void w_value(write_ctx_t *w, const db_value_t *val)
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
// 读取辅助
// ============================================================================

typedef struct
{
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
} read_ctx_t;

static inline int r_u32(read_ctx_t *r, uint32_t *out)
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

static inline int r_i64(read_ctx_t *r, int64_t *out)
{
    uint32_t hi, lo;
    if (r_u32(r, &hi) < 0 || r_u32(r, &lo) < 0)
    {
        return -1;
    }
    *out = ((int64_t)hi << 32) | lo;
    return 0;
}

static inline int r_double(read_ctx_t *r, double *out)
{
    if (r->pos + sizeof(double) > r->len)
    {
        return -1;
    }
    memcpy(out, r->data + r->pos, sizeof(double));
    r->pos += sizeof(double);
    return 0;
}

static inline char *r_string(read_ctx_t *r)
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

static inline int r_value(read_ctx_t *r, db_value_t *val)
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
            if (r_u32(r, &blob_len) < 0 || r->pos + blob_len > r->len)
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

#endif /* DB_SERIALIZE_IO_H */
