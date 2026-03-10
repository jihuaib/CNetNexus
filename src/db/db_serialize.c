/**
 * @file   db_serialize.c
 * @brief  数据库服务端序列化实现：解析请求、构造响应（供 netnexus-db 使用）
 * @author jhb
 * @date   2026/02/10
 */
#include "db_serialize.h"

#include "lib/db_serialize_io.h"

int db_deserialize_request_sql(const void *data, uint32_t len, char **db_name, char **sql)
{
    read_ctx_t r = {.data = data, .len = len, .pos = 0};
    *db_name = r_string(&r);
    *sql = r_string(&r);
    return 0;
}

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
