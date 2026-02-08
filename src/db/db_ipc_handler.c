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
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "db.h"
#include "db_cli.h"
#include "db_serialize.h"
#include "errcode.h"
#include "cfg.h"

/* 读取辅助结构 */
typedef struct read_ctx
{
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
} read_ctx_t;

static int ctx_read_u16(read_ctx_t *r, uint16_t *out)
{
    if (r->pos + 2 > r->len)
    {
        return -1;
    }
    uint16_t be;
    memcpy(&be, r->data + r->pos, 2);
    *out = ntohs(be);
    r->pos += 2;
    return 0;
}

static int ctx_read_u32(read_ctx_t *r, uint32_t *out)
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

static char *ctx_read_string(read_ctx_t *r)
{
    uint16_t len;
    if (ctx_read_u16(r, &len) < 0)
    {
        return NULL;
    }
    if (len == 0xFFFF)
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

static int ctx_read_u8(read_ctx_t *r, uint8_t *out)
{
    if (r->pos + 1 > r->len)
    {
        return -1;
    }
    *out = r->data[r->pos++];
    return 0;
}

static int ctx_read_i64(read_ctx_t *r, int64_t *out)
{
    uint32_t hi, lo;
    if (ctx_read_u32(r, &hi) < 0 || ctx_read_u32(r, &lo) < 0)
    {
        return -1;
    }
    *out = ((int64_t)hi << 32) | lo;
    return 0;
}

static int ctx_read_value(read_ctx_t *r, db_value_t *val)
{
    uint8_t type;
    if (ctx_read_u8(r, &type) < 0)
    {
        return -1;
    }
    val->type = (db_value_type_t)type;

    switch (val->type)
    {
        case DB_TYPE_NULL:
            break;
        case DB_TYPE_INTEGER:
            if (ctx_read_i64(r, &val->data.i64) < 0)
            {
                return -1;
            }
            break;
        case DB_TYPE_REAL:
            if (r->pos + sizeof(double) > r->len)
            {
                return -1;
            }
            memcpy(&val->data.real, r->data + r->pos, sizeof(double));
            r->pos += sizeof(double);
            break;
        case DB_TYPE_TEXT:
            val->data.text = ctx_read_string(r);
            break;
        case DB_TYPE_BLOB:
        {
            uint32_t blob_len;
            if (ctx_read_u32(r, &blob_len) < 0)
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

/**
 * @brief 发送 DB RPC 响应
 */
static void send_db_response(ipc_context_t *ctx, ipc_message_t *req_msg, int32_t retval,
                              const db_result_t *result)
{
    void *resp_data = NULL;
    uint32_t resp_len = 0;

    db_serialize_response(retval, result, &resp_data, &resp_len);

    ipc_message_t *resp =
        ipc_message_create(IPC_MSG_TYPE_DB_RESP, DEV_MODULE_ID_DB, req_msg->src_module_id, req_msg->request_id,
                              resp_data, resp_len, g_free);
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
    read_ctx_t r = {.data = msg->payload, .len = (uint32_t)msg->payload_len, .pos = 0};

    char *db_name = ctx_read_string(&r);
    char *table_name = ctx_read_string(&r);
    uint32_t num_fields = 0;
    ctx_read_u32(&r, &num_fields);

    if (!db_name || !table_name || num_fields == 0)
    {
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        g_free(db_name);
        g_free(table_name);
        return;
    }

    char **field_names = g_malloc0(num_fields * sizeof(char *));
    db_value_t *values = g_malloc0(num_fields * sizeof(db_value_t));

    for (uint32_t i = 0; i < num_fields; i++)
    {
        field_names[i] = ctx_read_string(&r);
    }
    for (uint32_t i = 0; i < num_fields; i++)
    {
        ctx_read_value(&r, &values[i]);
    }

    int ret = db_insert(db_name, table_name, (const char **)field_names, values, num_fields);
    send_db_response(ctx, msg, ret, NULL);

    /* 清理 */
    for (uint32_t i = 0; i < num_fields; i++)
    {
        g_free(field_names[i]);
        db_value_free(&values[i]);
    }
    g_free(field_names);
    g_free(values);
    g_free(db_name);
    g_free(table_name);
}

/**
 * @brief 处理 DB UPDATE 请求
 */
static void handle_db_update(ipc_context_t *ctx, ipc_message_t *msg)
{
    read_ctx_t r = {.data = msg->payload, .len = (uint32_t)msg->payload_len, .pos = 0};

    char *db_name = ctx_read_string(&r);
    char *table_name = ctx_read_string(&r);
    uint32_t num_fields = 0;
    ctx_read_u32(&r, &num_fields);

    if (!db_name || !table_name || num_fields == 0)
    {
        send_db_response(ctx, msg, -1, NULL);
        g_free(db_name);
        g_free(table_name);
        return;
    }

    char **field_names = g_malloc0(num_fields * sizeof(char *));
    db_value_t *values = g_malloc0(num_fields * sizeof(db_value_t));

    for (uint32_t i = 0; i < num_fields; i++)
    {
        field_names[i] = ctx_read_string(&r);
    }
    for (uint32_t i = 0; i < num_fields; i++)
    {
        ctx_read_value(&r, &values[i]);
    }

    char *where_clause = ctx_read_string(&r);

    int ret = db_update(db_name, table_name, (const char **)field_names, values, num_fields, where_clause);
    send_db_response(ctx, msg, ret, NULL);

    for (uint32_t i = 0; i < num_fields; i++)
    {
        g_free(field_names[i]);
        db_value_free(&values[i]);
    }
    g_free(field_names);
    g_free(values);
    g_free(db_name);
    g_free(table_name);
    g_free(where_clause);
}

/**
 * @brief 处理 DB DELETE 请求
 */
static void handle_db_delete(ipc_context_t *ctx, ipc_message_t *msg)
{
    read_ctx_t r = {.data = msg->payload, .len = (uint32_t)msg->payload_len, .pos = 0};

    char *db_name = ctx_read_string(&r);
    char *table_name = ctx_read_string(&r);
    char *where_clause = ctx_read_string(&r);

    if (!db_name || !table_name)
    {
        send_db_response(ctx, msg, -1, NULL);
        g_free(db_name);
        g_free(table_name);
        g_free(where_clause);
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
    read_ctx_t r = {.data = msg->payload, .len = (uint32_t)msg->payload_len, .pos = 0};

    char *db_name = ctx_read_string(&r);
    char *table_name = ctx_read_string(&r);
    uint32_t num_fields = 0;
    ctx_read_u32(&r, &num_fields);

    if (!db_name || !table_name)
    {
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        g_free(db_name);
        g_free(table_name);
        return;
    }

    char **field_names = NULL;
    if (num_fields > 0)
    {
        field_names = g_malloc0(num_fields * sizeof(char *));
        for (uint32_t i = 0; i < num_fields; i++)
        {
            field_names[i] = ctx_read_string(&r);
        }
    }

    char *where_clause = ctx_read_string(&r);

    db_result_t *result = NULL;
    int ret = db_query(db_name, table_name, (const char **)field_names, num_fields, where_clause, &result);
    send_db_response(ctx, msg, ret, result);

    if (result)
    {
        db_result_free(result);
    }
    if (field_names)
    {
        for (uint32_t i = 0; i < num_fields; i++)
        {
            g_free(field_names[i]);
        }
        g_free(field_names);
    }
    g_free(db_name);
    g_free(table_name);
    g_free(where_clause);
}

/**
 * @brief 处理 DB EXISTS 请求
 */
static void handle_db_exists(ipc_context_t *ctx, ipc_message_t *msg)
{
    read_ctx_t r = {.data = msg->payload, .len = (uint32_t)msg->payload_len, .pos = 0};

    char *db_name = ctx_read_string(&r);
    char *table_name = ctx_read_string(&r);
    char *where_clause = ctx_read_string(&r);

    if (!db_name || !table_name)
    {
        send_db_response(ctx, msg, ERRCODE_FAIL, NULL);
        g_free(db_name);
        g_free(table_name);
        g_free(where_clause);
        return;
    }

    gboolean exists = FALSE;
    int ret = db_exists(db_name, table_name, where_clause, &exists);

    if (ret == ERRCODE_SUCCESS)
    {
        /* 用 retval 编码 exists 结果：1 = 存在，0 = 不存在 */
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

// Forward declarations for registry handlers
void handle_db_registry_add(ipc_context_t *ctx, ipc_message_t *msg);
void handle_db_init_all(ipc_context_t *ctx, ipc_message_t *msg);

void db_ipc_msg_handler(ipc_context_t *ctx, ipc_message_t *msg)
{
    if (!ctx || !msg)
    {
        return;
    }

    uint32_t msg_category = IPC_MSG_CATEGORY(msg->msg_type);
    if (msg_category == IPC_CATEGORY_CFG)
    {
        uint32_t msg_subtype = IPC_MSG_SUBTYPE(msg->msg_type);
        switch (msg_subtype)
        {
            case 0x0001: // CFG_MSG_TYPE_CLI
                db_cli_process_command(msg);
                break;
            case 0x0005: // CFG_MSG_TYPE_CLI_CONTINUE
                db_cli_handle_continue(msg);
                break;
            default:
                fprintf(stderr, "[db] Unknown CFG message subtype: 0x%04X\n", msg_subtype);
                break;
        }

        ipc_message_free(msg);
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
        case IPC_MSG_TYPE_DB_INIT_ALL:
            handle_db_init_all(ctx, msg);
            break;
        default:
            fprintf(stderr, "[db] 未知 IPC 消息类型: 0x%08X\n", msg->msg_type);
            break;
    }

    ipc_message_free(msg);
}
