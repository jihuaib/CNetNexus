/**
 * @file   route_cli.c
 * @brief  Route 模块 CLI 命令处理
 * @author jhb
 * @date   2026/02/01
 */
#include "route_cli.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "route_main.h"

// ============================================================================
// 辅助函数
// ============================================================================

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_route_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理路由配置命令
 *
 * group_id=1, cfg_id:
 *   1=no, 2=ipv4, 3=ipv6,
 *   4=destination, 5=mask, 6=prefix_length,
 *   7=next_hop, 8=metric
 */
static int handle_route_config(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = FALSE;
    int is_ipv4 = 0;
    int is_ipv6 = 0;
    char destination[128] = {0};
    char mask[64] = {0};
    int64_t prefix_length = 0;
    char next_hop[128] = {0};
    int64_t metric = 0;
    int has_destination = 0;
    int has_mask = 0;
    int has_prefix_length = 0;
    int has_next_hop = 0;
    int has_metric = 0;

    /* 按 cfg_id 解析 TLV 条目 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* no 前缀 */
                is_no = TRUE;
                break;
            case 2: /* ipv4 关键字 */
                is_ipv4 = 1;
                break;
            case 3: /* ipv6 关键字 */
                is_ipv6 = 1;
                break;
            case 4: /* destination 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    strncpy(destination, text, sizeof(destination) - 1);
                    has_destination = 1;
                }
                break;
            }
            case 5: /* mask 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    strncpy(mask, text, sizeof(mask) - 1);
                    has_mask = 1;
                }
                break;
            }
            case 6: /* prefix_length 参数 */
                prefix_length = cli_tlv_entry_get_int(&entry);
                has_prefix_length = 1;
                break;
            case 7: /* next_hop 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    strncpy(next_hop, text, sizeof(next_hop) - 1);
                    has_next_hop = 1;
                }
                break;
            }
            case 8: /* metric 参数 */
                metric = cli_tlv_entry_get_int(&entry);
                has_metric = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    char resp_msg[512];

    /* 验证必填字段 */
    if (!has_destination)
    {
        send_resp(msg, "Error: Destination address required\r\n");
        return ERRCODE_FAIL;
    }

    if (is_ipv4)
    {
        if (!has_mask)
        {
            send_resp(msg, "Error: Subnet mask required for IPv4\r\n");
            return ERRCODE_FAIL;
        }

        if (is_no)
        {
            db_condition_t conditions[3];
            uint32_t num_conditions = 0;
            conditions[num_conditions++] = (db_condition_t){
                .field_name = "destination",
                .op = DB_CMP_EQ,
                .value = db_value_text(destination),
            };
            conditions[num_conditions++] = (db_condition_t){
                .field_name = "mask",
                .op = DB_CMP_EQ,
                .value = db_value_text(mask),
            };
            if (has_next_hop)
            {
                conditions[num_conditions++] = (db_condition_t){
                    .field_name = "next_hop",
                    .op = DB_CMP_EQ,
                    .value = db_value_text(next_hop),
                };
            }
            db_filter_t filter = {.conditions = conditions, .num_conditions = num_conditions};

            int rows = db_rpc_delete(g_route_local->dev_ipc_ctx, "route_ipv4", &filter);
            for (uint32_t i = 0; i < num_conditions; i++)
            {
                db_value_free(&conditions[i].value);
            }
            snprintf(resp_msg, sizeof(resp_msg), "Route deleted (%d row%s)\r\n", rows > 0 ? rows : 0,
                     rows == 1 ? "" : "s");
            send_resp(msg, resp_msg);
        }
        else
        {
            if (!has_next_hop)
            {
                send_resp(msg, "Error: Next hop address required\r\n");
                return ERRCODE_FAIL;
            }

            db_record_t *rec = db_record_new();
            db_record_set_text(rec, "destination", destination);
            db_record_set_text(rec, "mask", mask);
            db_record_set_text(rec, "next_hop", next_hop);
            db_record_set_int(rec, "metric", has_metric ? metric : 0);
            int ret = db_rpc_insert_record(g_route_local->dev_ipc_ctx, "route_ipv4", rec);
            db_record_free(rec);
            if (ret != ERRCODE_SUCCESS)
            {
                send_resp(msg, "Error: Failed to add route\r\n");
                return ERRCODE_FAIL;
            }

            send_resp(msg, "Route added successfully\r\n");
        }
    }
    else if (is_ipv6)
    {
        if (!has_prefix_length)
        {
            send_resp(msg, "Error: Prefix length required for IPv6\r\n");
            return ERRCODE_FAIL;
        }

        if (is_no)
        {
            db_condition_t conditions[3];
            uint32_t num_conditions = 0;
            conditions[num_conditions++] = (db_condition_t){
                .field_name = "destination",
                .op = DB_CMP_EQ,
                .value = db_value_text(destination),
            };
            conditions[num_conditions++] = (db_condition_t){
                .field_name = "prefix_length",
                .op = DB_CMP_EQ,
                .value = db_value_int(prefix_length),
            };
            if (has_next_hop)
            {
                conditions[num_conditions++] = (db_condition_t){
                    .field_name = "next_hop",
                    .op = DB_CMP_EQ,
                    .value = db_value_text(next_hop),
                };
            }
            db_filter_t filter = {.conditions = conditions, .num_conditions = num_conditions};

            int rows = db_rpc_delete(g_route_local->dev_ipc_ctx, "route_ipv6", &filter);
            for (uint32_t i = 0; i < num_conditions; i++)
            {
                db_value_free(&conditions[i].value);
            }
            snprintf(resp_msg, sizeof(resp_msg), "Route deleted (%d row%s)\r\n", rows > 0 ? rows : 0,
                     rows == 1 ? "" : "s");
            send_resp(msg, resp_msg);
        }
        else
        {
            if (!has_next_hop)
            {
                send_resp(msg, "Error: Next hop address required\r\n");
                return ERRCODE_FAIL;
            }

            db_record_t *rec = db_record_new();
            db_record_set_text(rec, "destination", destination);
            db_record_set_int(rec, "prefix_length", prefix_length);
            db_record_set_text(rec, "next_hop", next_hop);
            db_record_set_int(rec, "metric", has_metric ? metric : 0);
            int ret = db_rpc_insert_record(g_route_local->dev_ipc_ctx, "route_ipv6", rec);
            db_record_free(rec);
            if (ret != ERRCODE_SUCCESS)
            {
                send_resp(msg, "Error: Failed to add route\r\n");
                return ERRCODE_FAIL;
            }

            send_resp(msg, "Route added successfully\r\n");
        }
    }
    else
    {
        send_resp(msg, "Error: Must specify ipv4 or ipv6\r\n");
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 show route 命令
 *
 * group_id=2, cfg_id: 1=ipv4, 2=ipv6, 3=all, 4=destination
 * 直接构建格式化文本返回
 */
static int handle_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int show_ipv4 = 0;
    int show_ipv6 = 0;
    char filter_dest[128] = {0};

    /* 解析 TLV 条目 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* ipv4 */
                show_ipv4 = 1;
                break;
            case 2: /* ipv6 */
                show_ipv6 = 1;
                break;
            case 3: /* all */
                show_ipv4 = 1;
                show_ipv6 = 1;
                break;
            case 4: /* destination 过滤 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    strncpy(filter_dest, text, sizeof(filter_dest) - 1);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    /* 默认显示所有 */
    if (!show_ipv4 && !show_ipv6)
    {
        show_ipv4 = 1;
        show_ipv6 = 1;
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    size_t offset = 0;

    db_condition_t dest_condition;
    db_filter_t *query_filter = NULL;
    db_filter_t filter;
    if (strlen(filter_dest) > 0)
    {
        dest_condition = (db_condition_t){
            .field_name = "destination",
            .op = DB_CMP_EQ,
            .value = db_value_text(filter_dest),
        };
        filter = (db_filter_t){.conditions = &dest_condition, .num_conditions = 1};
        query_filter = &filter;
    }

    /* 显示 IPv4 路由 */
    if (show_ipv4)
    {
        db_result_t *result = NULL;
        int ret = db_rpc_query(g_route_local->dev_ipc_ctx, "route_ipv4", NULL, 0, query_filter, &result);

        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset,
                       "\r\nIPv4 Routing Table:\r\n"
                       "%-18s %-18s %-18s %-8s\r\n"
                       "------------------ ------------------ ------------------ --------\r\n",
                       "Destination", "Mask", "Next Hop", "Metric");

        if (ret == ERRCODE_SUCCESS && result != NULL)
        {
            for (uint32_t i = 0; i < result->num_rows; i++)
            {
                db_row_t *row = result->rows[i];
                const char *dest = db_row_get_text(row, "destination", "");
                const char *mask_val = db_row_get_text(row, "mask", "");
                const char *next_hop_str = db_row_get_text(row, "next_hop", "");
                int64_t metric_val = db_row_get_int(row, "metric", 0);

                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "%-18s %-18s %-18s %-8ld\r\n", dest, mask_val,
                               next_hop_str, metric_val);

                if (offset >= sizeof(resp_buf) - 128)
                {
                    CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  ... (truncated)\r\n");
                    break;
                }
            }
            if (result->num_rows == 0)
            {
                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  (no routes)\r\n");
            }
            db_result_free(result);
        }
        else
        {
            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  (no routes)\r\n");
        }
    }

    /* 显示 IPv6 路由 */
    if (show_ipv6)
    {
        db_result_t *result = NULL;
        int ret = db_rpc_query(g_route_local->dev_ipc_ctx, "route_ipv6", NULL, 0, query_filter, &result);

        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset,
                       "\r\nIPv6 Routing Table:\r\n"
                       "%-30s %-8s %-30s %-8s\r\n"
                       "------------------------------ -------- ------------------------------ --------\r\n",
                       "Destination", "Prefix", "Next Hop", "Metric");

        if (ret == ERRCODE_SUCCESS && result != NULL)
        {
            for (uint32_t i = 0; i < result->num_rows; i++)
            {
                db_row_t *row = result->rows[i];
                const char *dest = db_row_get_text(row, "destination", "");
                int64_t prefix_len = db_row_get_int(row, "prefix_length", 0);
                const char *next_hop_str = db_row_get_text(row, "next_hop", "");
                int64_t metric_val = db_row_get_int(row, "metric", 0);

                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "%-30s %-8ld %-30s %-8ld\r\n", dest, prefix_len,
                               next_hop_str, metric_val);

                if (offset >= sizeof(resp_buf) - 128)
                {
                    CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  ... (truncated)\r\n");
                    break;
                }
            }
            if (result->num_rows == 0)
            {
                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  (no routes)\r\n");
            }
            db_result_free(result);
        }
        else
        {
            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  (no routes)\r\n");
        }
    }

    if (query_filter)
    {
        db_value_free(&dest_condition.value);
    }

    CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\n");

    send_resp(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int route_cli_handle_continue(dev_ipc_message_t *msg)
{
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

int route_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        send_resp(msg, "Route Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case ROUTE_CLI_GROUP_ID_CONFIG:
            result = handle_route_config(msg, &parser);
            break;
        case ROUTE_CLI_GROUP_ID_SHOW:
            result = handle_show_route(msg, &parser);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            send_resp(msg, "Route Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
