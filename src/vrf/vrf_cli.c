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

/** CLI 命令组 ID：vrf <vrf-name> */
#define VRF_CLI_GROUP_ID_CREATE 1
/** CLI 命令组 ID：show vrf */
#define VRF_CLI_GROUP_ID_SHOW 2

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void vrf_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_VRF, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_vrf_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理 "vrf <vrf-name>" 命令（group_id=1）
 *
 * cfg_id: 1=vrf(keyword) 2=vrf-name(parameter)
 */
static int handle_vrf_create(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    char vrf_name[VRF_NAME_MAX_LEN] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == 2)
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
        vrf_send_cli_response(msg, "VRF Error: 缺少 VRF 名称\r\n");
        return ERRCODE_FAIL;
    }

    /* 公网 VRF 不允许手动创建 */
    if (strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        vrf_send_cli_response(msg, "VRF Error: 公网 VRF 不可手动创建\r\n");
        return ERRCODE_FAIL;
    }

    /* 检查名称是否已存在（幂等：已存在则提示） */
    if (vrf_find_by_name(vrf_name))
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "VRF '%s' 已存在\r\n", vrf_name);
        vrf_send_cli_response(msg, buf);
        return ERRCODE_SUCCESS;
    }

    vrf_entry_t *entry2 = vrf_create(vrf_name);
    if (!entry2)
    {
        vrf_send_cli_response(msg, "VRF Error: 创建失败\r\n");
        return ERRCODE_FAIL;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "VRF '%s' 创建成功 (id=%u)\r\n", entry2->name, entry2->vrf_id);
    vrf_send_cli_response(msg, buf);
    return ERRCODE_SUCCESS;
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
 * @brief 处理 "show vrf" 命令（group_id=2）
 */
static int handle_vrf_show(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* 跳过所有 TLV（show 命令无额外参数） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }

    /* 按 vrf_id 排序后输出 */
    GList *list = g_hash_table_get_values(g_vrf_local->vrf_by_id);
    list = g_list_sort(list, compare_vrf_by_id);

    char buf[CLI_MAX_RESP_LEN];
    size_t off = 0;

    CLI_BUF_APPEND(buf, sizeof(buf), off, "VRF Table:\r\n");
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  %-8s  %s\r\n", "VRF-ID", "Name");
    CLI_BUF_APPEND(buf, sizeof(buf), off, "  --------  --------------------------------\r\n");

    for (GList *node = list; node; node = node->next)
    {
        const vrf_entry_t *e = (const vrf_entry_t *)node->data;
        CLI_BUF_APPEND(buf, sizeof(buf), off, "  %-8u  %s\r\n", e->vrf_id, e->name);
    }
    g_list_free(list);

    vrf_send_cli_response(msg, buf);
    return ERRCODE_SUCCESS;
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

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        vrf_send_cli_response(msg, "VRF Error: 解析命令失败\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

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
            LOG_WARN("未知 group_id: %u", parser.group_id);
            vrf_send_cli_response(msg, "VRF Error: 未知命令\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}

int vrf_cli_handle_continue(dev_ipc_message_t *msg)
{
    vrf_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}
