/**
 * @file   if_show.c
 * @brief  IF 模块 show 命令处理（运行于 worker 线程）
 * @author jhb
 * @date   2026/04/21
 */
#include "if_show.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "if_cli.h"
#include "if_main.h"
#include "if_map.h"
#include "if_netlink.h"
#include "if_worker.h"
#include "log.h"
#include "net_addr.h"

/** loop 接口编号范围 */
#define IF_LOOP_ID_MIN 1U
#define IF_LOOP_ID_MAX 1024U

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(if_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static const char *if_cfgid_to_name(uint32_t cfg_id)
{
    switch (cfg_id)
    {
        case 0:
            return "null0";
        case 1:
            return "GE-1";
        case 2:
            return "GE-2";
        case 3:
            return "GE-3";
        case 4:
            return "GE-4";
        case 5:
            return "GE-5";
        case 6:
            return "GE-6";
        case 7:
            return "GE-7";
        case 8:
            return "GE-8";
        default:
            return NULL;
    }
}

static void show_format_prefix(const net_prefix_t *pfx, char *buf, size_t sz)
{
    if (!pfx || !buf || sz == 0)
    {
        return;
    }
    if (net_prefix_is_set(pfx))
    {
        net_prefix_to_str(pfx, buf, sz);
    }
    else
    {
        g_strlcpy(buf, "-", sz);
    }
}

static void show_format_dual_stack(const if_map_entry_t *e, char *buf, size_t sz)
{
    if (!e || !buf || sz == 0)
    {
        return;
    }
    char v4[70], v6[70];
    show_format_prefix(&e->prefix_v4, v4, sizeof(v4));
    show_format_prefix(&e->prefix_v6, v6, sizeof(v6));

    if (strcmp(v4, "-") != 0 && strcmp(v6, "-") != 0)
    {
        snprintf(buf, sz, "%s, %s", v4, v6);
    }
    else if (strcmp(v4, "-") != 0)
    {
        g_strlcpy(buf, v4, sz);
    }
    else if (strcmp(v6, "-") != 0)
    {
        g_strlcpy(buf, v6, sz);
    }
    else
    {
        g_strlcpy(buf, "-", sz);
    }
}

static void show_append_entry(GString *resp_buf, const if_map_entry_t *e)
{
    if_info_t info;
    gboolean is_null0 = (strcmp(e->logical_name, "null0") == 0);
    const char *link_str = "DOWN";
    gboolean link_up = FALSE;
    if (is_null0)
    {
        link_up = (e->link_up != 0);
        link_str = link_up ? "UP" : "DOWN";
    }
    else if (if_get_info(e->physical_name, &info) == ERRCODE_SUCCESS)
    {
        link_up = (info.state == IF_STATE_UP);
        link_str = link_up ? "UP" : "DOWN";
    }
    else if (e->link_up >= 0)
    {
        link_up = (e->link_up != 0);
        link_str = link_up ? "UP" : "DOWN";
    }

    gboolean has_ip = net_prefix_is_set(&e->prefix_v4) || net_prefix_is_set(&e->prefix_v6);
    gboolean proto_up = (!e->shutdown && link_up && (is_null0 || has_ip));
    const char *proto_str = proto_up ? "UP" : "DOWN";

    char ip_str[160];
    show_format_dual_stack(e, ip_str, sizeof(ip_str));
    g_string_append_printf(resp_buf, "%-14s %-6s %-6s %-48s\r\n", e->logical_name, proto_str, link_str, ip_str);
}

static gboolean show_foreach_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    show_append_entry((GString *)user_data, (const if_map_entry_t *)value);
    return FALSE;
}

static void show_single_entry(GString *resp_buf, const char *ifname, const if_map_entry_t *e)
{
    if_info_t info;
    gboolean is_null0 = (strcmp(e->logical_name, "null0") == 0);
    gboolean has_info = (!is_null0 && if_get_info(e->physical_name, &info) == ERRCODE_SUCCESS);

    char ip4_str[70], ip6_str[70], ip6_ll_str[70];
    show_format_prefix(&e->prefix_v4, ip4_str, sizeof(ip4_str));
    show_format_prefix(&e->prefix_v6, ip6_str, sizeof(ip6_str));
    show_format_prefix(&e->prefix_v6_linklocal, ip6_ll_str, sizeof(ip6_ll_str));

    char mac_str[32] = "-";
    const char *type_str = is_null0 ? "Blackhole" : "-";
    const char *link_str = "DOWN";
    gboolean link_up = FALSE;
    int mtu = 0;
    if (has_info)
    {
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2],
                 info.mac[3], info.mac[4], info.mac[5]);
        type_str = if_type_to_string(info.type);
        link_up = (info.state == IF_STATE_UP);
        link_str = link_up ? "UP" : "DOWN";
        mtu = info.mtu;
    }
    else if (e->link_up >= 0)
    {
        link_up = (e->link_up != 0);
        link_str = link_up ? "UP" : "DOWN";
    }

    gboolean has_ip = net_prefix_is_set(&e->prefix_v4) || net_prefix_is_set(&e->prefix_v6);
    gboolean proto_up = (!e->shutdown && link_up && (is_null0 || has_ip));
    const char *proto_str = proto_up ? "UP" : "DOWN";

    g_string_append_printf(resp_buf,
                           "\r\nInterface %s Detail:\r\n"
                           "============================\r\n"
                           "  Name       : %s\r\n"
                           "  Ifindex    : %u\r\n"
                           "  Type       : %s\r\n"
                           "  Proto State: %s\r\n"
                           "  Link State : %s\r\n"
                           "  IPv4 Addr  : %s\r\n"
                           "  IPv6 Addr  : %s\r\n"
                           "  IPv6 LLAddr: %s\r\n"
                           "  MAC        : %s\r\n"
                           "  MTU        : %d\r\n\r\n",
                           ifname, ifname, e->ifindex, type_str, proto_str, link_str, ip4_str, ip6_str, ip6_ll_str,
                           mac_str, mtu);
}

static if_map_entry_t *find_entry(const char *name)
{
    if (!g_if_work_local || !g_if_work_local->interface_map.all_entries || !name)
    {
        return NULL;
    }
    return (if_map_entry_t *)g_tree_lookup(g_if_work_local->interface_map.all_entries, name);
}

int if_show_handle_cli(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    /* We could check msg_type == CLI_MSG_TYPE_CONTINUE here, but it's now handled by the generic dispatcher
       before calling us if needed, or we can just redirect it. But wait, in work thread, how does it handle continue?
       Earlier in if_worker.c, CLI_MSG_TYPE_CONTINUE was sent to IF_WORKER_CMD_SHOW.
       So we must check if msg_type is CONTINUE. */
    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        return if_show_handle_continue(msg);
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        send_resp(msg, "IF show Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    if (parser.group_id != IF_CLI_GROUP_ID_SHOW)
    {
        cli_tlv_cleanup(&parser);
        return ERRCODE_SUCCESS; /* Let others handle if somehow miscued */
    }

    const char *ge_ifname = NULL;
    uint32_t loop_id = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(&parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id >= 1 && entry.cfg_id <= 8)
        {
            ge_ifname = if_cfgid_to_name(entry.cfg_id);
        }
        else if (entry.cfg_id == 9)
        {
            loop_id = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_cleanup(&parser);

    GString *resp_buf = g_string_new("");
    if (!resp_buf)
    {
        send_resp(msg, "IF Error: Out of memory.\r\n");
        return ERRCODE_FAIL;
    }

    if (ge_ifname)
    {
        if_map_entry_t *e = find_entry(ge_ifname);
        if (!e)
        {
            g_string_append_printf(resp_buf, "Error: Interface %s not found\r\n", ge_ifname);
            send_resp(msg, resp_buf->str);
            g_string_free(resp_buf, TRUE);
            return ERRCODE_FAIL;
        }
        show_single_entry(resp_buf, ge_ifname, e);
    }
    else if (loop_id >= IF_LOOP_ID_MIN && loop_id <= IF_LOOP_ID_MAX)
    {
        char loop_name[32];
        snprintf(loop_name, sizeof(loop_name), "loop%u", loop_id);
        if_map_entry_t *e = find_entry(loop_name);
        if (!e)
        {
            g_string_append_printf(resp_buf, "Error: Interface %s not found\r\n", loop_name);
            send_resp(msg, resp_buf->str);
            g_string_free(resp_buf, TRUE);
            return ERRCODE_FAIL;
        }
        show_single_entry(resp_buf, loop_name, e);
    }
    else
    {
        g_string_append_printf(resp_buf,
                               "\r\nInterface Status:\r\n"
                               "%-14s %-6s %-6s %-48s\r\n"
                               "-------------- ------ ------ ------------------------------------------------\r\n",
                               "Name", "Proto", "Link", "IP Address");

        if_map_t *map = &g_if_work_local->interface_map;
        if (map->all_entries)
        {
            g_tree_foreach(map->all_entries, show_foreach_cb, resp_buf);
        }

        g_string_append(resp_buf, "\r\n");
    }

    return if_show_send_chunked(msg, resp_buf);
}

/* ============================================================================
 * 分片流与候选查询（worker 独占访问 show_stream / interface_map）
 * ============================================================================ */

int if_show_send_chunked(dev_ipc_message_t *msg, GString *full_text)
{
    return cli_chunk_stream_start(&g_if_work_local->show_stream, if_local_ipc_ctx(), DEV_MODULE_ID_IF, msg, full_text);
}

int if_show_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_if_work_local->show_stream, if_local_ipc_ctx(), DEV_MODULE_ID_IF, msg);
}

void if_show_cleanup_state(void)
{
    if (g_if_work_local)
    {
        cli_chunk_stream_reset(&g_if_work_local->show_stream);
    }
}

static gint compare_if_entry_name(gconstpointer a, gconstpointer b)
{
    const if_map_entry_t *ea = (const if_map_entry_t *)a;
    const if_map_entry_t *eb = (const if_map_entry_t *)b;
    return g_ascii_strcasecmp(ea->logical_name, eb->logical_name);
}

static gboolean collect_candidate_entry_cb(gpointer key, gpointer value, gpointer user_data)
{
    (void)key;
    GList **entries = (GList **)user_data;
    if (!entries)
    {
        return FALSE;
    }
    *entries = g_list_prepend(*entries, value);
    return FALSE;
}

void if_show_handle_query_candidates(dev_ipc_message_t *msg)
{
    uint32_t query_id = 0;
    if (msg->payload && msg->payload_len >= sizeof(uint32_t))
    {
        uint32_t net_id = 0;
        memcpy(&net_id, msg->payload, sizeof(uint32_t));
        query_id = g_ntohl(net_id);
    }

    GByteArray *buf = g_byte_array_new();
    if (query_id == 1 && g_if_work_local && g_if_work_local->interface_map.all_entries)
    {
        GList *entries = NULL;
        g_tree_foreach(g_if_work_local->interface_map.all_entries, collect_candidate_entry_cb, &entries);
        entries = g_list_sort(entries, compare_if_entry_name);

        for (GList *node = entries; node; node = node->next)
        {
            const if_map_entry_t *e = (const if_map_entry_t *)node->data;
            if (!e)
            {
                continue;
            }
            g_byte_array_append(buf, (const guint8 *)e->logical_name, (guint)strlen(e->logical_name) + 1);
        }
        g_list_free(entries);
    }

    guint8 nul = '\0';
    g_byte_array_append(buf, &nul, 1);

    guint payload_len = buf->len;
    uint8_t *payload = g_byte_array_free(buf, FALSE);

    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_QUERY_CANDIDATES_RESP, DEV_MODULE_ID_IF,
                                                     msg->src_module_id, msg->request_id, payload, payload_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(if_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(payload);
    }
    dev_ipc_message_free(msg);
}
