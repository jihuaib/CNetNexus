/**
 * @file   db_ipc_registry_handler.c
 * @brief  DB 模块 Registry IPC 消息处理
 * @author jhb
 * @date   2026/02/06
 */
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "db.h"
#include "db_main.h"
#include "db_registry.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

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
 * @brief 处理DB registry注册请求
 */
void handle_db_registry_add(ipc_context_t *ctx, ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len == 0)
    {
        fprintf(stderr, "[db_ipc] Invalid registry add request\n");
        return;
    }

    const uint8_t *data = (const uint8_t *)msg->payload;
    uint32_t remaining = msg->payload_len;

    // 解析db_name
    char *db_name = read_string(&data, &remaining);
    if (!db_name)
    {
        fprintf(stderr, "[db_ipc] Failed to read db_name\n");
        return;
    }

    // 解析module_id
    if (remaining < sizeof(uint32_t))
    {
        g_free(db_name);
        fprintf(stderr, "[db_ipc] Failed to read module_id\n");
        return;
    }
    uint32_t module_id_be;
    memcpy(&module_id_be, data, sizeof(module_id_be));
    uint32_t module_id = ntohl(module_id_be);
    data += sizeof(module_id_be);
    remaining -= sizeof(module_id_be);

    printf("[db_ipc] Registering database: %s (module_id=%u)\n", db_name, module_id);

    // 创建DB定义
    db_definition_t *db_def = db_definition_create(db_name, module_id);
    if (!db_def)
    {
        g_free(db_name);
        fprintf(stderr, "[db_ipc] Failed to create db_definition\n");
        return;
    }

    // 解析num_tables
    if (remaining < sizeof(uint16_t))
    {
        g_free(db_name);
        fprintf(stderr, "[db_ipc] Failed to read num_tables\n");
        return;
    }
    uint16_t num_tables_be;
    memcpy(&num_tables_be, data, sizeof(num_tables_be));
    uint16_t num_tables = ntohs(num_tables_be);
    data += sizeof(num_tables_be);
    remaining -= sizeof(num_tables_be);

    // 解析tables
    for (uint16_t i = 0; i < num_tables; i++)
    {
        // table_name
        char *table_name = read_string(&data, &remaining);
        if (!table_name)
        {
            g_free(db_name);
            fprintf(stderr, "[db_ipc] Failed to read table_name\n");
            return;
        }

        db_table_t *db_table = db_table_create(table_name);
        g_free(table_name);

        if (!db_table)
        {
            g_free(db_name);
            fprintf(stderr, "[db_ipc] Failed to create table\n");
            return;
        }

        // num_fields
        if (remaining < sizeof(uint16_t))
        {
            g_free(db_name);
            fprintf(stderr, "[db_ipc] Failed to read num_fields\n");
            return;
        }
        uint16_t num_fields_be;
        memcpy(&num_fields_be, data, sizeof(num_fields_be));
        uint16_t num_fields = ntohs(num_fields_be);
        data += sizeof(num_fields_be);
        remaining -= sizeof(num_fields_be);

        // fields
        for (uint16_t j = 0; j < num_fields; j++)
        {
            // field_name
            char *field_name = read_string(&data, &remaining);
            if (!field_name)
            {
                g_free(db_name);
                fprintf(stderr, "[db_ipc] Failed to read field_name\n");
                return;
            }

            // type_str
            char *type_str = read_string(&data, &remaining);
            if (!type_str)
            {
                g_free(field_name);
                g_free(db_name);
                fprintf(stderr, "[db_ipc] Failed to read type_str\n");
                return;
            }

            db_field_t *db_field = db_field_create(field_name, type_str);
            g_free(field_name);
            g_free(type_str);

            if (db_field)
            {
                db_table_add_field(db_table, db_field);
            }
        }

        db_definition_add_table(db_def, db_table);
    }

    // 注册到registry
    db_registry_add(db_def);
    g_free(db_name);

    printf("[db_ipc] Database registered successfully\n");

    // 发送响应
    ipc_message_t *resp = ipc_message_create(
        IPC_MSG_TYPE_DB_RESP,
        DEV_MODULE_ID_DB,
        msg->src_module_id,
        msg->request_id,
        NULL,
        0,
        NULL
    );

    if (resp)
    {
        ipc_send_response(ctx, resp);
        ipc_message_free(resp);
    }
}

/**
 * @brief 处理DB初始化所有数据库请求
 */
void handle_db_init_all(ipc_context_t *ctx, ipc_message_t *msg)
{
    printf("[db_ipc] Initializing all databases\n");

    int ret = db_initialize_all();

    // 发送响应
    ipc_message_t *resp = ipc_message_create(
        IPC_MSG_TYPE_DB_RESP,
        DEV_MODULE_ID_DB,
        msg->src_module_id,
        msg->request_id,
        NULL,
        0,
        NULL
    );

    if (resp)
    {
        ipc_send_response(ctx, resp);
        ipc_message_free(resp);
    }

    if (ret == ERRCODE_SUCCESS)
    {
        printf("[db_ipc] All databases initialized successfully\n");
    }
    else
    {
        fprintf(stderr, "[db_ipc] Database initialization had errors\n");
    }
}
