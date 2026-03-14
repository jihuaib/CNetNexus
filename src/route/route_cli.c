/**
 * @file   route_cli.c
 * @brief  Route 模块 CLI 命令处理（RIB + DB + 批量路由 + subscribe通知）
 * @author jhb
 * @date   2026/02/01
 */
#include "route_cli.h"

#include <arpa/inet.h>
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
#include "route_pub.h"
#include "route_rib.h"

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

static int route_send_chunked_response(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_route_local->show_stream, g_route_local->dev_ipc_ctx, DEV_MODULE_ID_ROUTE, msg,
                                  full_text);
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
    /* 检查是否为连续掩码 */
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

/**
 * @brief 将 IPv4 前缀长度转换为掩码字符串
 * @return 成功返回 0，失败返回 -1
 */
static int prefix_len_to_mask_str(uint8_t prefix_len, char *mask, size_t mask_len)
{
    if (!mask || mask_len == 0 || prefix_len > 32)
    {
        return -1;
    }

    uint32_t value = (prefix_len == 0) ? 0 : (0xFFFFFFFFu << (32 - prefix_len));
    struct in_addr addr = {.s_addr = htonl(value)};
    return (inet_ntop(AF_INET, &addr, mask, (socklen_t)mask_len) != NULL) ? 0 : -1;
}

// ============================================================================
// 删除通知上下文
// ============================================================================

typedef struct
{
    dev_ipc_context_t *ctx;
    GList *subscribers;
} del_notify_ctx_t;

/**
 * @brief route_rib_del 回调：在删除路径前向subscribe者发送撤销通知
 */
static void on_path_del(const route_head_t *head, const route_path_t *path, void *userdata)
{
    del_notify_ctx_t *ctx = (del_notify_ctx_t *)userdata;
    route_pub_notify(ctx->ctx, ctx->subscribers, head, path, 1);
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
    gboolean is_no = FALSE;
    int is_ipv4 = 0, is_ipv6 = 0;
    char prefix[ROUTE_RIB_PREFIX_MAX] = {0};
    char mask[64] = {0};
    int64_t ipv6_prefix_len = 0;
    char nexthop[ROUTE_RIB_NEXTHOP_MAX] = {0};
    int64_t metric = 0;
    int has_prefix = 0, has_mask = 0, has_pfxlen = 0, has_nexthop = 0, has_metric = 0;

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
                is_no = TRUE;
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
                    g_strlcpy(prefix, text, sizeof(prefix));
                    has_prefix = 1;
                }
                break;
            }
            case 5:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(mask, text, sizeof(mask));
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
                    g_strlcpy(nexthop, text, sizeof(nexthop));
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
        int pl = mask_to_prefix_len(mask);
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

    del_notify_ctx_t notify_ctx = {g_route_local->dev_ipc_ctx, g_route_local->subscribers};

    if (is_no)
    {
        if (has_nexthop)
        {
            /* 删除指定下一跳的路径 */
            int ret = route_rib_del(g_route_local->rib, ROUTE_VRF_DEFAULT, afi, prefix, prefix_len,
                                    ROUTE_PROTOCOL_STATIC, nexthop, on_path_del, &notify_ctx);

            /* 同步删除 DB */
            db_condition_t conds[5];
            uint32_t nc = 0;
            conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(ROUTE_VRF_DEFAULT)};
            conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
            conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix)};
            conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
            conds[nc++] = (db_condition_t){"nexthop", DB_CMP_EQ, db_value_text(nexthop)};
            db_filter_t filter = {conds, nc};
            db_rpc_delete(g_route_local->dev_ipc_ctx, "route_static", &filter);
            for (uint32_t i = 0; i < nc; i++)
            {
                db_value_free(&conds[i].value);
            }

            char buf[128];
            snprintf(buf, sizeof(buf), "Static route(s) deleted (%d)\r\n", ret > 0 ? ret : 0);
            send_resp(msg, buf);
        }
        else
        {
            /* 删除该前缀下所有静态路径 */
            int ret = route_rib_del_proto_for_prefix(g_route_local->rib, ROUTE_VRF_DEFAULT, afi, prefix, prefix_len,
                                                     ROUTE_PROTOCOL_STATIC, on_path_del, &notify_ctx);

            db_condition_t conds[4];
            uint32_t nc = 0;
            conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(ROUTE_VRF_DEFAULT)};
            conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
            conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix)};
            conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
            db_filter_t filter = {conds, nc};
            db_rpc_delete(g_route_local->dev_ipc_ctx, "route_static", &filter);
            for (uint32_t i = 0; i < nc; i++)
            {
                db_value_free(&conds[i].value);
            }

            char buf[128];
            snprintf(buf, sizeof(buf), "Static route(s) deleted (%d)\r\n", ret > 0 ? ret : 0);
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

        int32_t pref = ROUTE_ADMIN_DIST_STATIC;
        int32_t m = has_metric ? (int32_t)metric : 0;

        /* 更新 RIB */
        route_rib_add(g_route_local->rib, ROUTE_VRF_DEFAULT, afi, prefix, prefix_len, ROUTE_PROTOCOL_STATIC, nexthop,
                      nexthop, m, pref);

        /* 通知subscribe者 */
        const route_head_t *head =
            route_rib_lookup_head(g_route_local->rib, ROUTE_VRF_DEFAULT, afi, prefix, prefix_len);
        if (head)
        {
            const route_path_t *path = route_rib_lookup_path(head, ROUTE_PROTOCOL_STATIC, nexthop);
            if (path)
            {
                route_pub_notify(g_route_local->dev_ipc_ctx, g_route_local->subscribers, head, path, 0);
            }
        }

        /* 写入 DB（upsert：存在则更新） */
        db_record_t *rec = db_record_new();
        db_record_set_int(rec, "vrf_id", ROUTE_VRF_DEFAULT);
        db_record_set_int(rec, "afi", afi);
        db_record_set_text(rec, "prefix", prefix);
        db_record_set_int(rec, "prefix_len", prefix_len);
        db_record_set_text(rec, "nexthop", nexthop);
        db_record_set_int(rec, "metric", m);
        db_record_set_int(rec, "preference", pref);

        db_condition_t conds[5];
        uint32_t nc = 0;
        conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(ROUTE_VRF_DEFAULT)};
        conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
        conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix)};
        conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
        conds[nc++] = (db_condition_t){"nexthop", DB_CMP_EQ, db_value_text(nexthop)};
        db_filter_t filter = {conds, nc};

        db_rpc_upsert(g_route_local->dev_ipc_ctx, "route_static", rec, &filter);
        db_record_free(rec);
        for (uint32_t i = 0; i < nc; i++)
        {
            db_value_free(&conds[i].value);
        }

        send_resp(msg, "Static route added\r\n");
    }

    return ERRCODE_SUCCESS;
}

// ============================================================================
// Group 2: show route 命令
//
// cfg-id 映射：
//   1=ipv4, 2=ipv6, 3=all, 4=dest_filter,
//   5=proto:static, 6=proto:bgp, 7=proto:ospf, 8=summary
// ============================================================================

typedef struct
{
    GString *buf;
    uint32_t count;
    const char *dest_filter;
} show_ctx_t;

static const char *proto_name(uint32_t protocol)
{
    switch (protocol)
    {
        case ROUTE_PROTOCOL_STATIC:
            return "S";
        case ROUTE_PROTOCOL_BGP:
            return "B";
        case ROUTE_PROTOCOL_OSPF:
            return "O";
        default:
            return "?";
    }
}

static void show_path_cb(const route_head_t *head, const route_path_t *path, void *userdata)
{
    show_ctx_t *ctx = (show_ctx_t *)userdata;
    if (!ctx || !ctx->buf)
    {
        return;
    }

    /* 目标前缀过滤 */
    if (ctx->dest_filter && ctx->dest_filter[0] != '\0' && strcmp(head->prefix, ctx->dest_filter) != 0)
    {
        return;
    }

    char prefix_str[80];
    snprintf(prefix_str, sizeof(prefix_str), "%s/%u", head->prefix, head->prefix_len);

    g_string_append_printf(ctx->buf, "%-2s %-22s %-20s %4d %4d\r\n", proto_name(path->protocol), prefix_str,
                           path->nexthop, path->metric, path->preference);
    ctx->count++;
}

static int handle_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int show_ipv4 = 0, show_ipv6 = 0;
    uint32_t proto_filter = ROUTE_PROTOCOL_MAX;
    int show_summary = 0;
    char dest_filter[ROUTE_RIB_PREFIX_MAX] = {0};

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
                show_ipv4 = 1;
                break;
            case 2:
                show_ipv6 = 1;
                break;
            case 3:
                show_ipv4 = 1;
                show_ipv6 = 1;
                break;
            case 4:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(dest_filter, text, sizeof(dest_filter));
                }
                break;
            }
            case 5:
                proto_filter = ROUTE_PROTOCOL_STATIC;
                break;
            case 6:
                proto_filter = ROUTE_PROTOCOL_BGP;
                break;
            case 7:
                proto_filter = ROUTE_PROTOCOL_OSPF;
                break;
            case 8:
                show_summary = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!show_ipv4 && !show_ipv6 && !show_summary)
    {
        show_ipv4 = 1;
        show_ipv6 = 1;
    }

    if (show_summary)
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "\r\nRoute Summary:\r\n"
                 "  Prefixes: %u\r\n"
                 "  Paths: %u\r\n\r\n",
                 route_rib_head_count(g_route_local->rib), route_rib_path_count(g_route_local->rib));
        send_resp(msg, buf);
        return ERRCODE_SUCCESS;
    }

    show_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = g_string_new("");
    if (!ctx.buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }
    ctx.dest_filter = dest_filter;

    g_string_append_printf(ctx.buf,
                           "\r\n%-2s %-22s %-20s %4s %4s\r\n"
                           "-- ---------------------- -------------------- ---- ----\r\n",
                           "P", "Prefix", "Nexthop", "Met", "Pref");

    if (show_ipv4)
    {
        route_rib_walk(g_route_local->rib, proto_filter, ROUTE_VRF_DEFAULT, show_path_cb, &ctx);
    }

    if (show_ipv6 && !show_ipv4)
    {
        /* 单独 IPv6 遍历：通过检查 afi 过滤（walk 没有 afi 过滤参数，依赖 cb 内部过滤） */
        route_rib_walk(g_route_local->rib, proto_filter, ROUTE_VRF_DEFAULT, show_path_cb, &ctx);
    }

    if (ctx.count == 0)
    {
        g_string_append(ctx.buf, "  (no routes)\r\n");
    }

    g_string_append_printf(ctx.buf, "\r\nTotal %u path(s)\r\n", ctx.count);
    return route_send_chunked_response(msg, ctx.buf);
}

// ============================================================================
// Group 3: 批量路由命令
//
// cfg-id 映射：
//   1=no, 2=batch, 3=ipv4, 4=ipv6,
//   5=start address, 6=mask(IPv4), 7=prefix_len(IPv6),
//   8=count value, 9=nexthop value
// ============================================================================

/**
 * @brief IPv6 地址字节数组递增（进位传播）
 */
static void ipv6_next_prefix(uint8_t *addr16, uint8_t prefix_len)
{
    /* 计算前缀末位字节的偏移 */
    int byte_idx = (prefix_len - 1) / 8;
    int bit_shift = 7 - ((prefix_len - 1) % 8);
    uint16_t carry = (uint16_t)(1 << bit_shift);

    for (int i = byte_idx; i >= 0 && carry; i--)
    {
        uint16_t sum = addr16[i] + carry;
        addr16[i] = (uint8_t)(sum & 0xFF);
        carry = sum >> 8;
    }
}

static int handle_route_batch(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = FALSE;
    int is_ipv4 = 0, is_ipv6 = 0;
    char start_addr[ROUTE_RIB_PREFIX_MAX] = {0};
    char mask[64] = {0};
    int64_t ipv6_prefix_len = 0;
    int64_t count = 0;
    char nexthop[ROUTE_RIB_NEXTHOP_MAX] = {0};
    int has_start = 0, has_mask = 0, has_pfxlen = 0, has_count = 0, has_nexthop = 0;

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
                is_no = TRUE;
                break;
            case 2:
                /* batch 关键字（无需处理） */
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
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    g_strlcpy(mask, text, sizeof(mask));
                    has_mask = 1;
                }
                break;
            }
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
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    /* no route batch：清除全部批量路由 */
    if (is_no)
    {
        del_notify_ctx_t notify_ctx = {g_route_local->dev_ipc_ctx, g_route_local->subscribers};
        int total = 0;

        for (GList *l = g_route_local->batch_entries; l; l = l->next)
        {
            route_batch_entry_t *be = (route_batch_entry_t *)l->data;
            int ret = route_rib_del(g_route_local->rib, be->vrf_id, be->afi, be->prefix, be->prefix_len,
                                    ROUTE_PROTOCOL_STATIC, be->nexthop, on_path_del, &notify_ctx);
            if (ret > 0)
            {
                total++;
            }
        }

        g_list_free_full(g_route_local->batch_entries, g_free);
        g_route_local->batch_entries = NULL;

        char buf[128];
        snprintf(buf, sizeof(buf), "Cleared %d batch route(s)\r\n", total);
        send_resp(msg, buf);
        return ERRCODE_SUCCESS;
    }

    /* 验证参数 */
    if (!has_start || !has_count || !has_nexthop)
    {
        send_resp(msg, "Error: Missing required parameters\r\n");
        return ERRCODE_FAIL;
    }

    if (is_ipv4)
    {
        if (!has_mask)
        {
            send_resp(msg, "Error: IPv4 batch route requires subnet mask\r\n");
            return ERRCODE_FAIL;
        }
        int pl = mask_to_prefix_len(mask);
        if (pl < 0)
        {
            send_resp(msg, "Error: Invalid subnet mask\r\n");
            return ERRCODE_FAIL;
        }
        uint8_t prefix_len = (uint8_t)pl;

        struct in_addr base;
        if (inet_pton(AF_INET, start_addr, &base) != 1)
        {
            send_resp(msg, "Error: Invalid start address\r\n");
            return ERRCODE_FAIL;
        }

        uint32_t step = (prefix_len < 32) ? (1u << (32 - prefix_len)) : 1u;
        uint32_t addr = ntohl(base.s_addr);
        int added = 0;

        for (int64_t i = 0; i < count; i++)
        {
            struct in_addr cur;
            cur.s_addr = htonl(addr);
            char prefix_str[ROUTE_RIB_PREFIX_MAX];
            inet_ntop(AF_INET, &cur, prefix_str, sizeof(prefix_str));

            route_rib_add(g_route_local->rib, ROUTE_VRF_DEFAULT, ROUTE_AFI_IPV4, prefix_str, prefix_len,
                          ROUTE_PROTOCOL_STATIC, nexthop, nexthop, 0, ROUTE_ADMIN_DIST_STATIC);

            /* 通知subscribe者 */
            const route_head_t *head =
                route_rib_lookup_head(g_route_local->rib, ROUTE_VRF_DEFAULT, ROUTE_AFI_IPV4, prefix_str, prefix_len);
            if (head)
            {
                const route_path_t *path = route_rib_lookup_path(head, ROUTE_PROTOCOL_STATIC, nexthop);
                if (path)
                {
                    route_pub_notify(g_route_local->dev_ipc_ctx, g_route_local->subscribers, head, path, 0);
                }
            }

            /* 记录批量条目 */
            route_batch_entry_t *be = (route_batch_entry_t *)g_malloc0(sizeof(route_batch_entry_t));
            be->vrf_id = ROUTE_VRF_DEFAULT;
            be->afi = ROUTE_AFI_IPV4;
            be->prefix_len = prefix_len;
            g_strlcpy(be->prefix, prefix_str, sizeof(be->prefix));
            g_strlcpy(be->nexthop, nexthop, sizeof(be->nexthop));
            g_route_local->batch_entries = g_list_append(g_route_local->batch_entries, be);

            addr += step;
            added++;
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "Added %d batch IPv4 route(s)\r\n", added);
        send_resp(msg, buf);
    }
    else if (is_ipv6)
    {
        if (!has_pfxlen)
        {
            send_resp(msg, "Error: IPv6 batch route requires prefix length\r\n");
            return ERRCODE_FAIL;
        }
        uint8_t prefix_len = (uint8_t)ipv6_prefix_len;

        uint8_t addr16[16];
        if (inet_pton(AF_INET6, start_addr, addr16) != 1)
        {
            send_resp(msg, "Error: Invalid IPv6 start address\r\n");
            return ERRCODE_FAIL;
        }

        int added = 0;
        for (int64_t i = 0; i < count; i++)
        {
            char prefix_str[ROUTE_RIB_PREFIX_MAX];
            struct in6_addr cur;
            memcpy(cur.s6_addr, addr16, 16);
            inet_ntop(AF_INET6, &cur, prefix_str, sizeof(prefix_str));

            route_rib_add(g_route_local->rib, ROUTE_VRF_DEFAULT, ROUTE_AFI_IPV6, prefix_str, prefix_len,
                          ROUTE_PROTOCOL_STATIC, nexthop, nexthop, 0, ROUTE_ADMIN_DIST_STATIC);

            /* 通知subscribe者 */
            const route_head_t *head =
                route_rib_lookup_head(g_route_local->rib, ROUTE_VRF_DEFAULT, ROUTE_AFI_IPV6, prefix_str, prefix_len);
            if (head)
            {
                const route_path_t *path = route_rib_lookup_path(head, ROUTE_PROTOCOL_STATIC, nexthop);
                if (path)
                {
                    route_pub_notify(g_route_local->dev_ipc_ctx, g_route_local->subscribers, head, path, 0);
                }
            }

            /* 记录批量条目 */
            route_batch_entry_t *be = (route_batch_entry_t *)g_malloc0(sizeof(route_batch_entry_t));
            be->vrf_id = ROUTE_VRF_DEFAULT;
            be->afi = ROUTE_AFI_IPV6;
            be->prefix_len = prefix_len;
            g_strlcpy(be->prefix, prefix_str, sizeof(be->prefix));
            g_strlcpy(be->nexthop, nexthop, sizeof(be->nexthop));
            g_route_local->batch_entries = g_list_append(g_route_local->batch_entries, be);

            ipv6_next_prefix(addr16, prefix_len);
            added++;
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "Added %d batch IPv6 route(s)\r\n", added);
        send_resp(msg, buf);
    }
    else
    {
        send_resp(msg, "Error: Must specify ipv4 or ipv6\r\n");
        return ERRCODE_FAIL;
    }

    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int route_cli_handle_show_config(dev_ipc_message_t *msg)
{
    GString *out = g_string_new("");
    if (!out)
    {
        return route_send_chunked_response(msg, NULL);
    }

    db_result_t *result = NULL;
    int ret = db_rpc_query(g_route_local->dev_ipc_ctx, "route_static", NULL, 0, NULL, &result);
    if (ret != ERRCODE_SUCCESS || !result || result->num_rows == 0)
    {
        if (result)
        {
            db_result_free(result);
        }
        return route_send_chunked_response(msg, out);
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
            char mask[32];
            if (prefix_len_to_mask_str((uint8_t)prefix_len, mask, sizeof(mask)) != 0)
            {
                continue;
            }

            g_string_append(out, "!\r\n");
            g_string_append_printf(out, "route ipv4 %s %s %s", prefix, mask, nexthop);
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
    return route_send_chunked_response(msg, out);
}

int route_cli_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_route_local->show_stream, g_route_local->dev_ipc_ctx, DEV_MODULE_ID_ROUTE, msg);
}

void route_cli_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_route_local->show_stream);
}

int route_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    /* 新命令到来时，清理上次可能残留的分片状态 */
    cli_chunk_stream_reset(&g_route_local->show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("Payload parsing failed");
        send_resp(msg, "Error: Command payload parsing failed\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("Received TLV payload (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case ROUTE_CLI_GROUP_ID_CONFIG:
            result = handle_route_config(msg, &parser);
            break;
        case ROUTE_CLI_GROUP_ID_SHOW:
            result = handle_show_route(msg, &parser);
            break;
        case ROUTE_CLI_GROUP_ID_BATCH:
            result = handle_route_batch(msg, &parser);
            break;
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            send_resp(msg, "Error: Unknown command group\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
