/**
 * @file   db_client.c
 * @brief  数据库客户端代理实现，通过 IPC RPC 远程调用 DB 进程
 * @author jhb
 * @date   2026/02/02
 *
 * 实现 db.h 中的 CRUD API，内部将操作序列化后通过 ipc_query()
 * 同步发送到 DB 进程，等待结果后反序列化返回给调用者。
 */

#include <stdio.h>
#include <string.h>

#include "db.h"
#include "db_serialize.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

/** DB RPC 超时时间（毫秒） */
#define DB_CLIENT_TIMEOUT_MS 5000

/* 全局 IPC 上下文，由使用方在进程启动时设置 */
static ipc_context_t *g_db_client_ipc = NULL;

/**
 * @brief 设置 DB 客户端使用的 IPC 上下文
 * @param ctx IPC 上下文
 */
void db_client_set_ipc(ipc_context_t *ctx)
{
    g_db_client_ipc = ctx;
}

/**
 * @brief 获取 DB 客户端的 IPC 上下文
 * @return IPC 上下文，未设置返回 NULL
 */
ipc_context_t *db_client_get_ipc(void)
{
    return g_db_client_ipc;
}

/**
 * @brief 内部辅助：发送 DB RPC 请求并获取响应的返回值
 * @param msg_type IPC 消息类型
 * @param data 序列化数据
 * @param data_len 数据长度
 * @param out_retval 输出返回值
 * @param out_result 输出结果集（可为 NULL）
 * @return 成功返回 0，失败返回 -1
 */
static int db_client_rpc(uint32_t msg_type, void *data, uint32_t data_len, int32_t *out_retval,
                          db_result_t **out_result)
{
    if (!g_db_client_ipc)
    {
        fprintf(stderr, "[db_client] IPC 上下文未初始化\n");
        if (data)
        {
            g_free(data);
        }
        return ERRCODE_FAIL;
    }

    /* 创建请求消息 */
    ipc_message_t *req = ipc_message_create(msg_type, ipc_get_module_id(g_db_client_ipc),
                                                   DEV_MODULE_ID_DB, 0, data, data_len, g_free);
    if (!req)
    {
        g_free(data);
        return ERRCODE_FAIL;
    }

    /* 同步查询 DB 进程 */
    ipc_message_t *resp = ipc_query(g_db_client_ipc, DEV_MODULE_ID_DB, req, DB_CLIENT_TIMEOUT_MS);
    ipc_message_free(req);

    if (!resp)
    {
        fprintf(stderr, "[db_client] DB RPC 超时或失败 (msg_type=0x%08X)\n", msg_type);
        return ERRCODE_FAIL;
    }

    /* 反序列化响应 */
    int ret = db_deserialize_response(resp->payload, (uint32_t)resp->payload_len, out_retval, out_result);
    ipc_message_free(resp);

    return ret;
}

// ============================================================================
// db.h CRUD API 实现（客户端代理）
// ============================================================================

int db_insert(const char *db_name, const char *table_name, const char **field_names, const db_value_t *values,
                 uint32_t num_fields)
{
    if (!db_name || !table_name || !field_names || !values || num_fields == 0)
    {
        return ERRCODE_FAIL;
    }

    void *data = NULL;
    uint32_t data_len = 0;

    int ret = db_serialize_insert(db_name, table_name, field_names, values, num_fields, &data, &data_len);
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int32_t retval = 0;
    ret = db_client_rpc(IPC_MSG_TYPE_DB_INSERT, data, data_len, &retval, NULL);
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    return retval;
}

int db_update(const char *db_name, const char *table_name, const char **field_names, const db_value_t *values,
                 uint32_t num_fields, const char *where_clause)
{
    if (!db_name || !table_name || !field_names || !values || num_fields == 0)
    {
        return -1;
    }

    void *data = NULL;
    uint32_t data_len = 0;

    int ret = db_serialize_update(db_name, table_name, field_names, values, num_fields, where_clause, &data,
                                     &data_len);
    if (ret != ERRCODE_SUCCESS)
    {
        return -1;
    }

    int32_t retval = 0;
    ret = db_client_rpc(IPC_MSG_TYPE_DB_UPDATE, data, data_len, &retval, NULL);
    if (ret != ERRCODE_SUCCESS)
    {
        return -1;
    }

    return retval;
}

int db_delete(const char *db_name, const char *table_name, const char *where_clause)
{
    if (!db_name || !table_name)
    {
        return -1;
    }

    void *data = NULL;
    uint32_t data_len = 0;

    int ret = db_serialize_delete(db_name, table_name, where_clause, &data, &data_len);
    if (ret != ERRCODE_SUCCESS)
    {
        return -1;
    }

    int32_t retval = 0;
    ret = db_client_rpc(IPC_MSG_TYPE_DB_DELETE, data, data_len, &retval, NULL);
    if (ret != ERRCODE_SUCCESS)
    {
        return -1;
    }

    return retval;
}

int db_query(const char *db_name, const char *table_name, const char **field_names, uint32_t num_fields,
                const char *where_clause, db_result_t **result)
{
    if (!db_name || !table_name || !result)
    {
        return ERRCODE_FAIL;
    }

    void *data = NULL;
    uint32_t data_len = 0;

    int ret = db_serialize_query(db_name, table_name, field_names, num_fields, where_clause, &data, &data_len);
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int32_t retval = 0;
    ret = db_client_rpc(IPC_MSG_TYPE_DB_QUERY, data, data_len, &retval, result);
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    return retval;
}

int db_exists(const char *db_name, const char *table_name, const char *where_clause, gboolean *exists)
{
    if (!db_name || !table_name || !exists)
    {
        return ERRCODE_FAIL;
    }

    void *data = NULL;
    uint32_t data_len = 0;

    int ret = db_serialize_exists(db_name, table_name, where_clause, &data, &data_len);
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    int32_t retval = 0;
    ret = db_client_rpc(IPC_MSG_TYPE_DB_EXISTS, data, data_len, &retval, NULL);
    if (ret != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }

    *exists = (retval != 0) ? TRUE : FALSE;
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 值辅助函数（客户端本地实现，无需 RPC）
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
