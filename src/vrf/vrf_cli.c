/**
 * @file   vrf_cli.c
 * @brief  VRF 模块 CLI 命令处理实现
 * @author jhb
 * @date   2026/03/05
 */
#include "vrf_cli.h"

#include <glib.h>
#include <string.h>

#include "cli.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "vrf_main.h"

/** CLI 命令组 ID：vrf <vrf-name>（创建） 和 no vrf <vrf-name>（删除） */
#define VRF_CLI_GROUP_ID_CREATE 1
/** CLI 命令组 ID：show vrf [<vrf-name>] */
#define VRF_CLI_GROUP_ID_SHOW 2

/** show vrf <vrf-name> 的参数 cfg_id */
#define VRF_CFGID_SHOW_NAME 3

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void vrf_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_VRF, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(vrf_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理 "vrf <vrf-name>" 和 "no vrf <vrf-name>" 命令（group_id=1）
 *
 * cfg_id: 1=vrf(keyword) 2=vrf-name(parameter) 4=delete-vrf-name(parameter)
 */
static int handle_vrf_create(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char vrf_name[VRF_NAME_MAX_LEN] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == 1)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(vrf_name, sizeof(vrf_name), "%s", s);
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (vrf_name[0] == '\0')
    {
        vrf_send_cli_response(msg, "VRF Error: Missing VRF name\r\n");
        return ERRCODE_FAIL;
    }

    /* 公网 VRF 不允许手动创建或删除 */
    if (strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        vrf_send_cli_response(msg, is_no ? "VRF Error: Public VRF cannot be deleted\r\n"
                                         : "VRF Error: Public VRF cannot be manually created\r\n");
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        if (vrf_delete(vrf_name) != ERRCODE_SUCCESS)
        {
            char errbuf[128];
            snprintf(errbuf, sizeof(errbuf), "VRF Error: '%s' does not exist\r\n", vrf_name);
            vrf_send_cli_response(msg, errbuf);
            return ERRCODE_FAIL;
        }

        char buf[128];
        snprintf(buf, sizeof(buf), "VRF '%s' deleted\r\n", vrf_name);
        vrf_send_cli_response(msg, buf);
        return ERRCODE_SUCCESS;
    }
    else
    {
        /* 检查名称是否已存在 */
        if (vrf_find_by_name(vrf_name))
        {
            vrf_send_cli_response(msg, "");
            return ERRCODE_SUCCESS;
        }

        vrf_entry_t *entry2 = vrf_create(vrf_name);
        if (!entry2)
        {
            vrf_send_cli_response(msg, "VRF Error: Creation failed\r\n");
            return ERRCODE_FAIL;
        }

        vrf_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }
}

/**
 * @brief vrf_entry_t 按 vrf_id 升序比较（用于排序）
 */
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

/**
 * @brief 处理 "show vrf [<vrf-name>]" 命令（group_id=2）
 *
 * 无参数时列出所有 VRF；有 cfg_id=3 参数时显示指定 VRF 详情。
 */
static int handle_vrf_show(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char vrf_name[VRF_NAME_MAX_LEN] = {0};

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
        cli_tlv_entry_free(&entry);
    }

    GString *buf = g_string_new("");
    if (!buf)
    {
        vrf_send_cli_response(msg, "VRF Error: Out of memory\r\n");
        return ERRCODE_FAIL;
    }

    if (vrf_name[0] != '\0')
    {
        /* show vrf <vrf-name>：显示单条 VRF 详情 */
        const vrf_entry_t *e = vrf_find_by_name(vrf_name);
        if (!e)
        {
            char errbuf[128];
            snprintf(errbuf, sizeof(errbuf), "VRF Error: '%s' does not exist\r\n", vrf_name);
            vrf_send_cli_response(msg, errbuf);
            g_string_free(buf, TRUE);
            return ERRCODE_FAIL;
        }

        g_string_append(buf, "\r\nVRF Detail:\r\n");
        g_string_append_printf(buf, "  VRF-ID : %u\r\n", e->vrf_id);
        g_string_append_printf(buf, "  Name   : %s\r\n\r\n", e->name);
    }
    else
    {
        /* show vrf：列出所有 VRF */
        GList *list = g_hash_table_get_values(g_vrf_local->vrf_by_id);
        list = g_list_sort(list, compare_vrf_by_id);

        g_string_append(buf, "VRF Table:\r\n");
        g_string_append_printf(buf, "  %-8s  %s\r\n", "VRF-ID", "Name");
        g_string_append(buf, "  --------  --------------------------------\r\n");

        for (GList *node = list; node; node = node->next)
        {
            const vrf_entry_t *e = (const vrf_entry_t *)node->data;
            g_string_append_printf(buf, "  %-8u  %s\r\n", e->vrf_id, e->name);
        }
        g_list_free(list);
    }

    return cli_chunk_stream_start(&g_vrf_local->show_stream, vrf_local_ipc_ctx(), DEV_MODULE_ID_VRF, msg, buf);
}

int vrf_cli_handle_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("VRF: invalid SHOW_CONFIG scope payload");
        return cli_chunk_stream_start(&g_vrf_local->show_stream, vrf_local_ipc_ctx(), DEV_MODULE_ID_VRF, msg, NULL);
    }

    GString *out = g_string_new("");
    if (!out)
    {
        return cli_chunk_stream_start(&g_vrf_local->show_stream, vrf_local_ipc_ctx(), DEV_MODULE_ID_VRF, msg, NULL);
    }

    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS)
    {
        if (strcmp(scope.view_name, CLI_VIEW_VRF) != 0)
        {
            g_string_free(out, TRUE);
            return cli_chunk_stream_start(&g_vrf_local->show_stream, vrf_local_ipc_ctx(), DEV_MODULE_ID_VRF, msg, NULL);
        }

        char vrf_name[VRF_NAME_MAX_LEN] = {0};
        if (cli_ctx_lookup_text(scope.ctx_data, scope.ctx_len, CLI_CTX_ID_VRF_NAME, vrf_name, sizeof(vrf_name)) != 0 ||
            vrf_name[0] == '\0')
        {
            g_string_free(out, TRUE);
            return cli_chunk_stream_start(&g_vrf_local->show_stream, vrf_local_ipc_ctx(), DEV_MODULE_ID_VRF, msg, NULL);
        }

        const vrf_entry_t *e = vrf_find_by_name(vrf_name);
        if (e && e->vrf_id != VRF_PUBLIC_VRF_ID)
        {
            g_string_append(out, "!\r\n");
            g_string_append_printf(out, "vrf %s\r\n", e->name);
            g_string_append(out, "!\r\n");
        }

        return cli_chunk_stream_start(&g_vrf_local->show_stream, vrf_local_ipc_ctx(), DEV_MODULE_ID_VRF, msg, out);
    }

    GList *list = g_hash_table_get_values(g_vrf_local->vrf_by_id);
    list = g_list_sort(list, compare_vrf_by_id);

    for (GList *node = list; node; node = node->next)
    {
        const vrf_entry_t *e = (const vrf_entry_t *)node->data;
        if (!e || e->vrf_id == VRF_PUBLIC_VRF_ID)
        {
            continue;
        }

        g_string_append(out, "!\r\n");
        g_string_append_printf(out, "vrf %s\r\n", e->name);
    }

    if (out->len > 0)
    {
        g_string_append(out, "!\r\n");
    }

    g_list_free(list);
    return cli_chunk_stream_start(&g_vrf_local->show_stream, vrf_local_ipc_ctx(), DEV_MODULE_ID_VRF, msg, out);
}

// ============================================================================
// 消息 dispatch
// ============================================================================

int vrf_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_chunk_stream_reset(&g_vrf_local->show_stream);

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("Payload parsing failed");
        vrf_send_cli_response(msg, "VRF Error: Failed to parse command\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("Received TLV payload (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case VRF_CLI_GROUP_ID_CREATE:
            result = handle_vrf_create(msg, &parser);
            break;
        case VRF_CLI_GROUP_ID_SHOW:
            result = handle_vrf_show(msg, &parser);
            break;
        default:
            LOG_WARN("Unknown group_id: %u", parser.group_id);
            vrf_send_cli_response(msg, "VRF Error: Unknown command\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}

int vrf_cli_handle_continue(dev_ipc_message_t *msg)
{
    return cli_chunk_stream_continue(&g_vrf_local->show_stream, vrf_local_ipc_ctx(), DEV_MODULE_ID_VRF, msg);
}

void vrf_cli_cleanup_state(void)
{
    cli_chunk_stream_reset(&g_vrf_local->show_stream);
}

// ============================================================================
// 动态候选值查询
// ============================================================================

void vrf_cli_handle_query_candidates(dev_ipc_message_t *msg)
{
    /* 解析 cfg_id（网络字节序） */
    uint32_t cfg_id = 0;
    if (msg->payload && msg->payload_len >= sizeof(uint32_t))
    {
        uint32_t net_id;
        memcpy(&net_id, msg->payload, sizeof(uint32_t));
        cfg_id = g_ntohl(net_id);
    }

    (void)cfg_id; // 当前命令仅一个候选值查询，无需区分 cfg_id

    /* 按 vrf_id 升序枚举所有 VRF */
    GList *list = g_hash_table_get_values(g_vrf_local->vrf_by_id);
    list = g_list_sort(list, compare_vrf_by_id);

    /* 构建 "name1\0name2\0\0" 格式负载 */
    GByteArray *buf = g_byte_array_new();
    for (GList *node = list; node; node = node->next)
    {
        const vrf_entry_t *e = (const vrf_entry_t *)node->data;

        g_byte_array_append(buf, (const guint8 *)e->name, (guint)strlen(e->name) + 1);
    }
    g_list_free(list);

    /* 末尾额外 '\0' 标记结束 */
    guint8 nul = '\0';
    g_byte_array_append(buf, &nul, 1);

    guint payload_len = buf->len;
    uint8_t *payload = g_byte_array_free(buf, FALSE);

    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_QUERY_CANDIDATES_RESP, DEV_MODULE_ID_VRF,
                                                     msg->src_module_id, msg->request_id, payload, payload_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(vrf_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(payload);
    }
    dev_ipc_message_free(msg);
}
