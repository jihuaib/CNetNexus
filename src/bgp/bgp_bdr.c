/**
 * @file   bgp_bdr.c
 * @brief  BGP 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/04
 */
#include "bgp_bdr.h"

#include <string.h>

#include "bgp_db.h"
#include "bgp_main.h"
#include "cli.h"
#include "dev.h"
#include "log.h"

/** 最多支持的 AFI 种类数 */
#define BGP_BDR_MAX_AFI 8

// ============================================================================
// 内部辅助
// ============================================================================

/**
 * @brief 向请求方发送配置文本响应
 * @param msg  原始请求消息
 * @param text 配置文本（空字符串表示该模块无配置）
 */
static void send_config_resp(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_bgp_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 各表配置追加函数
// ============================================================================

/**
 * @brief 追加协议头配置行（bgp <as> / bgp router-id）
 * @return TRUE 表示协议已配置，FALSE 表示无配置
 */
static gboolean bdr_append_protocol(dev_ipc_context_t *ctx, char *buf, size_t buf_size, size_t *off)
{
    db_result_t *result = NULL;
    if (bgp_db_query(ctx, &result) != 0 || !result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return FALSE;
    }

    uint32_t as_number = 0;
    const char *router_id = NULL;
    db_row_t *row = result->rows[0];

    for (uint32_t j = 0; j < row->num_fields; j++)
    {
        if (strcmp(row->field_names[j], "as_number") == 0 && row->values[j].type == DB_TYPE_INTEGER)
        {
            as_number = (uint32_t)row->values[j].data.i64;
        }
        else if (strcmp(row->field_names[j], "router_id") == 0 && row->values[j].type == DB_TYPE_TEXT)
        {
            router_id = row->values[j].data.text;
        }
    }

    CLI_BUF_APPEND(buf, buf_size, *off, "!\r\n");
    CLI_BUF_APPEND(buf, buf_size, *off, "bgp %u\r\n", as_number);
    if (router_id && strcmp(router_id, "0.0.0.0") != 0)
    {
        CLI_BUF_APPEND(buf, buf_size, *off, " bgp router-id %s\r\n", router_id);
    }

    db_result_free(result);
    return TRUE;
}

/**
 * @brief 追加会话配置行（neighbor <ip> as <as>）
 */
static void bdr_append_sessions(dev_ipc_context_t *ctx, char *buf, size_t buf_size, size_t *off)
{
    db_result_t *result = NULL;
    if (bgp_db_query_sessions(ctx, &result) != 0 || !result)
    {
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ip = NULL;
        int64_t remote_as = 0;

        for (uint32_t j = 0; j < row->num_fields; j++)
        {
            if (strcmp(row->field_names[j], "neighbor_ip") == 0 && row->values[j].type == DB_TYPE_TEXT)
            {
                ip = row->values[j].data.text;
            }
            else if (strcmp(row->field_names[j], "remote_as") == 0 && row->values[j].type == DB_TYPE_INTEGER)
            {
                remote_as = row->values[j].data.i64;
            }
        }

        if (ip)
        {
            CLI_BUF_APPEND(buf, buf_size, *off, " neighbor %s as %ld\r\n", ip, remote_as);
        }
    }

    db_result_free(result);
}

/**
 * @brief 追加地址族配置块（af <afi> / neighbor <ip> enable / !）
 */
static void bdr_append_af_neighbors(dev_ipc_context_t *ctx, char *buf, size_t buf_size, size_t *off)
{
    db_result_t *result = NULL;
    if (bgp_db_query_neighbors(ctx, &result) != 0 || !result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return;
    }

    /* 收集所有唯一的 AFI */
    char afi_list[BGP_BDR_MAX_AFI][64];
    uint32_t afi_count = 0;

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *afi = NULL;
        for (uint32_t j = 0; j < row->num_fields; j++)
        {
            if (strcmp(row->field_names[j], "afi") == 0 && row->values[j].type == DB_TYPE_TEXT)
            {
                afi = row->values[j].data.text;
                break;
            }
        }
        if (!afi)
        {
            continue;
        }

        gboolean found = FALSE;
        for (uint32_t k = 0; k < afi_count; k++)
        {
            if (strcmp(afi_list[k], afi) == 0)
            {
                found = TRUE;
                break;
            }
        }
        if (!found && afi_count < BGP_BDR_MAX_AFI)
        {
            snprintf(afi_list[afi_count++], sizeof(afi_list[0]), "%s", afi);
        }
    }

    /* 逐 AFI 输出地址族块 */
    for (uint32_t k = 0; k < afi_count; k++)
    {
        CLI_BUF_APPEND(buf, buf_size, *off, " af %s\r\n", afi_list[k]);
        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            db_row_t *row = result->rows[i];
            const char *ip = NULL;
            const char *afi = NULL;
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                if (strcmp(row->field_names[j], "neighbor_ip") == 0 && row->values[j].type == DB_TYPE_TEXT)
                {
                    ip = row->values[j].data.text;
                }
                else if (strcmp(row->field_names[j], "afi") == 0 && row->values[j].type == DB_TYPE_TEXT)
                {
                    afi = row->values[j].data.text;
                }
            }
            if (ip && afi && strcmp(afi, afi_list[k]) == 0)
            {
                CLI_BUF_APPEND(buf, buf_size, *off, "  neighbor %s enable\r\n", ip);
            }
        }
        CLI_BUF_APPEND(buf, buf_size, *off, " !\r\n");
    }

    db_result_free(result);
}

// ============================================================================
// 公共 API
// ============================================================================

void bgp_bdr_show_config(dev_ipc_message_t *msg)
{
    dev_ipc_context_t *ctx = g_bgp_local->dev_ipc_ctx;
    char buf[CLI_MAX_RESP_LEN];
    size_t off = 0;

    if (!bdr_append_protocol(ctx, buf, sizeof(buf), &off))
    {
        send_config_resp(msg, "");
        return;
    }

    bdr_append_sessions(ctx, buf, sizeof(buf), &off);
    bdr_append_af_neighbors(ctx, buf, sizeof(buf), &off);
    CLI_BUF_APPEND(buf, sizeof(buf), off, "!\r\n");

    send_config_resp(msg, buf);
}
