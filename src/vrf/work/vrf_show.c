/**
 * @file   vrf_show.c
 * @brief  VRF show 命令处理实现（worker 线程）
 * @author jhb
 * @date   2026/05/02
 */
#include "vrf_show.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "vrf.h"
#include "vrf_os.h"
#include "vrf_pub.h"
#include "vrf_table.h"
#include "vrf_worker.h"

/** show vrf 命令组 ID（与 vrf_cli 保持一致） */
#define VRF_CLI_GROUP_ID_SHOW 2
/** show vrf os 命令组 ID */
#define VRF_CLI_GROUP_ID_SHOW_OS 6
/** show vrf <name> 参数 cfg_id */
#define VRF_CFGID_SHOW_NAME 3
/** show vrf subscribe 参数 cfg_id */
#define VRF_CFGID_SHOW_SUBSCRIBE 4

static gint compare_vrf_by_id(gconstpointer a, gconstpointer b)
{
    const vrf_entry_t *ea = (const vrf_entry_t *)a;
    const vrf_entry_t *eb = (const vrf_entry_t *)b;
    if (ea->vrf_id < eb->vrf_id)
    {
        return -1;
    }
    if (ea->vrf_id > eb->vrf_id)
    {
        return 1;
    }
    return 0;
}

static void rd_to_str(const vrf_rd_t *rd, char *out, size_t cap)
{
    if (!rd || cap < 25)
    {
        if (cap > 0)
        {
            out[0] = '\0';
        }
        return;
    }
    /* type 0: 2B AS:4B value, type 1: 4B IPv4:2B value */
    uint16_t type = ((uint16_t)rd->bytes[0] << 8) | rd->bytes[1];
    if (type == 0)
    {
        uint16_t asn = ((uint16_t)rd->bytes[2] << 8) | rd->bytes[3];
        uint32_t val = ((uint32_t)rd->bytes[4] << 24) | ((uint32_t)rd->bytes[5] << 16) | ((uint32_t)rd->bytes[6] << 8) |
                       rd->bytes[7];
        snprintf(out, cap, "%u:%u", asn, val);
    }
    else if (type == 1)
    {
        uint16_t val = ((uint16_t)rd->bytes[6] << 8) | rd->bytes[7];
        snprintf(out, cap, "%u.%u.%u.%u:%u", rd->bytes[2], rd->bytes[3], rd->bytes[4], rd->bytes[5], val);
    }
    else
    {
        snprintf(out, cap, "raw:%02x%02x%02x%02x%02x%02x%02x%02x", rd->bytes[0], rd->bytes[1], rd->bytes[2],
                 rd->bytes[3], rd->bytes[4], rd->bytes[5], rd->bytes[6], rd->bytes[7]);
    }
}

static const char *afi_to_str(uint16_t afi)
{
    return (afi == VRF_AFI_IPV4) ? "ipv4" : (afi == VRF_AFI_IPV6) ? "ipv6" : "unknown";
}

static const char *safi_to_str(uint8_t safi)
{
    switch (safi)
    {
        case VRF_SAFI_UNICAST:
            return "unicast";
        default:
            return "unknown";
    }
}

static const char *module_name(uint32_t module_id)
{
    switch (module_id)
    {
        case DEV_MODULE_ID_DEV:
            return "dev";
        case DEV_MODULE_ID_DB:
            return "db";
        case DEV_MODULE_ID_CLI:
            return "cli";
        case DEV_MODULE_ID_IF:
            return "if";
        case DEV_MODULE_ID_BGP:
            return "bgp";
        case DEV_MODULE_ID_ROUTE:
            return "route";
        case DEV_MODULE_ID_VRF:
            return "vrf";
        case DEV_MODULE_ID_SBMP:
            return "sbmp";
        case DEV_MODULE_ID_ISIS:
            return "isis";
        case DEV_MODULE_ID_TUNNEL:
            return "tunnel";
        case DEV_MODULE_ID_FIB:
            return "fib";
        case DEV_MODULE_ID_LDP:
            return "ldp";
        default:
            return "unknown";
    }
}

static void append_mask_name(char *buf, size_t cap, const char *name)
{
    if (!buf || cap == 0 || !name || name[0] == '\0')
    {
        return;
    }
    if (buf[0] != '\0')
    {
        g_strlcat(buf, "|", cap);
    }
    g_strlcat(buf, name, cap);
}

static void af_mask_to_str(uint32_t mask, char *buf, size_t cap)
{
    if (!buf || cap == 0)
    {
        return;
    }
    buf[0] = '\0';
    if (mask == VRF_AF_MASK_ALL)
    {
        g_strlcpy(buf, "all", cap);
        return;
    }
    if ((mask & VRF_AF_MASK_IPV4) != 0)
    {
        append_mask_name(buf, cap, "ipv4");
    }
    if ((mask & VRF_AF_MASK_IPV6) != 0)
    {
        append_mask_name(buf, cap, "ipv6");
    }
    uint32_t known = VRF_AF_MASK_IPV4 | VRF_AF_MASK_IPV6;
    uint32_t unknown = mask & ~known;
    if (unknown != 0)
    {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "0x%08X", unknown);
        append_mask_name(buf, cap, tmp);
    }
    if (buf[0] == '\0')
    {
        g_strlcpy(buf, "-", cap);
    }
}

static void vrf_event_mask_to_str(uint32_t mask, char *buf, size_t cap)
{
    if (!buf || cap == 0)
    {
        return;
    }
    buf[0] = '\0';
    if (mask == VRF_EVENT_ALL)
    {
        g_strlcpy(buf, "all", cap);
        return;
    }
    if ((mask & VRF_EVENT_VRF_ADD) != 0)
    {
        append_mask_name(buf, cap, "vrf-add");
    }
    if ((mask & VRF_EVENT_VRF_DEL) != 0)
    {
        append_mask_name(buf, cap, "vrf-del");
    }
    if ((mask & VRF_EVENT_VRF_STATE) != 0)
    {
        append_mask_name(buf, cap, "vrf-state");
    }
    if ((mask & VRF_EVENT_AF_ENABLE) != 0)
    {
        append_mask_name(buf, cap, "af-enable");
    }
    if ((mask & VRF_EVENT_AF_DISABLE) != 0)
    {
        append_mask_name(buf, cap, "af-disable");
    }
    if ((mask & VRF_EVENT_AF_RD_ADD) != 0)
    {
        append_mask_name(buf, cap, "rd-add");
    }
    if ((mask & VRF_EVENT_AF_RD_DEL) != 0)
    {
        append_mask_name(buf, cap, "rd-del");
    }
    if ((mask & VRF_EVENT_AF_IMPORT_RT_ADD) != 0)
    {
        append_mask_name(buf, cap, "import-rt-add");
    }
    if ((mask & VRF_EVENT_AF_IMPORT_RT_DEL) != 0)
    {
        append_mask_name(buf, cap, "import-rt-del");
    }
    if ((mask & VRF_EVENT_AF_EXPORT_RT_ADD) != 0)
    {
        append_mask_name(buf, cap, "export-rt-add");
    }
    if ((mask & VRF_EVENT_AF_EXPORT_RT_DEL) != 0)
    {
        append_mask_name(buf, cap, "export-rt-del");
    }
    if ((mask & VRF_EVENT_SMOOTHSTART) != 0)
    {
        append_mask_name(buf, cap, "smoothstart");
    }
    if ((mask & VRF_EVENT_SMOOTHEND) != 0)
    {
        append_mask_name(buf, cap, "smoothend");
    }

    uint32_t known = VRF_EVENT_VRF_ADD | VRF_EVENT_VRF_DEL | VRF_EVENT_VRF_STATE | VRF_EVENT_AF_ENABLE |
                     VRF_EVENT_AF_DISABLE | VRF_EVENT_AF_RD_ADD | VRF_EVENT_AF_RD_DEL | VRF_EVENT_AF_IMPORT_RT_ADD |
                     VRF_EVENT_AF_IMPORT_RT_DEL | VRF_EVENT_AF_EXPORT_RT_ADD | VRF_EVENT_AF_EXPORT_RT_DEL |
                     VRF_EVENT_SMOOTHSTART | VRF_EVENT_SMOOTHEND;
    uint32_t unknown = mask & ~known;
    if (unknown != 0)
    {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "0x%08X", unknown);
        append_mask_name(buf, cap, tmp);
    }
    if (buf[0] == '\0')
    {
        g_strlcpy(buf, "-", cap);
    }
}

static int handle_show_subscribe(dev_ipc_message_t *msg)
{
    GString *buf = g_string_new("");
    if (!buf)
    {
        return ERRCODE_FAIL;
    }

    g_string_append_printf(buf,
                           "\r\nVRF Subscribers:\r\n"
                           "%-10s %-10s %-14s %-88s %-8s\r\n"
                           "---------- ---------- -------------- ------------------------------------------------"
                           "---------------------------------------- --------\r\n",
                           "Module", "Module-ID", "AF", "Events", "Replay");

    uint32_t count = 0;
    GList *subscribers = vrf_worker_subscribers();
    for (GList *l = subscribers; l; l = l->next)
    {
        const vrf_subscriber_t *sub = (const vrf_subscriber_t *)l->data;
        if (!sub)
        {
            continue;
        }

        char af_str[64];
        char event_str[256];
        af_mask_to_str(sub->af_mask, af_str, sizeof(af_str));
        vrf_event_mask_to_str(sub->event_mask, event_str, sizeof(event_str));
        g_string_append_printf(buf, "%-10s 0x%08X %-14s %-88s %-8s\r\n", module_name(sub->module_id), sub->module_id,
                               af_str, event_str, sub->pending_replay ? "pending" : "ready");
        count++;
    }

    if (count == 0)
    {
        g_string_append(buf, "  (no subscribers)\r\n");
    }
    g_string_append_printf(buf, "\r\nTotal %u subscriber(s)\r\n", count);
    return cli_chunk_stream_start(vrf_worker_show_stream(), vrf_worker_ipc_ctx(), DEV_MODULE_ID_VRF, msg, buf);
}

static const char *os_state_str(uint8_t state)
{
    switch (state)
    {
        case VRF_OS_STATE_UP:
            return "UP";
        case VRF_OS_STATE_DOWN:
            return "DOWN";
        default:
            return "UNKNOWN";
    }
}

static int handle_show(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char vrf_name[VRF_NAME_MAX_LEN] = {0};
    gboolean show_subscribe = FALSE;
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == VRF_CFGID_SHOW_NAME)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(vrf_name, sizeof(vrf_name), "%s", s);
            }
        }
        else if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == VRF_CFGID_SHOW_SUBSCRIBE)
        {
            show_subscribe = TRUE;
        }
        cli_tlv_entry_free(&entry);
    }

    if (show_subscribe)
    {
        return handle_show_subscribe(msg);
    }

    GString *buf = g_string_new("");
    vrf_table_t *t = vrf_worker_table();

    if (vrf_name[0] != '\0')
    {
        const vrf_entry_t *e = vrf_table_find_by_name(t, vrf_name);
        if (!e)
        {
            g_string_append_printf(buf, "VRF Error: '%s' does not exist\r\n", vrf_name);
        }
        else
        {
            g_string_append(buf, "\r\nVRF Detail:\r\n");
            g_string_append_printf(buf, "  VRF-ID         : %u\r\n", e->vrf_id);
            g_string_append_printf(buf, "  Name           : %s\r\n", e->name);
            g_string_append_printf(buf, "  L3VRF Table-ID : %u\r\n", e->l3vrf_table_id);
            g_string_append_printf(buf, "  OS State       : %s\r\n", os_state_str(e->os_state));
            if (e->afs)
            {
                GHashTableIter it;
                gpointer k = NULL;
                gpointer v = NULL;
                g_hash_table_iter_init(&it, e->afs);
                while (g_hash_table_iter_next(&it, &k, &v))
                {
                    (void)k;
                    const vrf_af_state_t *af = (const vrf_af_state_t *)v;
                    g_string_append_printf(buf, "  af %s-%s:\r\n", afi_to_str(af->afi), safi_to_str(af->safi));
                    if (af->has_rd)
                    {
                        char rd[40];
                        rd_to_str(&af->rd, rd, sizeof(rd));
                        g_string_append_printf(buf, "    RD            : %s\r\n", rd);
                    }
                    if (af->import_rts && af->import_rts->len > 0)
                    {
                        g_string_append_printf(buf, "    Import-RT     : %u\r\n", af->import_rts->len);
                        for (guint i = 0; i < af->import_rts->len; i++)
                        {
                            const vrf_rt_t *rt = &g_array_index(af->import_rts, vrf_rt_t, i);
                            char rt_str[40];
                            rd_to_str((const vrf_rd_t *)rt, rt_str, sizeof(rt_str));
                            g_string_append_printf(buf, "      %s\r\n", rt_str);
                        }
                    }
                    if (af->export_rts && af->export_rts->len > 0)
                    {
                        g_string_append_printf(buf, "    Export-RT     : %u\r\n", af->export_rts->len);
                        for (guint i = 0; i < af->export_rts->len; i++)
                        {
                            const vrf_rt_t *rt = &g_array_index(af->export_rts, vrf_rt_t, i);
                            char rt_str[40];
                            rd_to_str((const vrf_rd_t *)rt, rt_str, sizeof(rt_str));
                            g_string_append_printf(buf, "      %s\r\n", rt_str);
                        }
                    }
                }
            }
            g_string_append(buf, "\r\n");
        }
    }
    else
    {
        GList *list = g_hash_table_get_values(t->by_id);
        list = g_list_sort(list, compare_vrf_by_id);
        g_string_append(buf, "VRF Table:\r\n");
        g_string_append_printf(buf, "  %-8s  %-32s  %s\r\n", "VRF-ID", "Name", "Table-ID");
        g_string_append(buf, "  --------  --------------------------------  --------\r\n");
        for (GList *node = list; node; node = node->next)
        {
            const vrf_entry_t *e = (const vrf_entry_t *)node->data;
            g_string_append_printf(buf, "  %-8u  %-32s  %u\r\n", e->vrf_id, e->name, e->l3vrf_table_id);
        }
        g_list_free(list);
    }

    return cli_chunk_stream_start(vrf_worker_show_stream(), vrf_worker_ipc_ctx(), DEV_MODULE_ID_VRF, msg, buf);
}

static int handle_show_os(dev_ipc_message_t *msg)
{
    GString *buf = g_string_new("");
    if (vrf_os_show(buf) != 0)
    {
        g_string_append(buf, "vrf_os: dump failed\r\n");
    }
    return cli_chunk_stream_start(vrf_worker_show_stream(), vrf_worker_ipc_ctx(), DEV_MODULE_ID_VRF, msg, buf);
}

int vrf_show_handle_cli(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }
    cli_chunk_stream_reset(vrf_worker_show_stream());

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    if (parser.group_id == VRF_CLI_GROUP_ID_SHOW)
    {
        rc = handle_show(msg, &parser);
    }
    else if (parser.group_id == VRF_CLI_GROUP_ID_SHOW_OS)
    {
        rc = handle_show_os(msg);
    }
    cli_tlv_cleanup(&parser);
    return rc;
}

int vrf_show_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(vrf_worker_show_stream(), vrf_worker_ipc_ctx(), DEV_MODULE_ID_VRF, msg);
}

int vrf_show_handle_show_config(dev_ipc_message_t *msg)
{
    /* SHOW_CONFIG 走 IPC 线程的 vrf_bdr，不应到达 worker；防御性返回空。 */
    return cli_chunk_stream_start(vrf_worker_show_stream(), vrf_worker_ipc_ctx(), DEV_MODULE_ID_VRF, msg, NULL);
}

void vrf_show_handle_query_candidates(dev_ipc_message_t *msg)
{
    uint32_t query_id = VRF_CANDIDATE_QUERY_ALL;
    if (msg->payload && msg->payload_len >= sizeof(uint32_t))
    {
        uint32_t net_id = 0;
        memcpy(&net_id, msg->payload, sizeof(uint32_t));
        query_id = g_ntohl(net_id);
    }
    gboolean include_public = (query_id != VRF_CANDIDATE_QUERY_NON_PUBLIC);

    vrf_table_t *t = vrf_worker_table();
    GList *list = g_hash_table_get_values(t->by_id);
    list = g_list_sort(list, compare_vrf_by_id);

    GByteArray *buf = g_byte_array_new();
    for (GList *node = list; node; node = node->next)
    {
        const vrf_entry_t *e = (const vrf_entry_t *)node->data;
        if (!include_public && e && strcmp(e->name, VRF_PUBLIC_VRF_NAME) == 0)
        {
            continue;
        }
        g_byte_array_append(buf, (const guint8 *)e->name, (guint)strlen(e->name) + 1);
    }
    g_list_free(list);
    guint8 nul = '\0';
    g_byte_array_append(buf, &nul, 1);

    guint payload_len = buf->len;
    uint8_t *payload = g_byte_array_free(buf, FALSE);

    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_QUERY_CANDIDATES_RESP, DEV_MODULE_ID_VRF,
                                                     msg->src_module_id, msg->request_id, payload, payload_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(vrf_worker_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(payload);
    }
    dev_ipc_message_free(msg);
}
