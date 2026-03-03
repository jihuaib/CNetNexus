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
// 上下文序列化辅助（TLV 格式）
// ============================================================================

static void ctx_write_u8(GByteArray *buf, uint8_t v)
{
    g_byte_array_append(buf, &v, 1);
}

static void ctx_write_u16(GByteArray *buf, uint16_t v)
{
    uint16_t be = htons(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 2);
}

static void ctx_write_u32(GByteArray *buf, uint32_t v)
{
    uint32_t be = htonl(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 4);
}

// ============================================================================
// 发送响应辅助
// ============================================================================

static void send_resp(dev_ipc_message_t *msg, uint32_t msg_type, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(msg_type, DEV_MODULE_ID_IF, msg->src_module_id, msg->request_id,
                                                     resp_data, strlen(resp_data) + 1, g_free);
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
    const char *ifname = NULL;
    uint32_t if_cfg_id = 0;

    /* 解析 TLV 条目获取接口名 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        if (entry.cfg_id >= 1 && entry.cfg_id <= 4)
        {
            ifname = if_cfgid_to_name(entry.cfg_id);
            if_cfg_id = entry.cfg_id;
        }
        cli_tlv_entry_free(&entry);
    }

    if (!ifname)
    {
        send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "Error: Invalid interface name\r\n");
        return ERRCODE_FAIL;
    }

    /* 设置当前接口 */
    extern char g_current_interface[IFNAMSIZ];
    strncpy(g_current_interface, ifname, IFNAMSIZ - 1);

    /* 获取 view prompt template */
    char view_name[CFG_CLI_MAX_VIEW_LEN];

    if (g_if_local->dev_ipc_ctx && dev_ipc_is_connected(g_if_local->dev_ipc_ctx, DEV_MODULE_ID_CFG))
    {
        uint32_t view_id_be = htonl(CLI_VIEW_IF);
        uint32_t *view_id_copy = g_malloc(sizeof(view_id_be));
        memcpy(view_id_copy, &view_id_be, sizeof(view_id_be));
        dev_ipc_message_t *req = dev_ipc_message_create(CFG_MSG_TYPE_CLI_CONTINUE, DEV_MODULE_ID_IF, DEV_MODULE_ID_CFG,
                                                        msg->request_id, view_id_copy, sizeof(view_id_be), g_free);

        if (req)
        {
            req->msg_type = DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0010); /* GET_VIEW_PROMPT */
            dev_ipc_message_t *resp = dev_ipc_query(g_if_local->dev_ipc_ctx, DEV_MODULE_ID_CFG, req, 1000);
            if (resp && resp->payload && resp->payload_len > 0)
            {
                snprintf(view_name, sizeof(view_name), "%s", (char *)resp->payload);
                dev_ipc_message_free(resp);
            }
            else
            {
                snprintf(view_name, sizeof(view_name), "<NetNexus(config-if-%%s)>");
                if (resp)
                {
                    dev_ipc_message_free(resp);
                }
            }
            dev_ipc_message_free(req);
        }
        else
        {
            snprintf(view_name, sizeof(view_name), "<NetNexus(config-if-%%s)>");
        }
    }
    else
    {
        snprintf(view_name, sizeof(view_name), "<NetNexus(config-if-%%s)>");
    }

    char out_prompt[CLI_CLI_MAX_PROMPT_LEN];
    snprintf(out_prompt, CLI_CLI_MAX_PROMPT_LEN, view_name, ifname);

    /* 构建上下文: [num:u16][cfg_id:u32][type:u8][length:u16][value] */
    GByteArray *ctx_buf = g_byte_array_new();
    ctx_write_u16(ctx_buf, 1);                    /* num_fields = 1 */
    ctx_write_u32(ctx_buf, if_cfg_id);            /* cfg_id（接口编号） */
    ctx_write_u8(ctx_buf, (uint8_t)DB_TYPE_TEXT); /* 类型 */
    uint16_t name_len = (uint16_t)strlen(ifname);
    ctx_write_u16(ctx_buf, name_len); /* 长度 */
    g_byte_array_append(ctx_buf, (const uint8_t *)ifname, name_len);

    uint32_t total_len = CLI_CLI_MAX_PROMPT_LEN + ctx_buf->len;
    char *msg_out = g_malloc0(total_len);
    memcpy(msg_out, out_prompt, CLI_CLI_MAX_PROMPT_LEN);
    memcpy(msg_out + CLI_CLI_MAX_PROMPT_LEN, ctx_buf->data, ctx_buf->len);
    g_byte_array_free(ctx_buf, TRUE);

    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_VIEW_CHG, DEV_MODULE_ID_IF, msg->src_module_id,
                                                     msg->request_id, msg_out, total_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_if_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }

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
    char ctx_ifname[32] = {0};

    /* 解析 TLV 条目 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            /* 上下文字段 - 接口名 */
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                strncpy(ctx_ifname, text, sizeof(ctx_ifname) - 1);
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

    /* 判断接口名 */
    extern char g_current_interface[IFNAMSIZ];
    const char *ifname = ctx_ifname[0] ? ctx_ifname : g_current_interface;
    if (ifname[0] == '\0')
    {
        send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "Error: No interface selected\r\n");
        return ERRCODE_FAIL;
    }

    const char *phys_name = if_map_get_physical(ifname);

    if (has_ip)
    {
        /* ip address <ip> <mask> */
        if (if_set_ip(phys_name, ip, mask) == ERRCODE_SUCCESS)
        {
            const char *field_names[] = {"ip_address", "netmask"};
            db_value_t values[] = {db_value_text(ip), db_value_text(mask)};
            db_condition_t conditions[] = {
                {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
            };
            db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};
            db_rpc_update(g_if_local->dev_ipc_ctx, "if_interface", field_names, values, 2, &filter);
            db_value_free(&values[0]);
            db_value_free(&values[1]);
            db_value_free(&conditions[0].value);

            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "IP address configured successfully on %s\r\n", ifname);
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, resp_buf);
        }
        else
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Error: Failed to set IP address on %s\r\n", ifname);
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, resp_buf);
            return ERRCODE_FAIL;
        }
    }
    else
    {
        /* shutdown / no shutdown */
        int state = is_no ? 1 : 0; /* no shutdown → UP(1), shutdown → DOWN(0) */
        if (if_set_state(phys_name, state) == ERRCODE_SUCCESS)
        {
            const char *field_names[] = {"shutdown"};
            db_value_t values[] = {db_value_int(state ? 0 : 1)};
            db_condition_t conditions[] = {
                {.field_name = "name", .op = DB_CMP_EQ, .value = db_value_text(ifname)},
            };
            db_filter_t filter = {.conditions = conditions, .num_conditions = G_N_ELEMENTS(conditions)};
            db_rpc_update(g_if_local->dev_ipc_ctx, "if_interface", field_names, values, 1, &filter);
            db_value_free(&conditions[0].value);

            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Interface %s %s\r\n", ifname, state ? "enabled" : "disabled");
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, resp_buf);
        }
        else
        {
            char resp_buf[128];
            snprintf(resp_buf, sizeof(resp_buf), "Error: Failed to change state for %s\r\n", ifname);
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, resp_buf);
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
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
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
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, resp_buf);
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

    send_resp(msg, CFG_MSG_TYPE_CLI_RESP, resp_buf);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int if_cli_handle_continue(dev_ipc_message_t *msg)
{
    send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "");
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
        send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "IF Error: Failed to parse command payload.\r\n");
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
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "IF Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
