/**
 * @file   route_cli.c
 * @brief  Route 模块 CLI 命令处理
 * @author jhb
 * @date   2026/02/01
 */
#include "route_cli.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "route_main.h"
#include "cfg.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "ipc.h"

// ============================================================================
// 辅助函数
// ============================================================================

static void send_resp(ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                   msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        ipc_send_response(g_route_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }
}

// ============================================================================
// Show 输出写入临时 DB
// ============================================================================

#define ROUTE_SHOW_DB "route_show_db"
#define ROUTE_SHOW_META "route_show_meta"
#define ROUTE_SHOW_IPV4 "route_show_ipv4"
#define ROUTE_SHOW_IPV6 "route_show_ipv6"

static int route_insert_show_meta(int has_rows)
{
    const char *fields[] = {"has_rows"};
    db_value_t values[] = {db_value_int(has_rows ? 1 : 0)};
    return db_insert(ROUTE_SHOW_DB, ROUTE_SHOW_META, fields, values, 1);
}

static int route_insert_show_ipv4(const char *destination, const char *mask, const char *next_hop, int64_t metric,
                                  int is_first)
{
    const char *fields[] = {"destination", "mask", "next_hop", "metric", "is_first"};
    db_value_t values[] = {db_value_text(destination), db_value_text(mask), db_value_text(next_hop),
                              db_value_int(metric), db_value_int(is_first ? 1 : 0)};
    int ret = db_insert(ROUTE_SHOW_DB, ROUTE_SHOW_IPV4, fields, values, 5);
    db_value_free(&values[0]);
    db_value_free(&values[1]);
    db_value_free(&values[2]);
    return ret;
}

static int route_insert_show_ipv6(const char *destination, int64_t prefix_length, const char *next_hop, int64_t metric,
                                  int is_first)
{
    const char *fields[] = {"destination", "prefix_length", "next_hop", "metric", "is_first"};
    db_value_t values[] = {db_value_text(destination), db_value_int(prefix_length),
                              db_value_text(next_hop), db_value_int(metric), db_value_int(is_first ? 1 : 0)};
    int ret = db_insert(ROUTE_SHOW_DB, ROUTE_SHOW_IPV6, fields, values, 5);
    db_value_free(&values[0]);
    db_value_free(&values[2]);
    return ret;
}

// ============================================================================
// DB table 载荷处理函数
// ============================================================================

/**
 * @brief 处理 route_ipv4 表命令
 *
 * 根据字段判断操作类型：
 * - 有 destination + mask + next_hop → 添加 IPv4 路由
 * - no 命令 + destination + mask → 删除 IPv4 路由
 */
static int handle_route_ipv4_db(ipc_message_t *msg, cfg_db_payload_parser_t *parser)
{
    gboolean is_no = CFG_DB_IS_NO_CMD(parser);
    char destination[128] = {0};
    char mask[64] = {0};
    char next_hop[128] = {0};
    int64_t metric = 0;
    int has_destination = 0;
    int has_mask = 0;
    int has_next_hop = 0;
    int has_metric = 0;

    /* 解析字段 */
    uint8_t ff;
    char *fn;
    db_value_t val;
    while (cfg_db_payload_next(parser, &ff, &fn, &val) == 1)
    {
        if (strcmp(fn, "destination") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
        {
            strncpy(destination, val.data.text, sizeof(destination) - 1);
            has_destination = 1;
        }
        else if (strcmp(fn, "mask") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
        {
            strncpy(mask, val.data.text, sizeof(mask) - 1);
            has_mask = 1;
        }
        else if (strcmp(fn, "next_hop") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
        {
            strncpy(next_hop, val.data.text, sizeof(next_hop) - 1);
            has_next_hop = 1;
        }
        else if (strcmp(fn, "metric") == 0 && val.type == DB_TYPE_INTEGER)
        {
            metric = val.data.i64;
            has_metric = 1;
        }
        g_free(fn);
        db_value_free(&val);
    }

    char resp_msg[512];

    /* 验证必填字段 */
    if (!has_destination)
    {
        send_resp(msg, "Error: Destination address required\r\n");
        return ERRCODE_FAIL;
    }
    if (!has_mask)
    {
        send_resp(msg, "Error: Subnet mask required for IPv4\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        /* 删除路由 */
        char where_clause[512];
        if (has_next_hop)
        {
            snprintf(where_clause, sizeof(where_clause), "destination='%s' AND mask='%s' AND next_hop='%s'",
                     destination, mask, next_hop);
        }
        else
        {
            snprintf(where_clause, sizeof(where_clause), "destination='%s' AND mask='%s'", destination, mask);
        }

        int rows = db_delete("route_db", "route_ipv4", where_clause);
        snprintf(resp_msg, sizeof(resp_msg), "Route deleted (%d row%s)\r\n", rows > 0 ? rows : 0,
                 rows == 1 ? "" : "s");
        send_resp(msg, resp_msg);
    }
    else
    {
        /* 添加路由 */
        if (!has_next_hop)
        {
            send_resp(msg, "Error: Next hop address required\r\n");
            return ERRCODE_FAIL;
        }

        const char *fields[] = {"destination", "mask", "next_hop", "metric"};
        db_value_t values[4];
        values[0] = db_value_text(destination);
        values[1] = db_value_text(mask);
        values[2] = db_value_text(next_hop);
        values[3] = db_value_int(has_metric ? metric : 0);

        int ret = db_insert("route_db", "route_ipv4", fields, values, 4);
        if (ret != ERRCODE_SUCCESS)
        {
            send_resp(msg, "Error: Failed to add route\r\n");
            return ERRCODE_FAIL;
        }

        send_resp(msg, "Route added successfully\r\n");
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 route_ipv6 表命令
 */
static int handle_route_ipv6_db(ipc_message_t *msg, cfg_db_payload_parser_t *parser)
{
    gboolean is_no = CFG_DB_IS_NO_CMD(parser);
    char destination[128] = {0};
    int64_t prefix_length = 0;
    char next_hop[128] = {0};
    int64_t metric = 0;
    int has_destination = 0;
    int has_prefix_length = 0;
    int has_next_hop = 0;
    int has_metric = 0;

    /* 解析字段 */
    uint8_t ff;
    char *fn;
    db_value_t val;
    while (cfg_db_payload_next(parser, &ff, &fn, &val) == 1)
    {
        if (strcmp(fn, "destination") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
        {
            strncpy(destination, val.data.text, sizeof(destination) - 1);
            has_destination = 1;
        }
        else if (strcmp(fn, "prefix_length") == 0 && val.type == DB_TYPE_INTEGER)
        {
            prefix_length = val.data.i64;
            has_prefix_length = 1;
        }
        else if (strcmp(fn, "next_hop") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
        {
            strncpy(next_hop, val.data.text, sizeof(next_hop) - 1);
            has_next_hop = 1;
        }
        else if (strcmp(fn, "metric") == 0 && val.type == DB_TYPE_INTEGER)
        {
            metric = val.data.i64;
            has_metric = 1;
        }
        g_free(fn);
        db_value_free(&val);
    }

    char resp_msg[512];

    /* 验证必填字段 */
    if (!has_destination)
    {
        send_resp(msg, "Error: Destination address required\r\n");
        return ERRCODE_FAIL;
    }
    if (!has_prefix_length)
    {
        send_resp(msg, "Error: Prefix length required for IPv6\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        /* 删除路由 */
        char where_clause[512];
        if (has_next_hop)
        {
            snprintf(where_clause, sizeof(where_clause),
                     "destination='%s' AND prefix_length=%ld AND next_hop='%s'", destination, prefix_length, next_hop);
        }
        else
        {
            snprintf(where_clause, sizeof(where_clause), "destination='%s' AND prefix_length=%ld", destination,
                     prefix_length);
        }

        int rows = db_delete("route_db", "route_ipv6", where_clause);
        snprintf(resp_msg, sizeof(resp_msg), "Route deleted (%d row%s)\r\n", rows > 0 ? rows : 0,
                 rows == 1 ? "" : "s");
        send_resp(msg, resp_msg);
    }
    else
    {
        /* 添加路由 */
        if (!has_next_hop)
        {
            send_resp(msg, "Error: Next hop address required\r\n");
            return ERRCODE_FAIL;
        }

        const char *fields[] = {"destination", "prefix_length", "next_hop", "metric"};
        db_value_t values[4];
        values[0] = db_value_text(destination);
        values[1] = db_value_int(prefix_length);
        values[2] = db_value_text(next_hop);
        values[3] = db_value_int(has_metric ? metric : 0);

        int ret = db_insert("route_db", "route_ipv6", fields, values, 4);
        if (ret != ERRCODE_SUCCESS)
        {
            send_resp(msg, "Error: Failed to add route\r\n");
            return ERRCODE_FAIL;
        }

        send_resp(msg, "Route added successfully\r\n");
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 show route 命令
 *
 * 根据 payload 的 family/destination 字段决定显示 IPv4/IPv6/ALL
 */
static int handle_show_route_db(ipc_message_t *msg, cfg_db_payload_parser_t *parser)
{
    /* 解析可选的 destination 与 family 过滤条件 */
    char filter_dest[128] = {0};
    char family[16] = {0};
    int show_ipv4 = 0;
    int show_ipv6 = 0;

    uint8_t ff;
    char *fn;
    db_value_t val;
    while (cfg_db_payload_next(parser, &ff, &fn, &val) == 1)
    {
        if (strcmp(fn, "destination") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
        {
            strncpy(filter_dest, val.data.text, sizeof(filter_dest) - 1);
        }
        else if (strcmp(fn, "family") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
        {
            strncpy(family, val.data.text, sizeof(family) - 1);
        }
        g_free(fn);
        db_value_free(&val);
    }

    if (family[0] == '\0' || strcmp(family, "all") == 0)
    {
        show_ipv4 = 1;
        show_ipv6 = 1;
    }
    else if (strcmp(family, "ipv4") == 0)
    {
        show_ipv4 = 1;
    }
    else if (strcmp(family, "ipv6") == 0)
    {
        show_ipv6 = 1;
    }
    else
    {
        send_resp(msg, "Route Error: Invalid family.\r\n");
        return ERRCODE_FAIL;
    }

    if (route_insert_show_meta(0) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "Route Error: Failed to write show output.\r\n");
        return ERRCODE_FAIL;
    }

    char where_clause[256] = "";
    if (strlen(filter_dest) > 0)
    {
        snprintf(where_clause, sizeof(where_clause), "destination='%s'", filter_dest);
    }

    int ipv4_rows = 0;
    int ipv6_rows = 0;

    if (show_ipv4)
    {
        db_result_t *result = NULL;
        int ret = db_query("route_db", "route_ipv4", NULL, 0, strlen(where_clause) > 0 ? where_clause : NULL,
                              &result);
        if (ret == ERRCODE_SUCCESS && result != NULL)
        {
            for (uint32_t i = 0; i < result->num_rows; i++)
            {
                db_row_t *row = result->rows[i];
                const char *dest = "";
                const char *mask = "";
                const char *next_hop_str = "";
                int64_t metric_val = 0;

                for (uint32_t j = 0; j < row->num_fields; j++)
                {
                    if (strcmp(row->field_names[j], "destination") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        dest = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "mask") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        mask = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "next_hop") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        next_hop_str = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "metric") == 0 && row->values[j].type == DB_TYPE_INTEGER)
                        metric_val = row->values[j].data.i64;
                }

                if (route_insert_show_ipv4(dest, mask, next_hop_str ? next_hop_str : "", metric_val,
                                           ipv4_rows == 0) != ERRCODE_SUCCESS)
                {
                    db_result_free(result);
                    send_resp(msg, "Route Error: Failed to write show output.\r\n");
                    return ERRCODE_FAIL;
                }
                ipv4_rows++;
            }
            db_result_free(result);
        }
    }

    if (show_ipv6)
    {
        db_result_t *result = NULL;
        int ret = db_query("route_db", "route_ipv6", NULL, 0, strlen(where_clause) > 0 ? where_clause : NULL,
                              &result);
        if (ret == ERRCODE_SUCCESS && result != NULL)
        {
            for (uint32_t i = 0; i < result->num_rows; i++)
            {
                db_row_t *row = result->rows[i];
                const char *dest = "";
                int64_t prefix_len = 0;
                const char *next_hop_str = "";
                int64_t metric_val = 0;

                for (uint32_t j = 0; j < row->num_fields; j++)
                {
                    if (strcmp(row->field_names[j], "destination") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        dest = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "prefix_length") == 0 &&
                             row->values[j].type == DB_TYPE_INTEGER)
                        prefix_len = row->values[j].data.i64;
                    else if (strcmp(row->field_names[j], "next_hop") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        next_hop_str = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "metric") == 0 && row->values[j].type == DB_TYPE_INTEGER)
                        metric_val = row->values[j].data.i64;
                }

                if (route_insert_show_ipv6(dest, prefix_len, next_hop_str ? next_hop_str : "", metric_val,
                                           ipv6_rows == 0) != ERRCODE_SUCCESS)
                {
                    db_result_free(result);
                    send_resp(msg, "Route Error: Failed to write show output.\r\n");
                    return ERRCODE_FAIL;
                }
                ipv6_rows++;
            }
            db_result_free(result);
        }
    }

    int has_rows = (ipv4_rows + ipv6_rows) > 0;
    const char *fields[] = {"has_rows"};
    db_value_t values[] = {db_value_int(has_rows ? 1 : 0)};
    db_update(ROUTE_SHOW_DB, ROUTE_SHOW_META, fields, values, 1, NULL);

    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 使用 DB table 载荷格式分发命令
 *
 * 根据 table_name 和字段内容区分 IPv4/IPv6：
 * - route_ipv4 + 有 mask 字段 → IPv4 配置命令
 * - route_ipv4 + 有 prefix_length 字段 → IPv6 配置命令（需要操作 route_ipv6 表）
 * - route_ipv4 + show 命令（由 group 2 发来）→ show 命令
 */
static int route_dispatch_db_payload(ipc_message_t *msg, cfg_db_payload_parser_t *parser)
{
    /* 预扫描字段，判断是 IPv4 还是 IPv6
     * 注意：这会消耗迭代器，所以需要重新初始化 parser 或使用另一种方式
     * 实际上我们可以根据字段的存在来判断：
     * - 如果有 mask 字段 → IPv4
     * - 如果有 prefix_length 字段 → IPv6
     */

    /* 保存原始位置用于重新解析 */
    const uint8_t *orig_data = parser->_reader_data;
    uint32_t orig_len = parser->_reader_len;

    int has_mask = 0;
    int has_prefix_length = 0;
    int has_action_show = 0;

    uint8_t ff;
    char *fn;
    db_value_t val;
    while (cfg_db_payload_next(parser, &ff, &fn, &val) == 1)
    {
        if (strcmp(fn, "mask") == 0)
        {
            has_mask = 1;
        }
        else if (strcmp(fn, "prefix_length") == 0)
        {
            has_prefix_length = 1;
        }
        else if (strcmp(fn, "action") == 0 && val.type == DB_TYPE_TEXT && val.data.text &&
                 strcmp(val.data.text, "show") == 0)
        {
            has_action_show = 1;
        }
        g_free(fn);
        db_value_free(&val);
    }

    /* 重新初始化 parser 以便后续处理函数可以重新解析字段 */
    cfg_db_payload_parser_t new_parser;
    if (cfg_db_payload_init(&new_parser, orig_data, orig_len) != 0)
    {
        send_resp(msg, "Route Error: Failed to re-parse payload.\r\n");
        return ERRCODE_FAIL;
    }

    int result;

    if (strcmp(parser->table_name, "route_ipv4") == 0)
    {
        if (has_action_show)
        {
            result = handle_show_route_db(msg, &new_parser);
        }
        else if (has_prefix_length)
        {
            /* 虽然 table_name 是 route_ipv4，但有 prefix_length 说明是 IPv6 命令 */
            result = handle_route_ipv6_db(msg, &new_parser);
        }
        else if (has_mask)
        {
            result = handle_route_ipv4_db(msg, &new_parser);
        }
        else
        {
            send_resp(msg, "Route Error: Cannot determine route type.\r\n");
            result = ERRCODE_FAIL;
        }
    }
    else
    {
        printf("[route_cfg] 未知表名: %s\n", parser->table_name);
        send_resp(msg, "Route Error: Unknown table.\r\n");
        result = ERRCODE_FAIL;
    }

    cfg_db_payload_cleanup(&new_parser);
    return result;
}

// ============================================================================
// 旧 TLV 格式处理（兼容）
// ============================================================================

/**
 * @brief 使用旧 TLV 格式处理 show route 命令
 *
 * show route ipv4/ipv6/all 命令的 ipv4/ipv6/all 关键字没有 field 属性，
 * 因此无法通过新 DB 载荷格式传递。这些命令需要回退到旧 TLV 解析。
 */
static int handle_show_route_legacy(ipc_message_t *msg, cfg_tlv_parser_t *parser)
{
    int show_ipv4 = 0;
    int show_ipv6 = 0;
    char filter_dest[128] = {0};

    uint32_t cfg_id;
    const uint8_t *value;
    uint16_t len;
    while (cfg_tlv_parser_next(parser, &cfg_id, &value, &len) == 0)
    {
        switch (cfg_id)
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
            case 4: /* destination filter */
                if (len > 0 && len < sizeof(filter_dest))
                {
                    memcpy(filter_dest, value, len);
                    filter_dest[len] = '\0';
                }
                break;
        }
    }

    char response[4096];
    int offset = 0;

    char where_clause[256] = "";
    if (strlen(filter_dest) > 0)
    {
        snprintf(where_clause, sizeof(where_clause), "destination='%s'", filter_dest);
    }

    /* 显示 IPv4 路由 */
    if (show_ipv4)
    {
        offset += snprintf(response + offset, sizeof(response) - offset, "\r\nIPv4 Routes:\r\n");
        offset += snprintf(response + offset, sizeof(response) - offset, "%-18s %-18s %-18s %-8s\r\n", "Destination",
                           "Mask", "Next Hop", "Metric");
        offset += snprintf(response + offset, sizeof(response) - offset, "%-18s %-18s %-18s %-8s\r\n",
                           "------------------", "------------------", "------------------", "--------");

        db_result_t *result = NULL;
        int ret =
            db_query("route_db", "route_ipv4", NULL, 0, strlen(where_clause) > 0 ? where_clause : NULL, &result);
        if (ret == ERRCODE_SUCCESS && result != NULL)
        {
            for (uint32_t i = 0; i < result->num_rows; i++)
            {
                db_row_t *row = result->rows[i];
                const char *dest = "";
                const char *mask = "";
                const char *next_hop_str = "";
                int64_t metric_val = 0;

                for (uint32_t j = 0; j < row->num_fields; j++)
                {
                    if (strcmp(row->field_names[j], "destination") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        dest = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "mask") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        mask = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "next_hop") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        next_hop_str = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "metric") == 0 && row->values[j].type == DB_TYPE_INTEGER)
                        metric_val = row->values[j].data.i64;
                }

                offset += snprintf(response + offset, sizeof(response) - offset, "%-18s %-18s %-18s %-8ld\r\n", dest,
                                   mask, next_hop_str ? next_hop_str : "", metric_val);
            }
            db_result_free(result);
        }
    }

    /* 显示 IPv6 路由 */
    if (show_ipv6)
    {
        offset += snprintf(response + offset, sizeof(response) - offset, "\r\nIPv6 Routes:\r\n");
        offset += snprintf(response + offset, sizeof(response) - offset, "%-40s %-6s %-40s %-8s\r\n", "Destination",
                           "Prefix", "Next Hop", "Metric");
        offset += snprintf(response + offset, sizeof(response) - offset, "%-40s %-6s %-40s %-8s\r\n",
                           "----------------------------------------", "------",
                           "----------------------------------------", "--------");

        db_result_t *result = NULL;
        int ret =
            db_query("route_db", "route_ipv6", NULL, 0, strlen(where_clause) > 0 ? where_clause : NULL, &result);
        if (ret == ERRCODE_SUCCESS && result != NULL)
        {
            for (uint32_t i = 0; i < result->num_rows; i++)
            {
                db_row_t *row = result->rows[i];
                const char *dest = "";
                int64_t prefix_len = 0;
                const char *next_hop_str = "";
                int64_t metric_val = 0;

                for (uint32_t j = 0; j < row->num_fields; j++)
                {
                    if (strcmp(row->field_names[j], "destination") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        dest = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "prefix_length") == 0 &&
                             row->values[j].type == DB_TYPE_INTEGER)
                        prefix_len = row->values[j].data.i64;
                    else if (strcmp(row->field_names[j], "next_hop") == 0 && row->values[j].type == DB_TYPE_TEXT)
                        next_hop_str = row->values[j].data.text;
                    else if (strcmp(row->field_names[j], "metric") == 0 && row->values[j].type == DB_TYPE_INTEGER)
                        metric_val = row->values[j].data.i64;
                }

                offset += snprintf(response + offset, sizeof(response) - offset, "%-40s %-6ld %-40s %-8ld\r\n", dest,
                                   prefix_len, next_hop_str ? next_hop_str : "", metric_val);
            }
            db_result_free(result);
        }
    }

    offset += snprintf(response + offset, sizeof(response) - offset, "\r\n");
    send_resp(msg, response);

    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int route_cli_handle_continue(ipc_message_t *msg)
{
    char *resp_data = g_strdup("");
    ipc_message_t *resp_msg = ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                       msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp_msg)
    {
        ipc_send_response(g_route_local->ipc_ctx, resp_msg);
        ipc_message_free(resp_msg);
    }
    return ERRCODE_SUCCESS;
}

int route_cli_handle_message(ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    /* 尝试新 DB table 载荷格式 */
    cfg_db_payload_parser_t parser;
    if (cfg_db_payload_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) == 0)
    {
        printf("[route_cfg] 收到 DB table 载荷 (db=%s, table=%s, num_fields=%u)\n", parser.db_name, parser.table_name,
               parser.num_fields);

        int result = route_dispatch_db_payload(msg, &parser);
        cfg_db_payload_cleanup(&parser);
        return result;
    }

    /* 回退到旧 TLV 格式 */
    cfg_tlv_parser_t tlv_parser;
    if (cfg_tlv_parser_init(&tlv_parser, (const uint8_t *)msg->payload, msg->payload_len) == 0)
    {
        printf("[route_cfg] 旧 TLV 格式 (group_id=%u)\n", tlv_parser.group_id);
        if (tlv_parser.group_id == 2)
        {
            return handle_show_route_legacy(msg, &tlv_parser);
        }
    }

    printf("[route_cfg] 无法解析消息\n");
    send_resp(msg, "Route Error: Unsupported message format.\r\n");
    return ERRCODE_FAIL;
}
