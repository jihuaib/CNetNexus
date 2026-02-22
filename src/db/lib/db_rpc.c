/**
 * @file   db_rpc.c
 * @brief  数据库 RPC 接口实现，供其他模块通过 IPC 调用 DB 模块
 * @author jhb
 * @date   2026/02/11
 */
#define LOG_TAG "db"
#include "db_rpc.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db_serialize.h"
#include "errcode.h"
#include "log.h"

// ============================================================================
// 数据库创建 API
// ============================================================================

int db_rpc_create_db(ipc_context_t *ctx, const char *db_name, uint32_t module_id)
{
    if (!ctx || !db_name)
    {
        return ERRCODE_FAIL;
    }

    // 等待与 DB 模块的连接建立（异步握手可能尚未完成）
    for (int i = 0; i < 100; i++) // 最多等待 5 秒
    {
        if (ipc_is_connected(ctx, DEV_MODULE_ID_DB))
        {
            break;
        }
        usleep(50000); // 50ms
    }
    if (!ipc_is_connected(ctx, DEV_MODULE_ID_DB))
    {
        LOG_ERROR("错误: 无法连接到 DB 模块");
        return ERRCODE_FAIL;
    }

    // 序列化 payload: [db_name_len(2B)] [db_name] [module_id(4B)]
    uint16_t name_len = (uint16_t)strlen(db_name);
    uint32_t payload_len = sizeof(uint16_t) + name_len + sizeof(uint32_t);
    uint8_t *payload = g_malloc(payload_len);
    uint8_t *ptr = payload;

    uint16_t name_len_be = htons(name_len);
    memcpy(ptr, &name_len_be, sizeof(name_len_be));
    ptr += sizeof(name_len_be);

    memcpy(ptr, db_name, name_len);
    ptr += name_len;

    uint32_t module_id_be = htonl(module_id);
    memcpy(ptr, &module_id_be, sizeof(module_id_be));

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_REGISTRY_ADD, ipc_get_module_id(ctx), DEV_MODULE_ID_DB, 0,
                                            payload, payload_len, g_free);

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC 创建数据库失败: 无响应 (db=%s)", db_name);
        return ERRCODE_FAIL;
    }

    ipc_message_free(resp);
    LOG_INFO("数据库创建成功: %s", db_name);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// RPC CRUD 操作
// ============================================================================

int db_rpc_insert(ipc_context_t *ctx, const char *db_name, const char *table_name, const char **field_names,
                  const db_value_t *values, uint32_t num_fields)
{
    if (!ctx || !db_name || !table_name || !field_names || !values || num_fields == 0)
    {
        return ERRCODE_FAIL;
    }

    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_insert(db_name, table_name, field_names, values, num_fields, &payload, &payload_len);

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_INSERT, ipc_get_module_id(ctx), DEV_MODULE_ID_DB, 0,
                                            payload, payload_len, g_free);

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC 插入失败: 无响应");
        return ERRCODE_FAIL;
    }

    int32_t retval = ERRCODE_FAIL;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
    ipc_message_free(resp);
    return retval;
}

int db_rpc_update(ipc_context_t *ctx, const char *db_name, const char *table_name, const char **field_names,
                  const db_value_t *values, uint32_t num_fields, const char *where_clause)
{
    if (!ctx || !db_name || !table_name || !field_names || !values || num_fields == 0)
    {
        return -1;
    }

    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_update(db_name, table_name, field_names, values, num_fields, where_clause, &payload,
                                &payload_len);

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_UPDATE, ipc_get_module_id(ctx), DEV_MODULE_ID_DB, 0,
                                            payload, payload_len, g_free);

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC 更新失败: 无响应");
        return -1;
    }

    int32_t retval = -1;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
    ipc_message_free(resp);
    return retval;
}

int db_rpc_delete(ipc_context_t *ctx, const char *db_name, const char *table_name, const char *where_clause)
{
    if (!ctx || !db_name || !table_name)
    {
        return -1;
    }

    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_delete(db_name, table_name, where_clause, &payload, &payload_len);

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_DELETE, ipc_get_module_id(ctx), DEV_MODULE_ID_DB, 0,
                                            payload, payload_len, g_free);

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC 删除失败: 无响应");
        return -1;
    }

    int32_t retval = -1;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
    ipc_message_free(resp);
    return retval;
}

int db_rpc_query(ipc_context_t *ctx, const char *db_name, const char *table_name, const char **field_names,
                 uint32_t num_fields, const char *where_clause, db_result_t **result)
{
    if (!ctx || !db_name || !table_name || !result)
    {
        return ERRCODE_FAIL;
    }

    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_query(db_name, table_name, field_names, num_fields, where_clause, &payload, &payload_len);

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_QUERY, ipc_get_module_id(ctx), DEV_MODULE_ID_DB, 0, payload,
                                            payload_len, g_free);

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    ipc_message_free(req);

    if (!resp)
    {
        LOG_ERROR("RPC 查询失败: 无响应");
        return ERRCODE_FAIL;
    }

    int32_t retval = ERRCODE_FAIL;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, result);
    ipc_message_free(resp);
    return retval;
}

int db_rpc_exists(ipc_context_t *ctx, const char *db_name, const char *table_name, const char *where_clause,
                  gboolean *exists)
{
    if (!ctx || !db_name || !table_name || !exists)
    {
        return ERRCODE_FAIL;
    }

    void *payload = NULL;
    uint32_t payload_len = 0;
    db_serialize_request_exists(db_name, table_name, where_clause, &payload, &payload_len);

    ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DB_EXISTS, ipc_get_module_id(ctx), DEV_MODULE_ID_DB, 0,
                                            payload, payload_len, g_free);

    ipc_message_t *resp = ipc_query(ctx, DEV_MODULE_ID_DB, req, 5000);
    ipc_message_free(req);

    if (!resp)
    {
        return ERRCODE_FAIL;
    }

    int32_t retval = 0;
    db_deserialize_response(resp->payload, resp->payload_len, &retval, NULL);
    ipc_message_free(resp);

    if (retval == ERRCODE_FAIL)
    {
        return ERRCODE_FAIL;
    }

    *exists = (retval != 0);
    return ERRCODE_SUCCESS;
}
