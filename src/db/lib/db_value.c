/**
 * @file   db_value.c
 * @brief  数据库值类型辅助函数（无 SQLite 依赖，供 db_api 共享库使用）
 * @author jhb
 * @date   2026/03/10
 */
#include <string.h>

#include "db.h"

// ============================================================================
// db_value_t 构造函数
// ============================================================================

db_value_t db_value_int(int64_t value)
{
    db_value_t v;
    v.type = DB_TYPE_INTEGER;
    v.data.i64 = value;
    return v;
}

db_value_t db_value_text(const char *value)
{
    db_value_t v;
    v.type = DB_TYPE_TEXT;
    v.data.text = value ? g_strdup(value) : NULL;
    return v;
}

db_value_t db_value_real(double value)
{
    db_value_t v;
    v.type = DB_TYPE_REAL;
    v.data.real = value;
    return v;
}

db_value_t db_value_null(void)
{
    db_value_t v;
    v.type = DB_TYPE_NULL;
    return v;
}

void db_value_free(db_value_t *value)
{
    if (!value)
    {
        return;
    }

    if (value->type == DB_TYPE_TEXT && value->data.text)
    {
        g_free(value->data.text);
        value->data.text = NULL;
    }
    else if (value->type == DB_TYPE_BLOB && value->data.blob.data)
    {
        g_free(value->data.blob.data);
        value->data.blob.data = NULL;
    }
}

// ============================================================================
// db_result_t 释放函数
// ============================================================================

void db_result_free(db_result_t *result)
{
    if (!result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        if (row)
        {
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                g_free(row->field_names[j]);
                db_value_free(&row->values[j]);
            }
            g_free(row->field_names);
            g_free(row->values);
            g_free(row);
        }
    }

    g_free(result->rows);
    g_free(result);
}
