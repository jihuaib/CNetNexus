/**
 * @file   lldp_show.c
 * @brief  LLDP show 命令实现
 * @author jhb
 * @date   2026/06/07
 */
#include "lldp_show.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cli.h"
#include "errcode.h"
#include "lldp_cli.h"
#include "lldp_main.h"
#include "lldp_worker.h"

static cli_chunk_stream_t g_lldp_show_stream;

static void respond(dev_ipc_message_t *req, GString *full)
{
    (void)cli_chunk_stream_start(&g_lldp_show_stream, lldp_local_ipc_ctx(), DEV_MODULE_ID_LLDP, req, full);
}

static void respond_continue(dev_ipc_message_t *req)
{
    (void)cli_chunk_stream_continue(&g_lldp_show_stream, lldp_local_ipc_ctx(), DEV_MODULE_ID_LLDP, req);
}

static const char *admin_status_str(uint8_t s)
{
    switch (s)
    {
        case LLDP_IF_ADMIN_RX_ONLY:
            return "rxonly";
        case LLDP_IF_ADMIN_TX_ONLY:
            return "txonly";
        case LLDP_IF_ADMIN_TX_RX:
            return "txrx";
        case LLDP_IF_ADMIN_DISABLED:
        default:
            return "disabled";
    }
}

static void show_summary(GString *out)
{
    if (!g_lldp_work_local)
    {
        g_string_append(out, "LLDP not running\r\n");
        return;
    }
    lldp_worker_lock();
    g_string_append(out, "LLDP Protocol\r\n");
    g_string_append_printf(out, "  Admin          : %s\r\n", g_lldp_work_local->proto.admin_up ? "up" : "down");
    g_string_append_printf(out, "  Tx interval    : %u sec\r\n", g_lldp_work_local->proto.tx_interval_sec);
    g_string_append_printf(out, "  Hold multiplier: %u\r\n", g_lldp_work_local->proto.hold_multiplier);
    g_string_append_printf(out, "  Interfaces     : %u\r\n",
                           g_lldp_work_local->interfaces ? g_hash_table_size(g_lldp_work_local->interfaces) : 0u);
    g_string_append_printf(out, "  Neighbors      : %u\r\n",
                           g_lldp_work_local->neighbors ? g_hash_table_size(g_lldp_work_local->neighbors) : 0u);
    lldp_worker_unlock();
}

static void show_interface(GString *out)
{
    if (!g_lldp_work_local || !g_lldp_work_local->interfaces || g_hash_table_size(g_lldp_work_local->interfaces) == 0u)
    {
        g_string_append(out, "No LLDP-enabled interface\r\n");
        return;
    }

#define LLDP_IF_FMT "%-14s  %-6s  %-10s  %-10s  %-10s  %s\r\n"
    lldp_worker_lock();
    g_string_append_printf(out, LLDP_IF_FMT, "Interface", "IfIdx", "Link", "Admin", "Enabled", "Port-Desc");
    g_string_append_printf(out, LLDP_IF_FMT, "--------------", "------", "----------", "----------", "----------",
                           "---------");

    GHashTableIter it;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&it, g_lldp_work_local->interfaces);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        const lldp_iface_state_t *iface = (const lldp_iface_state_t *)val;
        char ifidx[16];
        snprintf(ifidx, sizeof(ifidx), "%u", iface->ifindex);
        g_string_append_printf(out, LLDP_IF_FMT, iface->ifname, ifidx, iface->link_up ? "up" : "down",
                               admin_status_str(iface->admin_status), iface->enabled ? "yes" : "no",
                               iface->port_desc[0] ? iface->port_desc : "-");
    }
    lldp_worker_unlock();
#undef LLDP_IF_FMT
}

static void format_id(const uint8_t *data, uint16_t len, char *out, size_t out_len)
{
    if (!out || out_len == 0u)
    {
        return;
    }
    out[0] = '\0';
    if (!data || len == 0u)
    {
        g_strlcpy(out, "-", out_len);
        return;
    }

    int printable = 1;
    for (uint16_t i = 0; i < len; i++)
    {
        if (data[i] < 32u || data[i] > 126u)
        {
            printable = 0;
            break;
        }
    }
    if (printable)
    {
        size_t n = len < out_len - 1u ? len : out_len - 1u;
        memcpy(out, data, n);
        out[n] = '\0';
        return;
    }

    size_t pos = 0u;
    for (uint16_t i = 0; i < len && pos + 3u < out_len; i++)
    {
        int n = snprintf(out + pos, out_len - pos, "%s%02x", i == 0u ? "" : ":", data[i]);
        if (n <= 0)
        {
            break;
        }
        pos += (size_t)n;
    }
}

static uint32_t remaining_ttl_sec(const lldp_neighbor_t *n)
{
    if (!n)
    {
        return 0u;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
    if (n->expire_msec <= now)
    {
        return 0u;
    }
    return (uint32_t)((n->expire_msec - now + 999u) / 1000u);
}

static uint32_t age_sec(const lldp_neighbor_t *n)
{
    if (!n)
    {
        return 0u;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
    if (now <= n->last_seen_msec)
    {
        return 0u;
    }
    return (uint32_t)((now - n->last_seen_msec) / 1000u);
}

static void show_neighbors(GString *out)
{
    if (!g_lldp_work_local || !g_lldp_work_local->neighbors || g_hash_table_size(g_lldp_work_local->neighbors) == 0u)
    {
        g_string_append(out, "No LLDP neighbor\r\n");
        return;
    }

#define LLDP_NBR_FMT "%-14s  %-24s  %-24s  %-24s  %-5s\r\n"
    lldp_worker_lock();
    g_string_append_printf(out, LLDP_NBR_FMT, "Interface", "System", "Chassis-ID", "Port-ID", "TTL");
    g_string_append_printf(out, LLDP_NBR_FMT, "--------------", "------------------------", "------------------------",
                           "------------------------", "-----");

    GHashTableIter it;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&it, g_lldp_work_local->neighbors);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        const lldp_neighbor_t *n = (const lldp_neighbor_t *)val;
        char chassis[80];
        char port[80];
        char ttl[16];
        format_id(n->chassis_id, n->chassis_len, chassis, sizeof(chassis));
        format_id(n->port_id, n->port_len, port, sizeof(port));
        snprintf(ttl, sizeof(ttl), "%u", remaining_ttl_sec(n));
        g_string_append_printf(out, LLDP_NBR_FMT, n->ifname, n->system_name[0] ? n->system_name : "-", chassis, port,
                               ttl);
    }
    lldp_worker_unlock();
#undef LLDP_NBR_FMT
}

static void show_neighbors_detail(GString *out)
{
    if (!g_lldp_work_local || !g_lldp_work_local->neighbors || g_hash_table_size(g_lldp_work_local->neighbors) == 0u)
    {
        g_string_append(out, "No LLDP neighbor\r\n");
        return;
    }

    lldp_worker_lock();
    GHashTableIter it;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&it, g_lldp_work_local->neighbors);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        const lldp_neighbor_t *n = (const lldp_neighbor_t *)val;
        char chassis[160];
        char port[160];
        format_id(n->chassis_id, n->chassis_len, chassis, sizeof(chassis));
        format_id(n->port_id, n->port_len, port, sizeof(port));

        g_string_append_printf(out, "Interface: %s\r\n", n->ifname);
        g_string_append_printf(out, "  System name       : %s\r\n", n->system_name[0] ? n->system_name : "-");
        g_string_append_printf(out, "  Chassis ID        : %s (subtype %u)\r\n", chassis, n->chassis_subtype);
        g_string_append_printf(out, "  Port ID           : %s (subtype %u)\r\n", port, n->port_subtype);
        g_string_append_printf(out, "  Port description  : %s\r\n", n->port_desc[0] ? n->port_desc : "-");
        g_string_append_printf(out, "  System description: %s\r\n", n->system_desc[0] ? n->system_desc : "-");
        g_string_append_printf(out, "  Capabilities      : supported 0x%04x, enabled 0x%04x\r\n", n->caps_supported,
                               n->caps_enabled);
        g_string_append_printf(out, "  TTL               : %u sec remaining, %u sec age\r\n", remaining_ttl_sec(n),
                               age_sec(n));
        g_string_append(out, "\r\n");
    }
    lldp_worker_unlock();
}

static void show_statistics(GString *out)
{
    if (!g_lldp_work_local)
    {
        g_string_append(out, "LLDP not running\r\n");
        return;
    }

    lldp_worker_lock();
    const lldp_stats_t *s = &g_lldp_work_local->stats;
    g_string_append(out, "LLDP Statistics\r\n");
    g_string_append_printf(out, "  TX frames         : %" G_GUINT64_FORMAT "\r\n", (guint64)s->tx_frames);
    g_string_append_printf(out, "  TX errors         : %" G_GUINT64_FORMAT "\r\n", (guint64)s->tx_errors);
    g_string_append_printf(out, "  RX frames         : %" G_GUINT64_FORMAT "\r\n", (guint64)s->rx_frames);
    g_string_append_printf(out, "  RX drops          : %" G_GUINT64_FORMAT "\r\n", (guint64)s->rx_drops);
    g_string_append_printf(out, "  RX parse errors   : %" G_GUINT64_FORMAT "\r\n", (guint64)s->rx_parse_errors);
    g_string_append_printf(out, "  Neighbor updates  : %" G_GUINT64_FORMAT "\r\n", (guint64)s->neighbor_updates);
    g_string_append_printf(out, "  Neighbor deletes  : %" G_GUINT64_FORMAT "\r\n", (guint64)s->neighbor_deletes);
    g_string_append_printf(out, "  Neighbor expires  : %" G_GUINT64_FORMAT "\r\n", (guint64)s->neighbor_expires);
    lldp_worker_unlock();
}

int lldp_show_handle_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        respond_continue(msg);
        dev_ipc_message_free(msg);
        return ERRCODE_SUCCESS;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        respond(msg, g_string_new("LLDP Error: Invalid show payload\r\n"));
        dev_ipc_message_free(msg);
        return ERRCODE_FAIL;
    }

    int show_if = 0;
    int want_neighbors = 0;
    int want_detail = 0;
    int want_statistics = 0;
    cli_tlv_entry_t entry;
    while (cli_tlv_next(&parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == 1)
            {
                show_if = 1;
            }
            else if (entry.cfg_id == 2)
            {
                want_neighbors = 1;
            }
            else if (entry.cfg_id == 3)
            {
                want_detail = 1;
            }
            else if (entry.cfg_id == 4)
            {
                want_statistics = 1;
            }
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_cleanup(&parser);

    GString *out = g_string_new("");
    if (show_if)
    {
        show_interface(out);
    }
    else if (want_statistics)
    {
        show_statistics(out);
    }
    else if (want_neighbors && want_detail)
    {
        show_neighbors_detail(out);
    }
    else if (want_neighbors)
    {
        show_neighbors(out);
    }
    else
    {
        show_summary(out);
    }

    respond(msg, out);
    dev_ipc_message_free(msg);
    return ERRCODE_SUCCESS;
}
