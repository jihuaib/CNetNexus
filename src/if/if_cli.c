/**
 * @file   if_cli.c
 * @brief  接口模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "if_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if.h"
#include "if_cfg_apply.h"
#include "if_main.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 接口上下文变量 ID（独立命名空间，ctx-id=5 对应 XML <context-out ctx-id="5">）
// ============================================================================

/** 接口索引 ctx 变量：值 1-4 分别对应 GE-1 到 GE-4 */
#define CLI_CTX_ID_IF_IDX 5

// ============================================================================
// 发送响应辅助
// ============================================================================

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_if_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 接口名称映射表（cfg_id → 逻辑接口名）
// ============================================================================

static const char *if_cfgid_to_name(uint32_t cfg_id)
{
    switch (cfg_id)
    {
        case 1:
            return "GE-1";
        case 2:
            return "GE-2";
        case 3:
            return "GE-3";
        case 4:
            return "GE-4";
        default:
            return NULL;
    }
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理接口进入命令（if GE-x → 视图切换）
 *
 * group_id=1, cfg_id: 1=GE-1, 2=GE-2, 3=GE-3, 4=GE-4
 */
static int handle_if_entry(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    /* 框架根据 XML <context-out ctx-id="5" value="N"/> 自动切换视图并写入上下文，
     * 视图 template "<NetNexus(config-if-GE-{ctx:5})>" 由框架格式化为 "GE-1/2/3/4"。
     * 模块只需验证并返回空 OK。 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        cli_tlv_entry_free(&entry);
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理接口配置命令（ip address / shutdown / no shutdown）
 *
 * group_id=2, cfg_id: 1=ip_address, 2=netmask, 3=shutdown, 4=no
 * 上下文 cfg_id 1-4 对应接口名
 */
static int handle_if_config(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    net_prefix_t prefix;
    memset(&prefix, 0, sizeof(prefix));
    gboolean has_ip = FALSE;
    gboolean has_shutdown = FALSE;
    uint32_t if_idx = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_IF_IDX)
            {
                if_idx = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* ip_address 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text && net_addr_from_str(text, &prefix.addr) == 0)
                {
                    has_ip = TRUE;
                }
                break;
            }
            case 2: /* prefix_len 参数（整数） */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    prefix.prefix_len = (uint8_t)atoi(text);
                }
                break;
            }
            case 3: /* shutdown 关键字 */
                has_shutdown = TRUE;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    /* 从上下文索引解析接口名 */
    const char *ifname = if_cfgid_to_name(if_idx);
    if (!ifname)
    {
        send_resp(msg, "Error: No interface selected\r\n");
        return ERRCODE_FAIL;
    }

    if (has_ip)
    {
        /* ip address <ip> <prefix-len> */
        if (if_cfg_apply_ip(is_no, ifname, &prefix) != ERRCODE_SUCCESS)
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Error: Failed to set IP address on %s\r\n", ifname);
            send_resp(msg, resp_buf);
            return ERRCODE_FAIL;
        }

        /* 持久化 */
        char ip_str[64] = "";
        if (!is_no)
        {
            net_addr_to_str(&prefix.addr, ip_str, sizeof(ip_str));
        }
        db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)};
        db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
        db_record_t *rec = db_record_new();
        db_record_set_text(rec, "ip_address", ip_str);
        db_record_set_int(rec, "prefix_len", is_no ? 0 : (int64_t)prefix.prefix_len);
        db_rpc_update_record(g_if_local->dev_ipc_ctx, "if_interface", rec, &filter);
        db_record_free(rec);
        db_value_free(&cond.value);

        char resp_buf[128];
        snprintf(resp_buf, sizeof(resp_buf), "IP address %s on %s\r\n", is_no ? "cleared" : "configured", ifname);
        send_resp(msg, resp_buf);
    }
    else if (has_shutdown)
    {
        /* shutdown / no shutdown */
        if (if_cfg_apply_shutdown(is_no, ifname) != ERRCODE_SUCCESS)
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Error: Failed to change state for %s\r\n", ifname);
            send_resp(msg, resp_buf);
            return ERRCODE_FAIL;
        }

        /* 持久化 */
        db_condition_t cond = {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)};
        db_filter_t filter = {.conditions = &cond, .num_conditions = 1};
        db_record_t *rec = db_record_new();
        db_record_set_int(rec, "shutdown", is_no ? 0 : 1);
        db_rpc_update_record(g_if_local->dev_ipc_ctx, "if_interface", rec, &filter);
        db_record_free(rec);
        db_value_free(&cond.value);

        char resp_buf[128];
        snprintf(resp_buf, sizeof(resp_buf), "Interface %s %s\r\n", ifname, is_no ? "enabled" : "disabled");
        send_resp(msg, resp_buf);
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 show interface 命令
 *
 * group_id=3, cfg_id: 1=GE-1, 2=GE-2, 3=GE-3, 4=GE-4
 * 直接构建格式化文本返回
 */
static int handle_if_show(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const char *ifname = NULL;

    /* 解析可选接口名 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id >= 1 && entry.cfg_id <= 4)
        {
            ifname = if_cfgid_to_name(entry.cfg_id);
        }
        cli_tlv_entry_free(&entry);
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    size_t offset = 0;

    if (ifname)
    {
        /* show if <name> - 显示单个接口详情 */
        if_map_entry_t *entry = if_cfg_find_entry(ifname);
        if (!entry)
        {
            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "Error: Interface %s not found\r\n", ifname);
            send_resp(msg, resp_buf);
            return ERRCODE_FAIL;
        }

        /* MAC / MTU / 类型 从 OS 取 */
        if_info_t info;
        gboolean has_info = (if_get_info(entry->physical_name, &info) == ERRCODE_SUCCESS);

        char ip_str[70] = "-";
        if (net_prefix_is_set(&entry->prefix))
        {
            net_prefix_to_str(&entry->prefix, ip_str, sizeof(ip_str));
        }

        char mac_str[32] = "-";
        const char *type_str = "-";
        int mtu = 0;
        if (has_info)
        {
            snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2],
                     info.mac[3], info.mac[4], info.mac[5]);
            type_str = if_type_to_string(info.type);
            mtu = info.mtu;
        }

        const char *state_str = entry->shutdown ? "DOWN" : "UP";

        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset,
                       "\r\nInterface %s Detail:\r\n"
                       "============================\r\n"
                       "  Name       : %s\r\n"
                       "  Type       : %s\r\n"
                       "  State      : %s\r\n"
                       "  IP Address : %s/%u\r\n"
                       "  MAC        : %s\r\n"
                       "  MTU        : %d\r\n\r\n",
                       ifname, ifname, type_str, state_str, ip_str, entry->prefix.prefix_len, mac_str, mtu);
    }
    else
    {
        /* 显示所有接口 */
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset,
                       "\r\nInterface Status:\r\n"
                       "%-12s %-6s %-20s\r\n"
                       "------------ ------ --------------------\r\n",
                       "Name", "State", "IP Address");

        if_map_t *map = &g_if_local->interface_map;
        for (int i = 0; i < map->count; i++)
        {
            if_map_entry_t *e = &map->entries[i];
            const char *state_str = e->shutdown ? "DOWN" : "UP";

            char ip_str[70] = "-";
            if (net_prefix_is_set(&e->prefix))
            {
                net_prefix_to_str(&e->prefix, ip_str, sizeof(ip_str));
            }

            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "%-12s %-6s %-20s\r\n", e->logical_name, state_str,
                           ip_str);

            if (offset >= sizeof(resp_buf) - 128)
            {
                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  ... (truncated)\r\n");
                break;
            }
        }
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\n");
    }

    send_resp(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int if_cli_handle_continue(dev_ipc_message_t *msg)
{
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

int if_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        send_resp(msg, "IF Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case IF_CLI_GROUP_ID_ENTRY:
            result = handle_if_entry(msg, &parser);
            break;
        case IF_CLI_GROUP_ID_CONFIG:
            result = handle_if_config(msg, &parser);
            break;
        case IF_CLI_GROUP_ID_SHOW:
            result = handle_if_show(msg, &parser);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            send_resp(msg, "IF Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
