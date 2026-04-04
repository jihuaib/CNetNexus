/**
 * @file   route_bdr.c
 * @brief  Route 配置构建器：读取 DB 并生成 show current-configuration 输出
 * @author jhb
 * @date   2026/03/28
 */
#include "route_bdr.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "route.h"
#include "route_main.h"

static void route_bdr_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(route_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

int route_bdr_handle_show_config(dev_ipc_message_t *msg)
{
    GString *out = g_string_new("");
    if (!out)
    {
        route_bdr_send_cli_response(msg, "");
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    int ret = db_rpc_query(g_route_local->dev_ipc_ctx, "route_static", NULL, 0, NULL, &result);
    if (ret != ERRCODE_SUCCESS || !result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        route_bdr_send_cli_response(msg, out->str);
        g_string_free(out, TRUE);
        return ERRCODE_SUCCESS;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        int64_t afi = db_row_get_int(row, "afi", ROUTE_AFI_IPV4);
        const char *prefix = db_row_get_text(row, "prefix", NULL);
        int64_t prefix_len = db_row_get_int(row, "prefix_len", 0);
        const char *nexthop = db_row_get_text(row, "nexthop", NULL);
        int64_t metric = db_row_get_int(row, "metric", 0);

        if (!prefix || !nexthop)
        {
            continue;
        }

        if (afi == ROUTE_AFI_IPV4)
        {
            g_string_append(out, "!\r\n");
            g_string_append_printf(out, "route ipv4 %s %ld %s", prefix, prefix_len, nexthop);
        }
        else if (afi == ROUTE_AFI_IPV6)
        {
            g_string_append(out, "!\r\n");
            g_string_append_printf(out, "route ipv6 %s %ld %s", prefix, prefix_len, nexthop);
        }
        else
        {
            continue;
        }

        if (metric != 0)
        {
            g_string_append_printf(out, " metric %ld", metric);
        }
        g_string_append(out, "\r\n");
    }

    if (out->len > 0)
    {
        g_string_append(out, "!\r\n");
    }

    db_result_free(result);

    /* 输出 batch 路由配置 */
    db_result_t *batch_result = NULL;
    ret = db_rpc_query(g_route_local->dev_ipc_ctx, "route_batch", NULL, 0, NULL, &batch_result);
    if (ret == ERRCODE_SUCCESS && batch_result && batch_result->num_rows > 0)
    {
        for (uint32_t i = 0; i < batch_result->num_rows; i++)
        {
            db_row_t *row = batch_result->rows[i];
            const char *name = db_row_get_text(row, "name", NULL);
            int64_t afi = db_row_get_int(row, "afi", ROUTE_AFI_IPV4);
            const char *start_addr = db_row_get_text(row, "start_addr", NULL);
            int64_t prefix_len = db_row_get_int(row, "prefix_len", 0);
            int64_t count = db_row_get_int(row, "count", 0);
            const char *nexthop = db_row_get_text(row, "nexthop", NULL);

            if (!name || !start_addr || !nexthop)
            {
                continue;
            }

            g_string_append(out, "!\r\n");
            if (afi == ROUTE_AFI_IPV4)
            {
                g_string_append_printf(out, "route batch %s ipv4 %s %ld count %ld nexthop %s\r\n", name, start_addr,
                                       prefix_len, count, nexthop);
            }
            else
            {
                g_string_append_printf(out, "route batch %s ipv6 %s %ld count %ld nexthop %s\r\n", name, start_addr,
                                       prefix_len, count, nexthop);
            }
        }
        if (out->len > 0)
        {
            g_string_append(out, "!\r\n");
        }
        db_result_free(batch_result);
    }

    route_bdr_send_cli_response(msg, out->str);
    g_string_free(out, TRUE);
    return ERRCODE_SUCCESS;
}
