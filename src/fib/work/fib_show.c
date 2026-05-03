#include "fib_show.h"

#include <glib.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "fib_main.h"
#include "fib_os.h"
#include "fib_worker.h"
#include "log.h"
#include "net_addr.h"
#include "route.h"

static cli_chunk_stream_t g_fib_show_stream;

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_FIB, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(fib_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static int send_chunked(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_fib_show_stream, fib_local_ipc_ctx(), DEV_MODULE_ID_FIB, msg, full_text);
}

typedef struct fib_show_route_ctx
{
    GString *buf;
    uint16_t afi;
    gboolean has_filter;
    net_addr_t filter_addr;
    uint8_t filter_prefix_len;
    uint32_t count;
} fib_show_route_ctx_t;

static const char *afi_name(uint16_t afi)
{
    switch (afi)
    {
        case ROUTE_AFI_IPV4:
            return "ipv4";
        case ROUTE_AFI_IPV6:
            return "ipv6";
        default:
            return "unknown";
    }
}

static const char *nh_type_name(uint8_t nh_type)
{
    switch (nh_type)
    {
        case FIB_NH_TYPE_IP:
            return "ip";
        case FIB_NH_TYPE_TUNNEL:
            return "tunnel";
        case FIB_NH_TYPE_BLACKHOLE:
            return "blackhole";
        default:
            return "unknown";
    }
}

static void prefix_to_str(const fib_route_entry_t *route, char *buf, size_t sz)
{
    char addr[64] = "-";
    if (!route || !buf || sz == 0)
    {
        return;
    }
    net_addr_to_str(&route->prefix_addr, addr, sizeof(addr));
    snprintf(buf, sz, "%s/%u", addr, route->prefix_len);
}

static void nexthop_to_str(const fib_route_entry_t *route, char *buf, size_t sz)
{
    if (!route || !buf || sz == 0)
    {
        return;
    }

    if (route->nh_type == FIB_NH_TYPE_TUNNEL)
    {
        snprintf(buf, sz, "tunnel:%u", route->tunnel_id);
        return;
    }
    if (route->nh_type == FIB_NH_TYPE_BLACKHOLE)
    {
        snprintf(buf, sz, "blackhole");
        return;
    }
    if (route->nexthop_addr.family == AF_INET || route->nexthop_addr.family == AF_INET6)
    {
        net_addr_to_str(&route->nexthop_addr, buf, sz);
        if (!net_addr_is_zero(&route->nexthop_addr))
        {
            return;
        }
    }
    snprintf(buf, sz, "-");
}

static void labels_to_str(uint8_t count, const uint32_t *labels, char *buf, size_t sz)
{
    if (!buf || sz == 0)
    {
        return;
    }
    buf[0] = '\0';
    if (!labels || count == 0)
    {
        snprintf(buf, sz, "-");
        return;
    }

    gsize used = 0;
    for (uint8_t i = 0; i < count; i++)
    {
        int n = snprintf(buf + used, sz - used, "%s%u", (i == 0) ? "" : ",", labels[i]);
        if (n < 0 || (size_t)n >= sz - used)
        {
            buf[sz - 1] = '\0';
            return;
        }
        used += (gsize)n;
    }
}

static void append_route_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    fib_route_state_t *state = (fib_route_state_t *)value;
    fib_show_route_ctx_t *ctx = (fib_show_route_ctx_t *)user_data;
    if (!state || !ctx || !ctx->buf || state->entry.afi != ctx->afi)
    {
        return;
    }
    if (ctx->has_filter && (state->entry.prefix_len != ctx->filter_prefix_len ||
                            !net_addr_equal(&state->entry.prefix_addr, &ctx->filter_addr)))
    {
        return;
    }

    char prefix[96] = "-";
    char nexthop[64] = "-";
    prefix_to_str(&state->entry, prefix, sizeof(prefix));
    nexthop_to_str(&state->entry, nexthop, sizeof(nexthop));

    g_string_append_printf(ctx->buf, "%-7s %-26s %-20s %-10s %-8u %-8d %-8d %-9s %-7s\r\n", afi_name(state->entry.afi),
                           prefix, nexthop, nh_type_name(state->entry.nh_type), state->entry.out_ifindex,
                           state->entry.metric, state->entry.preference, state->installed ? "yes" : "no",
                           (state->entry.flags & FIB_ROUTE_FLAG_SKIP_OS) ? "yes" : "no");
    ctx->count++;
}

static void append_route_detail_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    fib_route_state_t *state = (fib_route_state_t *)value;
    fib_show_route_ctx_t *ctx = (fib_show_route_ctx_t *)user_data;
    if (!state || !ctx || !ctx->buf || state->entry.afi != ctx->afi)
    {
        return;
    }
    if (ctx->has_filter && (state->entry.prefix_len != ctx->filter_prefix_len ||
                            !net_addr_equal(&state->entry.prefix_addr, &ctx->filter_addr)))
    {
        return;
    }

    char prefix[96] = "-";
    char nexthop[64] = "-";
    prefix_to_str(&state->entry, prefix, sizeof(prefix));
    nexthop_to_str(&state->entry, nexthop, sizeof(nexthop));

    ctx->count++;
    g_string_append_printf(ctx->buf,
                           "\r\nFIB Route Detail: %s\r\n"
                           "  AFI       : %s\r\n"
                           "  Protocol  : %u\r\n"
                           "  Nexthop   : %s\r\n"
                           "  NH-Type   : %s\r\n"
                           "  Tunnel-ID : %u\r\n"
                           "  OIF       : %u\r\n"
                           "  Metric    : %d\r\n"
                           "  Preference: %d\r\n"
                           "  Installed : %s\r\n"
                           "  Skip OS   : %s\r\n",
                           prefix, afi_name(state->entry.afi), state->entry.protocol, nexthop,
                           nh_type_name(state->entry.nh_type), state->entry.tunnel_id, state->entry.out_ifindex,
                           state->entry.metric, state->entry.preference, state->installed ? "yes" : "no",
                           (state->entry.flags & FIB_ROUTE_FLAG_SKIP_OS) ? "yes" : "no");

    if (state->entry.nh_type == FIB_NH_TYPE_TUNNEL && state->entry.tunnel_id != 0u)
    {
        fib_tunnel_state_t *tun =
            fib_rib_tunnel_lookup(g_fib_work_local ? g_fib_work_local->rib : NULL, state->entry.tunnel_id);
        if (tun)
        {
            char relay[64] = "-";
            char labels[128] = "-";
            if (tun->entry.relay_addr.family == AF_INET || tun->entry.relay_addr.family == AF_INET6)
            {
                net_addr_to_str(&tun->entry.relay_addr, relay, sizeof(relay));
            }
            labels_to_str(tun->entry.label_count, tun->entry.labels, labels, sizeof(labels));
            g_string_append_printf(ctx->buf, "  Tunnel    : state=%s relay=%s oif=%u labels=[%s]\r\n",
                                   tun->entry.state ? "up" : "down", relay, tun->entry.out_ifindex, labels);
        }
        else
        {
            g_string_append(ctx->buf, "  Tunnel    : pending\r\n");
        }
    }
}

static int handle_show_routes(dev_ipc_message_t *msg, uint16_t afi, const net_addr_t *filter_addr,
                              int64_t filter_prefix_len)
{
    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    fib_show_route_ctx_t ctx = {
        .buf = buf,
        .afi = afi,
        .has_filter = filter_addr != NULL,
        .filter_prefix_len = filter_addr ? (uint8_t)filter_prefix_len : 0u,
        .count = 0,
    };
    if (filter_addr)
    {
        ctx.filter_addr = *filter_addr;
    }

    if (filter_addr)
    {
        fib_rib_foreach_route(g_fib_work_local ? g_fib_work_local->rib : NULL, append_route_detail_cb, &ctx);
    }
    else
    {
        g_string_append_printf(buf,
                               "\r\nFIB Routes (%s)\r\n"
                               "%-7s %-26s %-20s %-10s %-8s %-8s %-8s %-9s %-7s\r\n"
                               "------- -------------------------- -------------------- ---------- "
                               "-------- -------- -------- --------- -------\r\n",
                               afi_name(afi), "AFI", "Prefix", "Nexthop", "NH-Type", "OIF", "Metric", "Pref",
                               "Installed", "SkipOS");

        fib_rib_foreach_route(g_fib_work_local ? g_fib_work_local->rib : NULL, append_route_cb, &ctx);
    }
    if (ctx.count == 0)
    {
        g_string_append(buf, "  (no routes)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u route(s)\r\n", ctx.count);
    return send_chunked(msg, buf);
}

static int handle_show_os(dev_ipc_message_t *msg, uint16_t afi)
{
    GString *buf = g_string_new("");
    if (!buf)
    {
        send_resp(msg, "Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    sa_family_t family = (afi == ROUTE_AFI_IPV6) ? AF_INET6 : AF_INET;
    if (fib_os_show(buf, family) != ERRCODE_SUCCESS)
    {
        g_string_free(buf, TRUE);
        send_resp(msg, "Error: 读取内核路由表失败\r\n");
        return ERRCODE_FAIL;
    }

    return send_chunked(msg, buf);
}

static int handle_show_fib(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    int show_ipv4 = 0;
    int show_ipv6 = 0;
    int show_os = 0;
    net_addr_t filter_addr;
    gboolean has_filter_addr = FALSE;
    int64_t filter_prefix_len = -1;
    gboolean has_filter_prefix_len = FALSE;
    cli_tlv_entry_t entry;

    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry))
        {
            switch (entry.cfg_id)
            {
                case 1:
                    show_ipv4 = 1;
                    break;
                case 2:
                    show_ipv6 = 1;
                    break;
                case 3:
                    show_os = 1;
                    break;
                case 4:
                {
                    const char *text = cli_tlv_entry_get_text(&entry);
                    if (text && net_addr_from_str(text, &filter_addr) == 0)
                    {
                        has_filter_addr = TRUE;
                    }
                    break;
                }
                case 5:
                case 6:
                    filter_prefix_len = cli_tlv_entry_get_int(&entry);
                    has_filter_prefix_len = TRUE;
                    break;
                default:
                    break;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    uint16_t afi = show_ipv6 ? ROUTE_AFI_IPV6 : ROUTE_AFI_IPV4;
    if (!show_ipv4 && !show_ipv6)
    {
        afi = ROUTE_AFI_IPV4;
    }

    if (has_filter_addr != has_filter_prefix_len)
    {
        send_resp(msg, "Error: prefix query must include prefix-length\r\n");
        return ERRCODE_FAIL;
    }
    if (show_os && has_filter_addr)
    {
        send_resp(msg, "Error: OS FIB query does not support prefix filter\r\n");
        return ERRCODE_FAIL;
    }
    if (has_filter_addr)
    {
        if (afi == ROUTE_AFI_IPV4 && (filter_addr.family != AF_INET || filter_prefix_len < 0 || filter_prefix_len > 32))
        {
            send_resp(msg, "Error: invalid IPv4 prefix/prefix-length\r\n");
            return ERRCODE_FAIL;
        }
        if (afi == ROUTE_AFI_IPV6 &&
            (filter_addr.family != AF_INET6 || filter_prefix_len < 0 || filter_prefix_len > 128))
        {
            send_resp(msg, "Error: invalid IPv6 prefix/prefix-length\r\n");
            return ERRCODE_FAIL;
        }
        (void)net_addr_prefix_normalize(&filter_addr, (uint8_t)filter_prefix_len);
    }

    return show_os ? handle_show_os(msg, afi)
                   : handle_show_routes(msg, afi, has_filter_addr ? &filter_addr : NULL, filter_prefix_len);
}

int fib_show_dispatch(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        return cli_chunk_stream_continue(&g_fib_show_stream, fib_local_ipc_ctx(), DEV_MODULE_ID_FIB, msg);
    }

    if (!msg->payload)
    {
        return ERRCODE_FAIL;
    }

    fib_show_cleanup();

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("FIB: show payload parse failed");
        return ERRCODE_FAIL;
    }

    int result = ERRCODE_FAIL;
    switch (parser.group_id)
    {
        case FIB_CLI_GROUP_ID_SHOW:
            result = handle_show_fib(msg, &parser);
            break;
        default:
            LOG_WARN("FIB: unknown show group_id=%u", parser.group_id);
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}

void fib_show_cleanup(void)
{
    cli_chunk_stream_reset(&g_fib_show_stream);
}
