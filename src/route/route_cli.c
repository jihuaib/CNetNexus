/**
 * @file   route_cli.c
 * @brief  Route 模块 CLI 配置命令处理（在 IPC 线程调用：TLV 解析 → DB 持久化 → 向 worker 派发配置应用）
 * @author jhb
 * @date   2026/02/01
 */
#include "route_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_db.h"
#include "route_main.h"
#include "route_worker.h"

// ============================================================================
// 辅助函数
// ============================================================================

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_ROUTE, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_route_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

/**
 * @brief 将 IPv4 掩码字符串（如 "255.255.255.0"）转换为前缀长度
 * @return 前缀长度 0-32，失败返回 -1
 */
static int mask_to_prefix_len(const char *mask)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, mask, &addr) != 1)
    {
        return -1;
    }
    uint32_t m = ntohl(addr.s_addr);
    if (m == 0)
    {
        return 0;
    }
    uint32_t inv = ~m;
    if ((inv & (inv + 1)) != 0)
    {
        return -1;
    }
    int len = 0;
    while (m & 0x80000000u)
    {
        len++;
        m <<= 1;
    }
    return len;
}

// ============================================================================
// Group 1: 路由配置命令
//
// cfg-id 映射：
//   1=no, 2=ipv4, 3=ipv6, 4=prefix(dest), 5=mask(IPv4),
//   6=prefix_len(IPv6), 7=nexthop, 8=metric value
// ============================================================================

static int handle_route_config(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int is_no = 0;
    int is_ipv4 = 0;
    int is_ipv6 = 0;
    char prefix_str[64] = {0};
    char mask_str[64] = {0};
    char nexthop_str[64] = {0};
    int64_t ipv6_prefix_len = 0;
    int64_t metric = 0;
    int has_prefix = 0;
    int has_mask = 0;
    int has_pfxlen = 0;
    int has_nexthop = 0;
    int has_metric = 0;

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
            case 1:
                is_no = 1;
                break;
            case 2:
                is_ipv4 = 1;
                break;
            case 3:
                is_ipv6 = 1;
                break;
            case 4:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(prefix_str, text, sizeof(prefix_str));
                    has_prefix = 1;
                }
                break;
            }
            case 5:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(mask_str, text, sizeof(mask_str));
                    has_mask = 1;
                }
                break;
            }
            case 6:
                ipv6_prefix_len = cli_tlv_entry_get_int(&entry);
                has_pfxlen = 1;
                break;
            case 7:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(nexthop_str, text, sizeof(nexthop_str));
                    has_nexthop = 1;
                }
                break;
            }
            case 8:
                metric = cli_tlv_entry_get_int(&entry);
                has_metric = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_prefix)
    {
        send_resp(msg, "Error: Missing destination prefix\r\n");
        return ERRCODE_FAIL;
    }

    uint16_t afi;
    uint8_t prefix_len;

    if (is_ipv4)
    {
        if (!has_mask)
        {
            send_resp(msg, "Error: IPv4 route requires subnet mask\r\n");
            return ERRCODE_FAIL;
        }
        int pl = mask_to_prefix_len(mask_str);
        if (pl < 0)
        {
            send_resp(msg, "Error: Invalid subnet mask\r\n");
            return ERRCODE_FAIL;
        }
        afi = ROUTE_AFI_IPV4;
        prefix_len = (uint8_t)pl;
    }
    else if (is_ipv6)
    {
        if (!has_pfxlen)
        {
            send_resp(msg, "Error: IPv6 route requires prefix length\r\n");
            return ERRCODE_FAIL;
        }
        afi = ROUTE_AFI_IPV6;
        prefix_len = (uint8_t)ipv6_prefix_len;
    }
    else
    {
        send_resp(msg, "Error: Must specify ipv4 or ipv6\r\n");
        return ERRCODE_FAIL;
    }

    net_addr_t prefix_addr;
    if (net_addr_from_str(prefix_str, &prefix_addr) != 0)
    {
        send_resp(msg, "Error: Invalid prefix address\r\n");
        return ERRCODE_FAIL;
    }
    if (net_addr_prefix_normalize(&prefix_addr, prefix_len) != 0)
    {
        send_resp(msg, "Error: Invalid prefix length\r\n");
        return ERRCODE_FAIL;
    }
    char normalized_prefix_str[64] = {0};
    net_addr_to_str(&prefix_addr, normalized_prefix_str, sizeof(normalized_prefix_str));
    int prefix_text_changed = (strcmp(prefix_str, normalized_prefix_str) != 0);

    if (is_no)
    {
        if (has_nexthop)
        {
            net_addr_t nexthop_addr;
            if (net_addr_from_str(nexthop_str, &nexthop_addr) != 0)
            {
                send_resp(msg, "Error: Invalid nexthop address\r\n");
                return ERRCODE_FAIL;
            }

            /* 先删除 DB 记录 */
            route_db_delete_static(g_route_local->dev_ipc_ctx, ROUTE_VRF_DEFAULT, afi, normalized_prefix_str,
                                   prefix_len, nexthop_str);
            if (prefix_text_changed)
            {
                route_db_delete_static(g_route_local->dev_ipc_ctx, ROUTE_VRF_DEFAULT, afi, prefix_str, prefix_len,
                                       nexthop_str);
            }

            /* 向 worker 派发内存删除操作 */
            route_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.op = ROUTE_APPLY_STATIC_DEL;
            apply.u.static_del.vrf_id = ROUTE_VRF_DEFAULT;
            apply.u.static_del.afi = afi;
            apply.u.static_del.prefix_len = prefix_len;
            apply.u.static_del.prefix_addr = prefix_addr;
            apply.u.static_del.nexthop_addr = nexthop_addr;
            route_worker_dispatch_apply(&apply);

            char buf[128];
            snprintf(buf, sizeof(buf), "Static route(s) deleted (%d)\r\n", apply.rc > 0 ? apply.rc : 0);
            send_resp(msg, buf);
        }
        else
        {
            /* 删除该前缀下所有候选静态路由 */
            route_db_delete_static_prefix(g_route_local->dev_ipc_ctx, ROUTE_VRF_DEFAULT, afi, normalized_prefix_str,
                                          prefix_len);
            if (prefix_text_changed)
            {
                route_db_delete_static_prefix(g_route_local->dev_ipc_ctx, ROUTE_VRF_DEFAULT, afi, prefix_str,
                                              prefix_len);
            }

            route_apply_cmd_t apply;
            memset(&apply, 0, sizeof(apply));
            apply.op = ROUTE_APPLY_STATIC_DEL_PREFIX;
            apply.u.static_del_prefix.vrf_id = ROUTE_VRF_DEFAULT;
            apply.u.static_del_prefix.afi = afi;
            apply.u.static_del_prefix.prefix_len = prefix_len;
            apply.u.static_del_prefix.prefix_addr = prefix_addr;
            route_worker_dispatch_apply(&apply);

            char buf[128];
            snprintf(buf, sizeof(buf), "Static route(s) deleted (%d)\r\n", apply.rc > 0 ? apply.rc : 0);
            send_resp(msg, buf);
        }
    }
    else
    {
        if (!has_nexthop)
        {
            send_resp(msg, "Error: Missing next-hop address\r\n");
            return ERRCODE_FAIL;
        }

        net_addr_t nexthop_addr;
        if (net_addr_from_str(nexthop_str, &nexthop_addr) != 0)
        {
            send_resp(msg, "Error: Invalid nexthop address\r\n");
            return ERRCODE_FAIL;
        }

        int32_t pref = ROUTE_ADMIN_DIST_STATIC;
        int32_t m = has_metric ? (int32_t)metric : 0;

        /* 先写入 DB */
        route_db_upsert_static(g_route_local->dev_ipc_ctx, ROUTE_VRF_DEFAULT, afi, normalized_prefix_str, prefix_len,
                               nexthop_str, m, pref);
        if (prefix_text_changed)
        {
            route_db_delete_static(g_route_local->dev_ipc_ctx, ROUTE_VRF_DEFAULT, afi, prefix_str, prefix_len,
                                   nexthop_str);
        }

        /* 向 worker 派发内存添加操作 */
        route_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = ROUTE_APPLY_STATIC_ADD;
        apply.u.static_add.vrf_id = ROUTE_VRF_DEFAULT;
        apply.u.static_add.afi = afi;
        apply.u.static_add.prefix_len = prefix_len;
        apply.u.static_add.prefix_addr = prefix_addr;
        apply.u.static_add.nexthop_addr = nexthop_addr;
        apply.u.static_add.metric = m;
        apply.u.static_add.preference = pref;
        route_worker_dispatch_apply(&apply);

        send_resp(msg, "Static route added\r\n");
    }

    return ERRCODE_SUCCESS;
}

// ============================================================================
// Group 3: 批量路由命令
//
// cfg-id 映射：
//   1=no, 2=batch, 3=ipv4, 4=ipv6,
//   5=start_addr, 6=prefix_len(IPv4 0-32), 7=prefix_len(IPv6 0-128),
//   8=count_value, 9=nexthop_value, 10=name
// ============================================================================

static int handle_route_batch(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int is_no = 0;
    int is_ipv4 = 0;
    int is_ipv6 = 0;
    char name[64] = {0};
    char start_addr[64] = {0};
    int64_t ipv4_prefix_len = 0;
    int64_t ipv6_prefix_len = 0;
    int64_t count = 0;
    char nexthop[64] = {0};
    int has_name = 0;
    int has_start = 0;
    int has_ipv4_pfxlen = 0;
    int has_pfxlen = 0;
    int has_count = 0;
    int has_nexthop = 0;

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
            case 1:
                is_no = 1;
                break;
            case 2:
                /* batch 关键字 */
                break;
            case 3:
                is_ipv4 = 1;
                break;
            case 4:
                is_ipv6 = 1;
                break;
            case 5:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(start_addr, text, sizeof(start_addr));
                    has_start = 1;
                }
                break;
            }
            case 6:
                ipv4_prefix_len = cli_tlv_entry_get_int(&entry);
                has_ipv4_pfxlen = 1;
                break;
            case 7:
                ipv6_prefix_len = cli_tlv_entry_get_int(&entry);
                has_pfxlen = 1;
                break;
            case 8:
                count = cli_tlv_entry_get_int(&entry);
                has_count = 1;
                break;
            case 9:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(nexthop, text, sizeof(nexthop));
                    has_nexthop = 1;
                }
                break;
            }
            case 10:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(name, text, sizeof(name));
                    has_name = 1;
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!has_name)
    {
        send_resp(msg, "Error: Missing batch name\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        /* 先删除 DB 记录 */
        route_db_delete_batch(g_route_local->dev_ipc_ctx, name);

        /* 向 worker 派发内存删除操作 */
        route_apply_cmd_t apply;
        memset(&apply, 0, sizeof(apply));
        apply.op = ROUTE_APPLY_BATCH_DEL;
        g_strlcpy(apply.u.batch_del.name, name, sizeof(apply.u.batch_del.name));
        route_worker_dispatch_apply(&apply);

        char buf[128];
        snprintf(buf, sizeof(buf), "Cleared %d batch route(s) for '%s'\r\n", apply.rc > 0 ? apply.rc : 0, name);
        send_resp(msg, buf);
        return ERRCODE_SUCCESS;
    }

    if (!has_start || !has_count || !has_nexthop)
    {
        send_resp(msg, "Error: Missing required parameters\r\n");
        return ERRCODE_FAIL;
    }

    uint16_t afi;
    uint8_t prefix_len;

    if (is_ipv4)
    {
        if (!has_ipv4_pfxlen)
        {
            send_resp(msg, "Error: IPv4 batch route requires prefix length\r\n");
            return ERRCODE_FAIL;
        }
        afi = ROUTE_AFI_IPV4;
        prefix_len = (uint8_t)ipv4_prefix_len;
    }
    else if (is_ipv6)
    {
        if (!has_pfxlen)
        {
            send_resp(msg, "Error: IPv6 batch route requires prefix length\r\n");
            return ERRCODE_FAIL;
        }
        afi = ROUTE_AFI_IPV6;
        prefix_len = (uint8_t)ipv6_prefix_len;
    }
    else
    {
        send_resp(msg, "Error: Must specify ipv4 or ipv6\r\n");
        return ERRCODE_FAIL;
    }

    /* 先写入 DB */
    route_db_upsert_batch(g_route_local->dev_ipc_ctx, name, afi, start_addr, prefix_len, count, nexthop);

    /* 向 worker 派发内存添加操作 */
    route_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = ROUTE_APPLY_BATCH_ADD;
    g_strlcpy(apply.u.batch_add.name, name, sizeof(apply.u.batch_add.name));
    apply.u.batch_add.afi = afi;
    apply.u.batch_add.prefix_len = prefix_len;
    g_strlcpy(apply.u.batch_add.start_addr, start_addr, sizeof(apply.u.batch_add.start_addr));
    apply.u.batch_add.count = count;
    g_strlcpy(apply.u.batch_add.nexthop, nexthop, sizeof(apply.u.batch_add.nexthop));
    route_worker_dispatch_apply(&apply);

    if (apply.rc < 0)
    {
        send_resp(msg, "Error: Invalid start address\r\n");
        return ERRCODE_FAIL;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "Added %d batch %s route(s) for '%s'\r\n", apply.rc, is_ipv4 ? "IPv4" : "IPv6", name);
    send_resp(msg, buf);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int route_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("[route_cli] payload 解析失败");
        send_resp(msg, "Error: Command payload parsing failed\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("[route_cli] 收到配置命令 group_id=%u", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case ROUTE_CLI_GROUP_ID_CONFIG:
            result = handle_route_config(msg, &parser);
            break;
        case ROUTE_CLI_GROUP_ID_BATCH:
            result = handle_route_batch(msg, &parser);
            break;
        default:
            LOG_WARN("[route_cli] 未知配置命令 group_id: %u", parser.group_id);
            send_resp(msg, "Error: Unknown command group\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
