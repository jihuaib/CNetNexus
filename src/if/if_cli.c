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
#include "if_main.h"
#include "if_map.h"
#include "log.h"

// ============================================================================
// 接口上下文变量 ID（独立命名空间，ctx-id=5 对应 XML <context-out ctx-id="5">）
// ============================================================================

/** 接口索引 ctx 变量：值 1-4 分别对应 GE-1 到 GE-4 */
#define IF_CTX_VAR_IDX 5

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
    gboolean is_no = FALSE;
    char ip[20] = {0};
    char mask[20] = {0};
    gboolean has_ip = FALSE;
    gboolean has_shutdown = FALSE;

    /* 解析 TLV 条目 */
    uint32_t if_idx = 0; /* 接口索引（ctx_id=IF_CTX_VAR_IDX 的值，1-4） */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            /* 上下文变量：ctx_id=IF_CTX_VAR_IDX → 接口索引 */
            if (entry.cfg_id == IF_CTX_VAR_IDX)
            {
                if_idx = (uint32_t)cli_tlv_entry_get_int(&entry);
            }
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* ip_address 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    strncpy(ip, text, sizeof(ip) - 1);
                    has_ip = TRUE;
                }
                break;
            }
            case 2: /* netmask 参数 */
            {
                const char *text = cli_tlv_entry_get_text(&entry);
                if (text)
                {
                    strncpy(mask, text, sizeof(mask) - 1);
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

    const char *phys_name = if_map_get_physical(ifname);

    if (has_ip)
    {
        /* ip address <ip> <mask> */
        if (if_set_ip(phys_name, ip, mask) == ERRCODE_SUCCESS)
        {
            db_condition_t conditions[] = {
                {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
            };
            db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};
            db_record_t *rec = db_record_new();
            db_record_set_text(rec, "ip_address", ip);
            db_record_set_text(rec, "netmask", mask);
            db_rpc_update_record(g_if_local->dev_ipc_ctx, "if_interface", rec, &filter);
            db_record_free(rec);
            db_value_free(&conditions[0].value);

            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "IP address configured successfully on %s\r\n", ifname);
            send_resp(msg, resp_buf);
        }
        else
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Error: Failed to set IP address on %s\r\n", ifname);
            send_resp(msg, resp_buf);
            return ERRCODE_FAIL;
        }
    }
    else
    {
        /* shutdown / no shutdown */
        int state = is_no ? 1 : 0; /* no shutdown → UP(1), shutdown → DOWN(0) */
        if (if_set_state(phys_name, state) == ERRCODE_SUCCESS)
        {
            db_condition_t conditions[] = {
                {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
            };
            db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};
            db_record_t *rec = db_record_new();
            db_record_set_int(rec, "shutdown", state ? 0 : 1);
            db_rpc_update_record(g_if_local->dev_ipc_ctx, "if_interface", rec, &filter);
            db_record_free(rec);
            db_value_free(&conditions[0].value);

            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Interface %s %s\r\n", ifname, state ? "enabled" : "disabled");
            send_resp(msg, resp_buf);
        }
        else
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Error: Failed to change state for %s\r\n", ifname);
            send_resp(msg, resp_buf);
            return ERRCODE_FAIL;
        }
    }

    (void)has_shutdown;
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
        /* show if <name> - 显示单个接口 */
        const char *phys_name = if_map_get_physical(ifname);
        if_info_t info;
        if (if_get_info(phys_name, &info) == ERRCODE_SUCCESS)
        {
            char mac_str[32];
            snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1], info.mac[2],
                     info.mac[3], info.mac[4], info.mac[5]);

            const char *type_str = if_type_to_string(info.type);
            const char *state_str = info.state == IF_STATE_UP ? "UP" : "DOWN";
            const char *ip_str = info.ip_address[0] ? info.ip_address : "not configured";
            const char *mask_str = info.netmask[0] ? info.netmask : "not configured";

            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset,
                           "\r\nInterface %s Detail:\r\n"
                           "============================\r\n"
                           "  Name       : %s\r\n"
                           "  Type       : %s\r\n"
                           "  State      : %s\r\n"
                           "  IP Address : %s\r\n"
                           "  Netmask    : %s\r\n"
                           "  MAC        : %s\r\n"
                           "  MTU        : %d\r\n\r\n",
                           ifname, ifname, type_str, state_str, ip_str, mask_str, mac_str, info.mtu);
        }
        else
        {
            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "Error: Interface %s not found\r\n", ifname);
            send_resp(msg, resp_buf);
            return ERRCODE_FAIL;
        }
    }
    else
    {
        /* 显示所有接口 */
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset,
                       "\r\nInterface Status:\r\n"
                       "%-12s %-10s %-6s %-16s\r\n"
                       "------------ ---------- ------ ----------------\r\n",
                       "Name", "Type", "State", "IP Address");

        for (int i = 0; i < g_interface_map.count; i++)
        {
            const char *logical_name = g_interface_map.entries[i].logical_name;
            const char *phys_name = g_interface_map.entries[i].physical_name;

            if_info_t info;
            if (if_get_info(phys_name, &info) == ERRCODE_SUCCESS)
            {
                const char *type_str = if_type_to_string(info.type);
                const char *state_str = info.state == IF_STATE_UP ? "UP" : "DOWN";
                const char *ip_str = info.ip_address[0] ? info.ip_address : "-";
                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "%-12s %-10s %-6s %-16s\r\n", logical_name, type_str,
                               state_str, ip_str);
            }
            else
            {
                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "%-12s %-10s %-6s %-16s\r\n", logical_name, "-",
                               "DOWN", "-");
            }

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
