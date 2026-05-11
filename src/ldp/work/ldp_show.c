/**
 * @file   ldp_show.c
 * @brief  LDP show 命令实现
 *
 * 在 worker 线程内消费内存态，输出文本响应。M2 支持：
 *   show ldp protocol  → 全局参数 + 计数
 *   show ldp interface → 每个使能接口的运行时信息
 *   show ldp neighbor  → 邻接列表
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_show.h"

#include <inttypes.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "ldp_cli.h"
#include "ldp_lib.h"
#include "ldp_main.h"
#include "ldp_session.h"
#include "ldp_worker.h"
#include "log.h"

static cli_chunk_stream_t g_ldp_show_stream;

static void respond(dev_ipc_message_t *req, GString *full)
{
    (void)cli_chunk_stream_start(&g_ldp_show_stream, ldp_local_ipc_ctx(), DEV_MODULE_ID_LDP, req, full);
}

static void respond_continue(dev_ipc_message_t *req)
{
    (void)cli_chunk_stream_continue(&g_ldp_show_stream, ldp_local_ipc_ctx(), DEV_MODULE_ID_LDP, req);
}

static void show_protocol(GString *out)
{
    if (!g_ldp_work_local)
    {
        g_string_append(out, "LDP not running\r\n");
        return;
    }
    char lsr[16];
    ldp_worker_format_lsr_id(g_ldp_work_local->proto.lsr_id, lsr, sizeof(lsr));
    g_string_append_printf(out, "LDP Protocol\r\n");
    g_string_append_printf(out, "  Admin           : %s\r\n", g_ldp_work_local->proto.admin_up ? "up" : "down");
    g_string_append_printf(out, "  LSR-ID          : %s\r\n", lsr);
    g_string_append_printf(out, "  Hello interval  : %u ms\r\n", g_ldp_work_local->proto.hello_interval_ms);
    g_string_append_printf(out, "  Hold time       : %u ms\r\n", g_ldp_work_local->proto.hold_time_ms);
    g_string_append_printf(out, "  Keepalive       : %u ms\r\n", g_ldp_work_local->proto.keepalive_ms);
    g_string_append_printf(out, "  Interfaces      : %u\r\n",
                           g_ldp_work_local->interfaces ? g_hash_table_size(g_ldp_work_local->interfaces) : 0u);
    g_string_append_printf(out, "  Adjacencies     : %u\r\n",
                           g_ldp_work_local->adjacencies ? g_hash_table_size(g_ldp_work_local->adjacencies) : 0u);
    g_string_append_printf(out, "  Sessions        : %u\r\n",
                           g_ldp_work_local->peers ? g_hash_table_size(g_ldp_work_local->peers) : 0u);
}

static void show_interface(GString *out)
{
    if (!g_ldp_work_local || !g_ldp_work_local->interfaces || g_hash_table_size(g_ldp_work_local->interfaces) == 0u)
    {
        g_string_append(out, "No LDP-enabled interface\r\n");
        return;
    }
    g_string_append(out, "Interface  IfIdx  Local-IP        State        Hello(ms)  Hold(ms)\r\n");
    g_string_append(out, "---------- ------ --------------- ------------ ---------- ---------\r\n");

    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->interfaces);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        const ldp_iface_state_t *iface = (const ldp_iface_state_t *)val;
        if (!iface)
        {
            continue;
        }
        char ip[16] = "-";
        if (iface->ipv4_local != 0u)
        {
            ldp_worker_format_lsr_id(iface->ipv4_local, ip, sizeof(ip));
        }
        const char *state = "init";
        if (!iface->enabled)
        {
            state = "disabled";
        }
        else if (iface->udp_fd < 0)
        {
            state = "no-socket";
        }
        else if (!iface->link_up)
        {
            state = "link-down";
        }
        else
        {
            state = "up";
        }
        g_string_append_printf(out, "%-10s %-6u %-15s %-12s %-10u %-9u\r\n", iface->ifname, iface->ifindex, ip, state,
                               ldp_worker_effective_hello_ms(iface), ldp_worker_effective_hold_ms(iface));
    }
}

static void show_neighbor(GString *out)
{
    if (!g_ldp_work_local || !g_ldp_work_local->adjacencies || g_hash_table_size(g_ldp_work_local->adjacencies) == 0u)
    {
        g_string_append(out, "No LDP adjacency\r\n");
        return;
    }
    g_string_append(out, "Peer-LSR-ID    Lsp Interface  Transport       Session       Hold(ms)\r\n");
    g_string_append(out, "-------------- --- ---------- --------------- ------------- ---------\r\n");

    GHashTableIter it;
    gpointer key = NULL, val = NULL;
    g_hash_table_iter_init(&it, g_ldp_work_local->adjacencies);
    while (g_hash_table_iter_next(&it, &key, &val))
    {
        const ldp_adjacency_t *adj = (const ldp_adjacency_t *)val;
        if (!adj)
        {
            continue;
        }
        char lsr[16];
        ldp_worker_format_lsr_id(adj->peer_lsr_id, lsr, sizeof(lsr));
        char xport[16] = "-";
        if (adj->peer_transport_v4 != 0u)
        {
            ldp_worker_format_lsr_id(adj->peer_transport_v4, xport, sizeof(xport));
        }
        const char *sess = "NON_EXIST";
        ldp_peer_t *peer = ldp_session_lookup(adj->peer_lsr_id, adj->peer_label_space);
        if (peer)
        {
            sess = ldp_session_state_str(peer->state);
        }
        g_string_append_printf(out, "%-14s %-3u %-10s %-15s %-13s %-9u\r\n", lsr, adj->peer_label_space, adj->ifname,
                               xport, sess, adj->neg_hold_ms);
    }
}

int ldp_show_handle_msg(dev_ipc_message_t *msg)
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
        respond(msg, g_string_new("LDP Error: Invalid show payload\r\n"));
        dev_ipc_message_free(msg);
        return ERRCODE_FAIL;
    }

    int show_proto = 0;
    int show_if = 0;
    int show_nbr = 0;
    int show_bind = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(&parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry))
        {
            switch (entry.cfg_id)
            {
                case 1:
                    show_proto = 1;
                    break;
                case 2:
                    show_if = 1;
                    break;
                case 3:
                    show_nbr = 1;
                    break;
                case 4:
                    show_bind = 1;
                    break;
                default:
                    break;
            }
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_cleanup(&parser);

    GString *out = g_string_new("");
    if (show_proto)
    {
        show_protocol(out);
    }
    else if (show_if)
    {
        show_interface(out);
    }
    else if (show_nbr)
    {
        show_neighbor(out);
    }
    else if (show_bind)
    {
        GHashTable *local = ldp_lib_local_table();
        GHashTable *remote = ldp_lib_remote_table();
        if ((!local || g_hash_table_size(local) == 0u) && (!remote || g_hash_table_size(remote) == 0u))
        {
            g_string_append(out, "No LDP label binding\r\n");
        }
        else
        {
            g_string_append(out, "Local Label Information Base\r\n");
            g_string_append(out, "Prefix              Label\r\n");
            g_string_append(out, "------------------- ---------\r\n");
            if (local)
            {
                GHashTableIter it;
                gpointer k = NULL, v = NULL;
                g_hash_table_iter_init(&it, local);
                while (g_hash_table_iter_next(&it, &k, &v))
                {
                    const ldp_local_label_t *e = (const ldp_local_label_t *)v;
                    char pfx[16];
                    ldp_worker_format_lsr_id(e->fec.prefix, pfx, sizeof(pfx));
                    g_string_append_printf(out, "%s/%-16u %u\r\n", pfx, e->fec.prefix_len, e->label);
                }
            }

            g_string_append(out, "\r\nRemote Label Information Base\r\n");
            g_string_append(out, "Peer-LSR-ID    Prefix              Label\r\n");
            g_string_append(out, "-------------- ------------------- ---------\r\n");
            if (remote)
            {
                GHashTableIter it;
                gpointer k = NULL, v = NULL;
                g_hash_table_iter_init(&it, remote);
                while (g_hash_table_iter_next(&it, &k, &v))
                {
                    const ldp_remote_label_t *e = (const ldp_remote_label_t *)v;
                    char peer[16], pfx[16];
                    ldp_worker_format_lsr_id(e->peer_lsr_id, peer, sizeof(peer));
                    ldp_worker_format_lsr_id(e->fec.prefix, pfx, sizeof(pfx));
                    g_string_append_printf(out, "%-14s %s/%-16u %u\r\n", peer, pfx, e->fec.prefix_len, e->label);
                }
            }
        }
    }
    else
    {
        g_string_append(out, "LDP: unknown show command\r\n");
    }

    respond(msg, out);
    dev_ipc_message_free(msg);
    return ERRCODE_SUCCESS;
}

void ldp_show_cleanup(void)
{
    cli_chunk_stream_reset(&g_ldp_show_stream);
}
