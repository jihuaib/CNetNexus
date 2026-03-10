/**
 * @file   db_serialize.c
 * @brief  数据库客户端序列化实现：构造请求、解析响应（供 db_api 库使用）
 * @author jhb
 * @date   2026/03/10
 */
#include "db_serialize.h"

#include "db_serialize_io.h"

void db_serialize_request_sql(const char *db_name, const char *sql, void **out_data, uint32_t *out_len)
{
    write_ctx_t w;
    w_init(&w);
    w_string(&w, db_name);
    w_string(&w, sql);
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

            /* 读列名（取第一行） */
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
