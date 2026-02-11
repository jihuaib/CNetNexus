/**
 * @file   db_ipc_handler.c
 * @brief  数据库模块 IPC 消息处理器实现
 * @author jhb
 * @date   2026/02/02
 *
 * 接收来自其他进程的 DB RPC 消息，反序列化参数后调用本地
 * db_api.c 函数执行操作，将结果序列化后发送响应。
 */

#include "db_ipc_handler.h"

#include <arpa/inet.h>
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

/**
 * @brief 发送 DB RPC 响应
 */
static void send_db_response(ipc_context_t *ctx, ipc_message_t *req_msg, int32_t retval, const db_result_t *result)
{
    void *resp_data = NULL;
    uint32_t resp_len = 0;

    db_serialize_response(retval, result, &resp_data, &resp_len);

    ipc_message_t *resp = ipc_message_create(IPC_MSG_TYPE_DB_RESP, DEV_MODULE_ID_DB, req_msg->src_module_id,
                                             req_msg->request_id, resp_data, resp_len, g_free);
    if (resp)
    {
        ipc_send_response(ctx, resp);
        ipc_message_free(resp);
    }
    else
    {
        g_free(resp_data);
    }
}

/**
 * @brief 处理 DB INSERT 请求
 */
static void handle_db_insert(ipc_context_t *ctx, ipc_message_t *msg)
{
    char *db_name = NULL;
    char *table_name = NULL;
    char **field_names = NULL;
    db_value_t *values = NULL;
    uint32_t num_fields = 0;

    if (db_deserialize_request_insert(msg->payload, msg->payload_len, &db_name, &table_name, &field_names, &values,
                                      &num_fields) < 0)
    {
        fprintf(stderr, "[db] Failed to deserialize INSERT request\n");
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        return;
    }

    int ret = db_insert(db_name, table_name, (const char **)field_names, values, num_fields);
    send_db_response(ctx, msg, ret, NULL);

    /* 清理 */
    g_free(db_name);
    g_free(table_name);
    if (field_names)
    {
        for (uint32_t i = 0; i < num_fields; i++)
        {
            g_free(field_names[i]);
        }
        g_free(field_names);
    }
    if (values)
    {
        for (uint32_t i = 0; i < num_fields; i++)
        {
            db_value_free(&values[i]);
        }
        g_free(values);
    }
}

/**
 * @brief 处理 DB UPDATE 请求
 */
static void handle_db_update(ipc_context_t *ctx, ipc_message_t *msg)
{
    char *db_name = NULL;
    char *table_name = NULL;
    char **field_names = NULL;
    db_value_t *values = NULL;
    uint32_t num_fields = 0;
    char *where_clause = NULL;

    if (db_deserialize_request_update(msg->payload, msg->payload_len, &db_name, &table_name, &field_names, &values,
                                      &num_fields, &where_clause) < 0)
    {
        fprintf(stderr, "[db] Failed to deserialize UPDATE request\n");
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        return;
    }

    int ret = db_update(db_name, table_name, (const char **)field_names, values, num_fields, where_clause);
    send_db_response(ctx, msg, ret, NULL);

    g_free(db_name);
    g_free(table_name);
    g_free(where_clause);
    if (field_names)
    {
        for (uint32_t i = 0; i < num_fields; i++)
        {
            g_free(field_names[i]);
        }
        g_free(field_names);
    }
    if (values)
    {
        for (uint32_t i = 0; i < num_fields; i++)
        {
            db_value_free(&values[i]);
        }
        g_free(values);
    }
}

/**
 * @brief 处理 DB DELETE 请求
 */
static void handle_db_delete(ipc_context_t *ctx, ipc_message_t *msg)
{
    char *db_name = NULL;
    char *table_name = NULL;
    char *where_clause = NULL;

    if (db_deserialize_request_delete(msg->payload, msg->payload_len, &db_name, &table_name, &where_clause) < 0)
    {
        fprintf(stderr, "[db] Failed to deserialize DELETE request\n");
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        return;
    }

    int ret = db_delete(db_name, table_name, where_clause);
    send_db_response(ctx, msg, ret, NULL);

    g_free(db_name);
    g_free(table_name);
    g_free(where_clause);
}

/**
 * @brief 处理 DB QUERY 请求
 */
static void handle_db_query(ipc_context_t *ctx, ipc_message_t *msg)
{
    char *db_name = NULL;
    char *table_name = NULL;
    char **field_names = NULL;
    uint32_t num_fields = 0;
    char *where_clause = NULL;

    if (db_deserialize_request_query(msg->payload, msg->payload_len, &db_name, &table_name, &field_names, &num_fields,
                                     &where_clause) < 0)
    {
        fprintf(stderr, "[db] Failed to deserialize QUERY request\n");
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        return;
    }

    db_result_t *result = NULL;
    int ret = db_query(db_name, table_name, (const char **)field_names, num_fields, where_clause, &result);
    send_db_response(ctx, msg, ret, result);

    if (result)
    {
        db_result_free(result);
    }
    g_free(db_name);
    g_free(table_name);
    g_free(where_clause);
    if (field_names)
    {
        for (uint32_t i = 0; i < num_fields; i++)
        {
            g_free(field_names[i]);
        }
        g_free(field_names);
    }
}

/**
 * @brief 处理 DB EXISTS 请求
 */
static void handle_db_exists(ipc_context_t *ctx, ipc_message_t *msg)
{
    char *db_name = NULL;
    char *table_name = NULL;
    char *where_clause = NULL;

    if (db_deserialize_request_exists(msg->payload, msg->payload_len, &db_name, &table_name, &where_clause) < 0)
    {
        fprintf(stderr, "[db] Failed to deserialize EXISTS request\n");
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        return;
    }

    gboolean exists = FALSE;
    int ret = db_exists(db_name, table_name, where_clause, &exists);

    if (ret == ERRCODE_SUCCESS)
    {
        send_db_response(ctx, msg, exists ? 1 : 0, NULL);
    }
    else
    {
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
    }

    g_free(db_name);
    g_free(table_name);
    g_free(where_clause);
}

/**
 * @brief 从payload中读取字符串
 */
static char *read_string(const uint8_t **data, uint32_t *remaining)
{
    if (*remaining < sizeof(uint16_t))
    {
        return NULL;
    }

    uint16_t len_be;
    memcpy(&len_be, *data, sizeof(len_be));
    uint16_t len = ntohs(len_be);
    *data += sizeof(len_be);
    *remaining -= sizeof(len_be);

    if (*remaining < len)
    {
        return NULL;
    }

    char *str = g_malloc(len + 1);
    memcpy(str, *data, len);
    str[len] = '\0';
    *data += len;
    *remaining -= len;

    return str;
}

/**
 * @brief 处理DB registry注册请求（只创建数据库，不创建表）
 *
 * payload 格式: [db_name_len(2B)] [db_name] [module_id(4B)]
 */
void handle_db_registry_add(ipc_context_t *ctx, ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len == 0)
    {
        fprintf(stderr, "[db_ipc] 无效的注册请求\n");
        return;
    }

    const uint8_t *data = (const uint8_t *)msg->payload;
    uint32_t remaining = msg->payload_len;

    // 解析db_name
    char *db_name = read_string(&data, &remaining);
    if (!db_name)
    {
        fprintf(stderr, "[db_ipc] 读取 db_name 失败\n");
        return;
    }

    // 解析module_id
    if (remaining < sizeof(uint32_t))
    {
        g_free(db_name);
        fprintf(stderr, "[db_ipc] 读取 module_id 失败\n");
        return;
    }
    uint32_t module_id_be;
    memcpy(&module_id_be, data, sizeof(module_id_be));
    uint32_t module_id = ntohl(module_id_be);

    printf("[db_ipc] 注册数据库: %s (module_id=%u)\n", db_name, module_id);

    // 只创建DB定义（不创建表），表由各模块自行创建
    db_definition_t *db_def = db_definition_create(db_name, module_id);
    if (!db_def)
    {
        g_free(db_name);
        fprintf(stderr, "[db_ipc] 创建数据库定义失败\n");
        return;
    }

    // 注册到registry
    db_registry_add(db_def);

    // 初始化数据库连接（创建 SQLite 文件和连接）
    if (db_initialize_database(db_def) != ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[db_ipc] 初始化数据库连接失败: %s\n", db_name);
    }
    else
    {
        printf("[db_ipc] 数据库连接初始化成功: %s\n", db_name);
    }

    g_free(db_name);

    printf("[db_ipc] 数据库注册成功\n");

    // 发送响应
    ipc_message_t *resp =
        ipc_message_create(IPC_MSG_TYPE_DB_RESP, DEV_MODULE_ID_DB, msg->src_module_id, msg->request_id, NULL, 0, NULL);

    if (resp)
    {
        ipc_send_response(ctx, resp);
        ipc_message_free(resp);
    }
}

void db_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    if (!ctx || !msg)
    {
        return;
    }

    switch (msg->msg_type)
    {
        case IPC_MSG_TYPE_DB_INSERT:
            handle_db_insert(ctx, msg);
            break;
        case IPC_MSG_TYPE_DB_UPDATE:
            handle_db_update(ctx, msg);
            break;
        case IPC_MSG_TYPE_DB_DELETE:
            handle_db_delete(ctx, msg);
            break;
        case IPC_MSG_TYPE_DB_QUERY:
            handle_db_query(ctx, msg);
            break;
        case IPC_MSG_TYPE_DB_EXISTS:
            handle_db_exists(ctx, msg);
            break;
        case IPC_MSG_TYPE_DB_REGISTRY_ADD:
            handle_db_registry_add(ctx, msg);
            break;
        default:
            fprintf(stderr, "[db] Unknown IPC message type: 0x%08X\n", msg->msg_type);
            break;
    }

    ipc_message_free(msg);
}
