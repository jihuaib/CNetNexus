/**
 * @file   route_cli.c
 * @brief  Route 模块 CLI 命令处理（RIB + DB + 批量路由 + subscribe通知）
 * @author jhb
 * @date   2026/02/01
 */
#include "route_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if_event.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_main.h"
#include "route_os.h"
#include "route_pub.h"
#include "route_relay.h"
#include "route_rib.h"
#include "route_static.h"

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
    /* 字符串仅用于 DB 操作（DB 边界保持字符串格式） */
    char prefix_str[64] = {0};
    char mask_str[64] = {0};
    char nexthop_str[64] = {0};
    int64_t ipv6_prefix_len = 0;
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

    /* 将 CLI 字符串转为二进制地址（内存操作边界） */
    net_addr_t prefix_addr;
    if (net_addr_from_str(prefix_str, &prefix_addr) != 0)
    {
        send_resp(msg, "Error: Invalid prefix address\r\n");
        return ERRCODE_FAIL;
    }

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

            /* 从候选表删除（同步撤销 RIB + 通知订阅者） */
            int ret = route_static_del(ROUTE_VRF_DEFAULT, afi, &prefix_addr, prefix_len, &nexthop_addr);

            /* 同步删除 DB（DB 边界仍用字符串） */
            db_condition_t conds[5];
            uint32_t nc = 0;
            conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(ROUTE_VRF_DEFAULT)};
            conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
            conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix_str)};
            conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
            conds[nc++] = (db_condition_t){"nexthop", DB_CMP_EQ, db_value_text(nexthop_str)};
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
            /* 删除该前缀下所有候选静态路由（同步撤销 RIB + 通知订阅者） */
            int ret = route_static_del_prefix(ROUTE_VRF_DEFAULT, afi, &prefix_addr, prefix_len);

            db_condition_t conds[4];
            uint32_t nc = 0;
            conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(ROUTE_VRF_DEFAULT)};
            conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
            conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix_str)};
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

        net_addr_t nexthop_addr;
        if (net_addr_from_str(nexthop_str, &nexthop_addr) != 0)
        {
            send_resp(msg, "Error: Invalid nexthop address\r\n");
            return ERRCODE_FAIL;
        }

        int32_t pref = ROUTE_ADMIN_DIST_STATIC;
        int32_t m = has_metric ? (int32_t)metric : 0;

        /* 写入候选表（nexthop 可达时自动写 RIB + 通知订阅者） */
        route_static_add(ROUTE_VRF_DEFAULT, afi, &prefix_addr, prefix_len, &nexthop_addr, m, pref);

        /* 写入 DB（upsert：存在则更新；DB 边界仍用字符串） */
        db_record_t *rec = db_record_new();
        db_record_set_int(rec, "vrf_id", ROUTE_VRF_DEFAULT);
        db_record_set_int(rec, "afi", afi);
        db_record_set_text(rec, "prefix", prefix_str);
        db_record_set_int(rec, "prefix_len", prefix_len);
        db_record_set_text(rec, "nexthop", nexthop_str);
        db_record_set_int(rec, "metric", m);
        db_record_set_int(rec, "preference", pref);

        db_condition_t conds[5];
        uint32_t nc = 0;
        conds[nc++] = (db_condition_t){"vrf_id", DB_CMP_EQ, db_value_int(ROUTE_VRF_DEFAULT)};
        conds[nc++] = (db_condition_t){"afi", DB_CMP_EQ, db_value_int(afi)};
        conds[nc++] = (db_condition_t){"prefix", DB_CMP_EQ, db_value_text(prefix_str)};
        conds[nc++] = (db_condition_t){"prefix_len", DB_CMP_EQ, db_value_int(prefix_len)};
        conds[nc++] = (db_condition_t){"nexthop", DB_CMP_EQ, db_value_text(nexthop_str)};
        db_filter_t filter = {conds, nc};

        db_rpc_upsert(g_route_local->dev_ipc_ctx, "route_static", rec, &filter);
        db_record_free(rec);
        for (uint32_t i = 0; i < nc; i++)
        {
            db_value_free(&conds[i].value);
        }

        send_resp(msg, "Static route added\r\n");
    }

    route_recompute_iter_paths();
    return ERRCODE_SUCCESS;
}

// ============================================================================
// Group 2: show route 命令
//
// cfg-id 映射：
//   1=ipv4, 2=ipv6, 4=dest_filter,
//   5=proto:static, 6=proto:bgp, 7=proto:ospf, 8=summary,
//   9=proto:connected, 10=os
// ============================================================================

typedef struct
{
    GString *buf;
    uint32_t count;
    int show_ipv4;                      /**< 是否显示 IPv4 路由 */
    int show_ipv6;                      /**< 是否显示 IPv6 路由 */
    const if_intf_map_resp_t *intf_map; /**< IF 模块接口映射（用于逻辑名显示，可为 NULL） */
} show_ctx_t;

typedef struct
{
    uint32_t connected; /**< 直连路由条数 */
    uint32_t static_;   /**< 静态路由条数 */
    uint32_t bgp;       /**< BGP 路由条数 */
    uint32_t ospf;      /**< OSPF 路由条数 */
    uint32_t other;     /**< 其他协议路由条数 */
    int show_ipv4;
    int show_ipv6;
} summary_ctx_t;

static void summary_path_cb(const route_head_t *head, const route_path_t *path, void *userdata)
{
    summary_ctx_t *ctx = (summary_ctx_t *)userdata;
    sa_family_t family = head->key.addr.family;
    if (!ctx->show_ipv4 && family == AF_INET)
    {
        return;
    }
    if (!ctx->show_ipv6 && family == AF_INET6)
    {
        return;
    }
    switch (path->key.protocol)
    {
        case ROUTE_PROTOCOL_CONNECTED:
            ctx->connected++;
            break;
        case ROUTE_PROTOCOL_STATIC:
            ctx->static_++;
            break;
        case ROUTE_PROTOCOL_BGP:
            ctx->bgp++;
            break;
        case ROUTE_PROTOCOL_OSPF:
            ctx->ospf++;
            break;
        default:
            ctx->other++;
            break;
    }
}

static const char *proto_name(uint32_t protocol)
{
    switch (protocol)
    {
        case ROUTE_PROTOCOL_CONNECTED:
            return "C";
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

static const char *proto_name_long(uint32_t protocol)
{
    switch (protocol)
    {
        case ROUTE_PROTOCOL_CONNECTED:
            return "connected";
        case ROUTE_PROTOCOL_STATIC:
            return "static";
        case ROUTE_PROTOCOL_BGP:
            return "bgp";
        case ROUTE_PROTOCOL_OSPF:
            return "ospf";
        default:
            return "unknown";
    }
}

static const char *nh_state_str(uint8_t state)
{
    switch (state)
    {
        case ROUTE_NH_STATE_RESOLVED:
            return "resolved";
        case ROUTE_NH_STATE_UNRESOLVED:
            return "unresolved";
        default:
            return "unknown";
    }
}

/* 将接口索引转换为逻辑名字符串，写入 buf（长度至少 IF_NAMESIZE）
 * 优先使用 intf_map 中的逻辑名；不可用时回退到 OS 物理名 */
static void ifindex_to_name(uint32_t ifindex, const if_intf_map_resp_t *intf_map, char *buf)
{
    if (ifindex == 0)
    {
        g_strlcpy(buf, "-", IF_NAMESIZE);
        return;
    }
    const char *logical = if_intf_map_lookup(intf_map, ifindex);
    if (logical)
    {
        g_strlcpy(buf, logical, IF_NAMESIZE);
        return;
    }
    if (if_indextoname(ifindex, buf) == NULL)
    {
        g_strlcpy(buf, "-", IF_NAMESIZE);
    }
}

static void show_path_cb(const route_head_t *head, const route_path_t *path, void *userdata)
{
    show_ctx_t *ctx = (show_ctx_t *)userdata;
    if (!ctx || !ctx->buf)
    {
        return;
    }

    /* AFI 过滤 */
    sa_family_t family = head->key.addr.family;
    if (!ctx->show_ipv4 && family == AF_INET)
    {
        return;
    }
    if (!ctx->show_ipv6 && family == AF_INET6)
    {
        return;
    }

    char addr_str[64], nh_str[64], prefix_str[80], oif_str[IF_NAMESIZE];
    net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
    net_addr_to_str(&path->nexthop, nh_str, sizeof(nh_str));
    snprintf(prefix_str, sizeof(prefix_str), "%s/%u", addr_str, head->key.prefix_len);
    ifindex_to_name(path->out_ifindex, ctx->intf_map, oif_str);

    g_string_append_printf(ctx->buf, "%-2s %-24s %-20s %-14s %4d %4d\r\n", proto_name(path->key.protocol), prefix_str,
                           nh_str, oif_str, path->metric, path->preference);
    ctx->count++;
}

/* 详情视图回调：每条路径显示完整块 */
typedef struct
{
    GString *buf;
    uint32_t count;
    int show_ipv4;
    int show_ipv6;
    net_addr_t dest_filter;
    const if_intf_map_resp_t *intf_map; /**< IF 模块接口映射（用于逻辑名显示，可为 NULL） */
} detail_ctx_t;

static void detail_path_cb(const route_head_t *head, const route_path_t *path, void *userdata)
{
    detail_ctx_t *ctx = (detail_ctx_t *)userdata;

    sa_family_t family = head->key.addr.family;
    if (!ctx->show_ipv4 && family == AF_INET)
    {
        return;
    }
    if (!ctx->show_ipv6 && family == AF_INET6)
    {
        return;
    }
    if (!net_addr_equal(&head->key.addr, &ctx->dest_filter))
    {
        return;
    }

    char addr_str[64], nh_str[64], iter_nh_str[64], oif_str[IF_NAMESIZE];
    net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
    net_addr_to_str(&path->nexthop, nh_str, sizeof(nh_str));
    net_addr_to_str(&path->os_nexthop, iter_nh_str, sizeof(iter_nh_str));
    ifindex_to_name(path->out_ifindex, ctx->intf_map, oif_str);

    /* 格式化更新时间 */
    time_t sec = (time_t)(path->updated_at_usec / G_USEC_PER_SEC);
    struct tm tm_buf;
    char time_str[32] = "-";
    struct tm *tm = localtime_r(&sec, &tm_buf);
    if (tm)
    {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
    }

    ctx->count++;
    g_string_append_printf(ctx->buf,
                           "  Path [%u]: %s\r\n"
                           "    Nexthop   : %s\r\n"
                           "    Interface : %s\r\n"
                           "    Iter NH   : %s\r\n"
                           "    Iter OIF  : %s\r\n"
                           "    Flags     : 0x%08X\r\n"
                           "    Metric    : %d\r\n"
                           "    Preference: %d\r\n"
                           "    NH State  : %s\r\n"
                           "    Updated   : %s\r\n",
                           ctx->count, proto_name_long(path->key.protocol), nh_str, oif_str, iter_nh_str, oif_str,
                           (unsigned int)path->flags, path->metric, path->preference, nh_state_str(path->nh_state),
                           time_str);
}

static int handle_show_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int show_ipv4 = 0, show_ipv6 = 0;
    uint32_t proto_filter = ROUTE_PROTOCOL_MAX;
    int show_summary = 0;
    int show_os = 0;
    net_addr_t dest_filter_addr;
    gboolean has_dest_filter = FALSE;

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
            case 4:
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text && net_addr_from_str(text, &dest_filter_addr) == 0)
                {
                    has_dest_filter = TRUE;
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
            case 9:
                proto_filter = ROUTE_PROTOCOL_CONNECTED;
                break;
            case 10:
                show_os = 1;
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

    if (show_os)
    {
        GString *buf = g_string_new("");
        if (!buf)
        {
            send_resp(msg, "Error: Out of memory\r\n");
            return ERRCODE_FAIL;
        }
        sa_family_t family = show_ipv6 ? AF_INET6 : AF_INET;
        if (route_os_show(buf, family) != 0)
        {
            g_string_free(buf, TRUE);
            send_resp(msg, "Error: 读取内核路由表失败\r\n");
            return ERRCODE_FAIL;
        }
        return route_send_chunked_response(msg, buf);
    }

    if (show_summary)
    {
        summary_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.show_ipv4 = show_ipv4;
        sctx.show_ipv6 = show_ipv6;
        route_rib_walk(g_route_local->rib, ROUTE_PROTOCOL_MAX, ROUTE_VRF_DEFAULT, summary_path_cb, &sctx);
        uint32_t total = sctx.connected + sctx.static_ + sctx.bgp + sctx.ospf + sctx.other;
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "\r\nRoute Summary:\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n\r\n",
                 "Connected:", sctx.connected, "Static:", sctx.static_, "BGP:", sctx.bgp, "OSPF:", sctx.ospf,
                 "Other:", sctx.other, "Total:", total);
        send_resp(msg, buf);
        return ERRCODE_SUCCESS;
    }

    /* 向 IF 模块查询接口映射表，用于在 show 时显示逻辑接口名 */
    if_intf_map_resp_t *intf_map = if_rpc_get_intf_map(g_route_local->dev_ipc_ctx);

    /* 指定目标地址时显示详情，否则显示汇总表格 */
    if (has_dest_filter)
    {
        detail_ctx_t dctx;
        memset(&dctx, 0, sizeof(dctx));
        dctx.buf = g_string_new("");
        if (!dctx.buf)
        {
            g_free(intf_map);
            send_resp(msg, "Error: Out of memory\r\n");
            return ERRCODE_FAIL;
        }
        dctx.show_ipv4 = show_ipv4;
        dctx.show_ipv6 = show_ipv6;
        dctx.dest_filter = dest_filter_addr;
        dctx.intf_map = intf_map;

        char filter_str[64];
        net_addr_to_str(&dest_filter_addr, filter_str, sizeof(filter_str));
        g_string_append_printf(dctx.buf, "\r\nRouting entry for %s\r\n", filter_str);

        route_rib_walk(g_route_local->rib, proto_filter, ROUTE_VRF_DEFAULT, detail_path_cb, &dctx);

        if (dctx.count == 0)
        {
            g_string_append(dctx.buf, "  (no matching routes)\r\n");
        }
        else
        {
            g_string_append_printf(dctx.buf, "\r\nTotal %u path(s)\r\n", dctx.count);
        }
        g_free(intf_map);
        return route_send_chunked_response(msg, dctx.buf);
    }

    show_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = g_string_new("");
    if (!ctx.buf)
    {
        g_free(intf_map);
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }
    ctx.show_ipv4 = show_ipv4;
    ctx.show_ipv6 = show_ipv6;
    ctx.intf_map = intf_map;

    g_string_append_printf(ctx.buf,
                           "\r\n%-2s %-24s %-20s %-14s %4s %4s\r\n"
                           "-- ------------------------ -------------------- -------------- ---- ----\r\n",
                           "P", "Prefix", "Nexthop", "Interface", "Met", "Pref");

    route_rib_walk(g_route_local->rib, proto_filter, ROUTE_VRF_DEFAULT, show_path_cb, &ctx);

    if (ctx.count == 0)
    {
        g_string_append(ctx.buf, "  (no routes)\r\n");
    }

    g_string_append_printf(ctx.buf, "\r\nTotal %u path(s)\r\n", ctx.count);
    g_free(intf_map);
    return route_send_chunked_response(msg, ctx.buf);
}

// ============================================================================
// Group 3: 批量路由命令
//
// cfg-id 映射：
//   1=no, 2=batch, 3=ipv4, 4=ipv6,
//   5=start_addr, 6=prefix_len(IPv4 0-32), 7=prefix_len(IPv6 0-128),
//   8=count_value, 9=nexthop_value, 10=name
// ============================================================================

/**
 * @brief IPv6 地址字节数组递增（进位传播）
 */
static void ipv6_next_prefix(uint8_t *addr16, uint8_t prefix_len)
{
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

/**
 * @brief 生成 batch 路由并加入 RIB 和 batch_entries
 *
 * @param name       batch 名称
 * @param afi        地址族（ROUTE_AFI_IPV4 或 ROUTE_AFI_IPV6）
 * @param start_addr 起始地址字符串
 * @param prefix_len 前缀长度
 * @param count      路由数量
 * @param nexthop    下一跳
 * @param do_notify  是否通知订阅者（当前 CLI/DB 恢复都传 TRUE；FALSE 仅保留给内部无需发布场景）
 * @return 成功添加数量，地址解析失败返回 -1
 */
static int batch_do_add(const char *name, uint16_t afi, const char *start_addr, uint8_t prefix_len, int64_t count,
                        const char *nexthop, gboolean do_notify)
{
    (void)do_notify; /* 统一走 route_static_add，由候选表负责迭代/RIB/OS 全流程 */
    int added = 0;

    /* 将下一跳字符串转为二进制（DB 边界进入内存之后不再用字符串） */
    net_addr_t nexthop_addr;
    if (net_addr_from_str(nexthop, &nexthop_addr) != 0)
    {
        return -1;
    }

    if (afi == ROUTE_AFI_IPV4)
    {
        struct in_addr base;
        if (inet_pton(AF_INET, start_addr, &base) != 1)
        {
            return -1;
        }

        uint32_t step = (prefix_len < 32) ? (1u << (32 - prefix_len)) : 1u;
        uint32_t addr = ntohl(base.s_addr);

        for (int64_t i = 0; i < count; i++)
        {
            struct in_addr cur;
            cur.s_addr = htonl(addr);

            net_addr_t prefix_addr;
            prefix_addr.family = AF_INET;
            prefix_addr.u.v4 = cur;

            /* 走静态候选表：nexthop 迭代、RIB 写入、OS 下发均在其内部完成 */
            route_static_add(ROUTE_VRF_DEFAULT, ROUTE_AFI_IPV4, &prefix_addr, prefix_len, &nexthop_addr, 0,
                             ROUTE_ADMIN_DIST_STATIC);

            route_batch_entry_t *be = (route_batch_entry_t *)g_malloc0(sizeof(route_batch_entry_t));
            g_strlcpy(be->name, name, sizeof(be->name));
            be->vrf_id = ROUTE_VRF_DEFAULT;
            be->afi = ROUTE_AFI_IPV4;
            be->prefix_len = prefix_len;
            be->prefix_addr = prefix_addr;
            be->nexthop_addr = nexthop_addr;
            g_route_local->batch_entries = g_list_append(g_route_local->batch_entries, be);

            addr += step;
            added++;
        }
    }
    else if (afi == ROUTE_AFI_IPV6)
    {
        uint8_t addr16[16];
        if (inet_pton(AF_INET6, start_addr, addr16) != 1)
        {
            return -1;
        }

        for (int64_t i = 0; i < count; i++)
        {
            struct in6_addr cur;
            memcpy(cur.s6_addr, addr16, 16);

            net_addr_t prefix_addr;
            prefix_addr.family = AF_INET6;
            prefix_addr.u.v6 = cur;

            /* 走静态候选表：nexthop 迭代、RIB 写入、OS 下发均在其内部完成 */
            route_static_add(ROUTE_VRF_DEFAULT, ROUTE_AFI_IPV6, &prefix_addr, prefix_len, &nexthop_addr, 0,
                             ROUTE_ADMIN_DIST_STATIC);

            route_batch_entry_t *be = (route_batch_entry_t *)g_malloc0(sizeof(route_batch_entry_t));
            g_strlcpy(be->name, name, sizeof(be->name));
            be->vrf_id = ROUTE_VRF_DEFAULT;
            be->afi = ROUTE_AFI_IPV6;
            be->prefix_len = prefix_len;
            be->prefix_addr = prefix_addr;
            be->nexthop_addr = nexthop_addr;
            g_route_local->batch_entries = g_list_append(g_route_local->batch_entries, be);

            ipv6_next_prefix(addr16, prefix_len);
            added++;
        }
    }

    return added;
}

static int handle_route_batch(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = FALSE;
    int is_ipv4 = 0, is_ipv6 = 0;
    char name[64] = {0};
    char start_addr[64] = {0};
    int64_t ipv4_prefix_len = 0;
    int64_t ipv6_prefix_len = 0;
    int64_t count = 0;
    char nexthop[64] = {0};
    int has_name = 0, has_start = 0, has_ipv4_pfxlen = 0, has_pfxlen = 0, has_count = 0, has_nexthop = 0;

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
                /* IPv4 前缀长度（数字，0-32） */
                ipv4_prefix_len = cli_tlv_entry_get_int(&entry);
                has_ipv4_pfxlen = 1;
                break;
            case 7:
                /* IPv6 前缀长度（数字，0-128） */
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

    /* no route batch <name>：按 name 删除该 batch 的所有路由 */
    if (is_no)
    {
        int total = 0;

        GList *l = g_route_local->batch_entries;
        while (l)
        {
            route_batch_entry_t *be = (route_batch_entry_t *)l->data;
            GList *next = l->next;
            if (strcmp(be->name, name) == 0)
            {
                /* 经静态候选表删除：自动处理 RIB 撤销、订阅者通知和 OS 撤销 */
                int ret = route_static_del(be->vrf_id, be->afi, &be->prefix_addr, be->prefix_len, &be->nexthop_addr);
                if (ret > 0)
                {
                    total++;
                }
                g_route_local->batch_entries = g_list_delete_link(g_route_local->batch_entries, l);
                g_free(be);
            }
            l = next;
        }

        /* 从 DB 删除 */
        db_condition_t cond = {"name", DB_CMP_EQ, db_value_text(name)};
        db_filter_t filter = {&cond, 1};
        db_rpc_delete(g_route_local->dev_ipc_ctx, "route_batch", &filter);
        db_value_free(&cond.value);

        char buf[128];
        snprintf(buf, sizeof(buf), "Cleared %d batch route(s) for '%s'\r\n", total, name);
        send_resp(msg, buf);
        route_recompute_iter_paths();
        return ERRCODE_SUCCESS;
    }

    /* 验证参数 */
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

    /* 若同名 batch 已存在，先清除其旧路由（DB 后续 upsert 覆盖） */
    GList *l = g_route_local->batch_entries;
    while (l)
    {
        route_batch_entry_t *be = (route_batch_entry_t *)l->data;
        GList *next = l->next;
        if (strcmp(be->name, name) == 0)
        {
            route_static_del(be->vrf_id, be->afi, &be->prefix_addr, be->prefix_len, &be->nexthop_addr);
            g_route_local->batch_entries = g_list_delete_link(g_route_local->batch_entries, l);
            g_free(be);
        }
        l = next;
    }

    /* 生成路由并写入 RIB */
    int added = batch_do_add(name, afi, start_addr, prefix_len, count, nexthop, TRUE);
    if (added < 0)
    {
        send_resp(msg, "Error: Invalid start address\r\n");
        return ERRCODE_FAIL;
    }

    /* 写入 DB（name 为主键，upsert 覆盖同名 batch） */
    db_record_t *rec = db_record_new();
    db_record_set_text(rec, "name", name);
    db_record_set_int(rec, "afi", afi);
    db_record_set_text(rec, "start_addr", start_addr);
    db_record_set_int(rec, "prefix_len", prefix_len);
    db_record_set_int(rec, "count", count);
    db_record_set_text(rec, "nexthop", nexthop);

    db_condition_t cond = {"name", DB_CMP_EQ, db_value_text(name)};
    db_filter_t filter = {&cond, 1};
    db_rpc_upsert(g_route_local->dev_ipc_ctx, "route_batch", rec, &filter);
    db_record_free(rec);
    db_value_free(&cond.value);

    char buf[128];
    snprintf(buf, sizeof(buf), "Added %d batch %s route(s) for '%s'\r\n", added, is_ipv4 ? "IPv4" : "IPv6", name);
    send_resp(msg, buf);
    route_recompute_iter_paths();
    return ERRCODE_SUCCESS;
}

void route_batch_restore_from_db(void)
{
    db_result_t *result = NULL;
    int ret = db_rpc_query(route_local_ipc_ctx(), "route_batch", NULL, 0, NULL, &result);
    if (ret != ERRCODE_SUCCESS || !result)
    {
        return;
    }

    uint32_t restored = 0;
    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *name = db_row_get_text(row, "name", NULL);
        int64_t afi = db_row_get_int(row, "afi", ROUTE_AFI_IPV4);
        const char *start_addr = db_row_get_text(row, "start_addr", NULL);
        int64_t prefix_len = db_row_get_int(row, "prefix_len", 0);
        int64_t count = db_row_get_int(row, "count", 0);
        const char *nexthop = db_row_get_text(row, "nexthop", NULL);

        if (!name || !start_addr || !nexthop || count <= 0)
        {
            continue;
        }

        int added = batch_do_add(name, (uint16_t)afi, start_addr, (uint8_t)prefix_len, count, nexthop, TRUE);
        if (added >= 0)
        {
            restored++;
        }
    }

    LOG_INFO("Restored %u batch route group(s) from DB", restored);
    route_recompute_iter_paths();
    db_result_free(result);
}

// ============================================================================
// Group 4: show route relay 命令
//
// cfg-id 映射：
//   1=bgp（按 BGP 模块过滤）
// ============================================================================

static int handle_show_relay(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t module_filter = 0;
    int has_filter = 0;
    int show_static = 0;
    uint16_t afi_filter = 0;
    int has_afi_filter = 0;

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
                /* ipv4 关键字 */
                afi_filter = ROUTE_AFI_IPV4;
                has_afi_filter = 1;
                break;
            case 2:
                /* ipv6 关键字 */
                afi_filter = ROUTE_AFI_IPV6;
                has_afi_filter = 1;
                break;
            case 3:
                /* bgp 关键字 */
                module_filter = DEV_MODULE_ID_BGP;
                has_filter = 1;
                break;
            case 4:
                /* static 关键字：显示候选静态路由的 nexthop 迭代状态 */
                show_static = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    if (show_static)
    {
        route_static_show_relay(buf, afi_filter, has_afi_filter);
    }
    else
    {
        route_relay_show(buf, module_filter, has_filter, afi_filter, has_afi_filter);
    }
    return route_send_chunked_response(msg, buf);
}

// ============================================================================
// Group 5: show route static 命令（候选静态路由表）
// ============================================================================

static int handle_show_static(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint16_t afi_filter = 0;
    int has_afi_filter = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry))
        {
            switch (entry.cfg_id)
            {
                case 1:
                    afi_filter = ROUTE_AFI_IPV4;
                    has_afi_filter = 1;
                    break;
                case 2:
                    afi_filter = ROUTE_AFI_IPV6;
                    has_afi_filter = 1;
                    break;
                default:
                    break;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    route_static_show(buf, afi_filter, has_afi_filter);
    return route_send_chunked_response(msg, buf);
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
        case ROUTE_CLI_GROUP_ID_RELAY_SHOW:
            result = handle_show_relay(msg, &parser);
            break;
        case ROUTE_CLI_GROUP_ID_STATIC_SHOW:
            result = handle_show_static(msg, &parser);
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
