/**
 * @file   route_show.c
 * @brief  Route 模块 show 命令处理（在内存 RIB 中查询，仅在 worker 线程访问）
 * @author jhb
 * @date   2026/03/28
 */
#include "route_show.h"

#include <arpa/inet.h>
#include <glib.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "if.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"
#include "route_cli.h"
#include "route_main.h"
#include "route_os.h"
#include "route_relay.h"
#include "route_rib.h"
#include "route_static.h"
#include "route_worker.h"

/** show 命令分片流状态，仅在 worker 线程访问 */
static cli_chunk_stream_t g_route_show_stream;

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
        dev_ipc_send_response(route_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

int route_show_send_chunked(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_route_show_stream, route_local_ipc_ctx(), DEV_MODULE_ID_ROUTE, msg, full_text);
}

// ============================================================================
// Group 2: show route 命令内部辅助类型与回调
//
// cfg-id 映射：
//   1=ipv4, 2=ipv6, 4=dest_filter,
//   5=proto:static, 6=proto:bgp, 7=proto:ospf, 8=summary,
//   9=proto:connected, 10=os, 11=proto:isis,
//   12=prefix_len_v4, 13=prefix_len_v6
// ============================================================================

typedef struct
{
    GString *buf;
    uint32_t count;
    int show_ipv4; /**< 是否显示 IPv4 路由 */
    int show_ipv6; /**< 是否显示 IPv6 路由 */
} show_ctx_t;

typedef struct
{
    uint32_t connected; /**< 直连路由条数 */
    uint32_t static_;   /**< 静态路由条数 */
    uint32_t bgp;       /**< BGP 路由条数 */
    uint32_t ospf;      /**< OSPF 路由条数 */
    uint32_t isis;      /**< ISIS 路由条数 */
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
        case ROUTE_PROTOCOL_ISIS:
            ctx->isis++;
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
        case ROUTE_PROTOCOL_ISIS:
            return "I";
        case ROUTE_PROTOCOL_BLACKHOLE:
            return "S";
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
        case ROUTE_PROTOCOL_ISIS:
            return "isis";
        case ROUTE_PROTOCOL_BLACKHOLE:
            return "static(blackhole)";
        default:
            return "unknown";
    }
}

/* 将接口索引转换为逻辑名字符串，写入 buf（长度至少 IF_NAMESIZE）
 * 优先使用 Route 本地缓存中的逻辑名；不可用时回退到 OS 物理名。 */
static void ifindex_to_name(uint32_t ifindex, char *buf)
{
    if (ifindex == 0)
    {
        g_strlcpy(buf, "-", IF_NAMESIZE);
        return;
    }
    const char *logical = if_api_cache_get_logical_name(ifindex);
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
    if (path->key.protocol == ROUTE_PROTOCOL_BLACKHOLE)
    {
        g_strlcpy(oif_str, "Null0", IF_NAMESIZE);
    }
    else
    {
        ifindex_to_name(path->out_ifindex, oif_str);
    }

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
    uint8_t dest_prefix_len;
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
    if (head->key.prefix_len != ctx->dest_prefix_len)
    {
        return;
    }

    char addr_str[64], nh_str[64], iter_nh_str[64], oif_str[IF_NAMESIZE], iter_oif_str[IF_NAMESIZE];
    net_addr_to_str(&head->key.addr, addr_str, sizeof(addr_str));
    net_addr_to_str(&path->nexthop, nh_str, sizeof(nh_str));
    net_addr_to_str(&path->relay_addr, iter_nh_str, sizeof(iter_nh_str));
    if (path->key.protocol == ROUTE_PROTOCOL_BLACKHOLE)
    {
        g_strlcpy(oif_str, "Null0", IF_NAMESIZE);
        g_strlcpy(iter_oif_str, "Null0", IF_NAMESIZE);
    }
    else
    {
        ifindex_to_name(path->out_ifindex, oif_str);
        ifindex_to_name(path->iter_out_ifindex, iter_oif_str);
    }

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
                           "    Updated   : %s\r\n",
                           ctx->count, proto_name_long(path->key.protocol), nh_str, oif_str, iter_nh_str, iter_oif_str,
                           (unsigned int)path->flags, path->metric, path->preference, time_str);
}

// ============================================================================
// 公共接口实现
// ============================================================================

int route_show_handle_route(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int show_ipv4 = 0, show_ipv6 = 0;
    uint32_t proto_filter = ROUTE_PROTOCOL_MAX;
    int show_summary = 0;
    int show_os = 0;
    net_addr_t dest_filter_addr;
    gboolean has_dest_filter = FALSE;
    int64_t dest_filter_pfxlen = -1;
    gboolean has_dest_filter_pfxlen = FALSE;

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
            case 11:
                proto_filter = ROUTE_PROTOCOL_ISIS;
                break;
            case 12:
            case 13:
                dest_filter_pfxlen = cli_tlv_entry_get_int(&entry);
                has_dest_filter_pfxlen = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    if (has_dest_filter != has_dest_filter_pfxlen)
    {
        send_resp(msg, "Error: destination query must include prefix-length\r\n");
        return ERRCODE_FAIL;
    }
    if (has_dest_filter)
    {
        if (show_ipv4 && !show_ipv6)
        {
            if (dest_filter_addr.family != AF_INET || dest_filter_pfxlen < 0 || dest_filter_pfxlen > 32)
            {
                send_resp(msg, "Error: invalid IPv4 destination/prefix-length\r\n");
                return ERRCODE_FAIL;
            }
        }
        else if (show_ipv6 && !show_ipv4)
        {
            if (dest_filter_addr.family != AF_INET6 || dest_filter_pfxlen < 0 || dest_filter_pfxlen > 128)
            {
                send_resp(msg, "Error: invalid IPv6 destination/prefix-length\r\n");
                return ERRCODE_FAIL;
            }
        }
        else if (dest_filter_pfxlen < 0 || (dest_filter_addr.family == AF_INET && dest_filter_pfxlen > 32) ||
                 (dest_filter_addr.family == AF_INET6 && dest_filter_pfxlen > 128))
        {
            send_resp(msg, "Error: invalid destination/prefix-length\r\n");
            return ERRCODE_FAIL;
        }
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
        return route_show_send_chunked(msg, buf);
    }

    if (show_summary)
    {
        summary_ctx_t sctx;
        memset(&sctx, 0, sizeof(sctx));
        sctx.show_ipv4 = show_ipv4;
        sctx.show_ipv6 = show_ipv6;
        route_rib_walk(g_route_work_local->rib, ROUTE_PROTOCOL_MAX, ROUTE_VRF_DEFAULT, summary_path_cb, &sctx);
        uint32_t total = sctx.connected + sctx.static_ + sctx.bgp + sctx.ospf + sctx.isis + sctx.other;
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "\r\nRoute Summary:\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n"
                 "  %-12s %u\r\n\r\n",
                 "Connected:", sctx.connected, "Static:", sctx.static_, "BGP:", sctx.bgp, "OSPF:", sctx.ospf,
                 "ISIS:", sctx.isis, "Other:", sctx.other, "Total:", total);
        send_resp(msg, buf);
        return ERRCODE_SUCCESS;
    }

    /* 指定目标地址时显示详情，否则显示汇总表格 */
    if (has_dest_filter)
    {
        detail_ctx_t dctx;
        memset(&dctx, 0, sizeof(dctx));
        dctx.buf = g_string_new("");
        if (!dctx.buf)
        {
            send_resp(msg, "Error: Out of memory\r\n");
            return ERRCODE_FAIL;
        }
        dctx.show_ipv4 = show_ipv4;
        dctx.show_ipv6 = show_ipv6;
        dctx.dest_filter = dest_filter_addr;
        dctx.dest_prefix_len = (uint8_t)dest_filter_pfxlen;

        char filter_str[64];
        net_addr_to_str(&dest_filter_addr, filter_str, sizeof(filter_str));
        g_string_append_printf(dctx.buf, "\r\nRouting entry for %s/%u\r\n", filter_str, dctx.dest_prefix_len);

        route_rib_walk(g_route_work_local->rib, proto_filter, ROUTE_VRF_DEFAULT, detail_path_cb, &dctx);

        if (dctx.count == 0)
        {
            g_string_append(dctx.buf, "  (no matching routes)\r\n");
        }
        else
        {
            g_string_append_printf(dctx.buf, "\r\nTotal %u path(s)\r\n", dctx.count);
        }
        return route_show_send_chunked(msg, dctx.buf);
    }

    show_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = g_string_new("");
    if (!ctx.buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }
    ctx.show_ipv4 = show_ipv4;
    ctx.show_ipv6 = show_ipv6;

    g_string_append_printf(ctx.buf,
                           "\r\n%-2s %-24s %-20s %-14s %4s %4s\r\n"
                           "-- ------------------------ -------------------- -------------- ---- ----\r\n",
                           "P", "Prefix", "Nexthop", "Interface", "Met", "Pref");

    route_rib_walk(g_route_work_local->rib, proto_filter, ROUTE_VRF_DEFAULT, show_path_cb, &ctx);

    if (ctx.count == 0)
    {
        g_string_append(ctx.buf, "  (no routes)\r\n");
    }

    g_string_append_printf(ctx.buf, "\r\nTotal %u path(s)\r\n", ctx.count);
    return route_show_send_chunked(msg, ctx.buf);
}

int route_show_handle_relay(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
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
    return route_show_send_chunked(msg, buf);
}

int route_show_handle_static(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
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
    return route_show_send_chunked(msg, buf);
}

int route_show_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_route_show_stream, route_local_ipc_ctx(), DEV_MODULE_ID_ROUTE, msg);
}

void route_show_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_route_show_stream);
}

int route_show_dispatch(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    /* 分片继续请求：不能重置流状态，也不需要解析 payload。 */
    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        return route_show_handle_continue(msg);
    }

    if (!msg->payload)
    {
        return ERRCODE_FAIL;
    }

    /* 新 show 命令到来时清理上次残留的分片状态 */
    route_show_cleanup_state();

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("[route_show] payload 解析失败");
        return ERRCODE_FAIL;
    }

    int result;
    switch (parser.group_id)
    {
        case ROUTE_CLI_GROUP_ID_SHOW:
            result = route_show_handle_route(msg, &parser);
            break;
        case ROUTE_CLI_GROUP_ID_RELAY_SHOW:
            result = route_show_handle_relay(msg, &parser);
            break;
        case ROUTE_CLI_GROUP_ID_STATIC_SHOW:
            result = route_show_handle_static(msg, &parser);
            break;
        default:
            LOG_WARN("[route_show] 未知 show group_id: %u", parser.group_id);
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
