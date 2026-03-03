/**
 * @file   db_ipc_handler.c
 * @brief  数据库模块 IPC 消息处理器实现
 * @author jhb
 * @date   2026/02/02
 *
 * 接收来自其他模块的 SQL 字符串，直接调用本地 db_exec_sql / db_query_sql 执行，
 * 将结果序列化后发送响应。
 */

#include "db_ipc_handler.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "db_api.h"
#include "db_cli.h"
#include "db_main.h"
#include "db_serialize.h"
#include "errcode.h"
#include "log.h"

/**
 * @brief 发送 DB RPC 响应
 */
static void send_db_response(dev_ipc_context_t *ctx, dev_ipc_message_t *req_msg, int32_t retval,
                             const db_result_t *result)
{
    void *resp_data = NULL;
    uint32_t resp_len = 0;

    db_serialize_response(retval, result, &resp_data, &resp_len);

    dev_ipc_message_t *resp = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DB_RESP, DEV_MODULE_ID_DB, req_msg->src_module_id,
                                                     req_msg->request_id, resp_data, resp_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(ctx, resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(resp_data);
    }
}

/**
 * @brief 处理 DB_EXEC_SQL 请求（INSERT / UPDATE / DELETE / DDL）
 *
 * payload: db_name + sql 字符串，服务端直接执行，返回影响行数。
 */
static void handle_db_exec_sql(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    char *db_name = NULL;
    char *sql = NULL;

    if (db_deserialize_request_sql(msg->payload, msg->payload_len, &db_name, &sql) < 0)
    {
        LOG_ERROR("反序列化 EXEC_SQL 请求失败");
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        return;
    }

    int rows = db_exec_sql(db_name, sql);
    send_db_response(ctx, msg, rows, NULL);

    g_free(db_name);
    g_free(sql);
}

/**
 * @brief 处理 DB_QUERY_SQL 请求（SELECT）
 *
 * payload: db_name + sql 字符串，服务端直接执行并返回结果集。
 */
static void handle_db_query_sql(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    char *db_name = NULL;
    char *sql = NULL;

    if (db_deserialize_request_sql(msg->payload, msg->payload_len, &db_name, &sql) < 0)
    {
        LOG_ERROR("反序列化 QUERY_SQL 请求失败");
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        return;
    }

    db_result_t *result = NULL;
    int ret = db_query_sql(db_name, sql, &result);
    send_db_response(ctx, msg, ret, result);

    if (result)
    {
        db_result_free(result);
    }
    g_free(db_name);
    g_free(sql);
}

void db_ipc_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!ctx || !msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case DEV_IPC_MSG_TYPE_DB_EXEC_SQL:
            handle_db_exec_sql(ctx, msg);
            break;
        case DEV_IPC_MSG_TYPE_DB_QUERY_SQL:
            handle_db_query_sql(ctx, msg);
            break;
        default:
            LOG_WARN("Unknown IPC message type: 0x%08X", msg->msg_type);
            break;
    }

    dev_ipc_message_free(msg);
}
