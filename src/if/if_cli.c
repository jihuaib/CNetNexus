/**
 * @file   if_cli.c
 * @brief  接口模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "if_cli.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "cfg.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "if.h"
#include "if_main.h"
#include "if_map.h"
#include "ipc.h"

// ============================================================================
// 上下文序列化辅助（新字段格式）
// ============================================================================

static void ctx_write_u16(GByteArray *buf, uint16_t v)
{
    uint16_t be = htons(v);
    g_byte_array_append(buf, (const uint8_t *)&be, 2);
}

static void ctx_write_u8(GByteArray *buf, uint8_t v)
{
    g_byte_array_append(buf, &v, 1);
}

static void ctx_write_string(GByteArray *buf, const char *s)
{
    if (!s)
    {
        ctx_write_u16(buf, 0xFFFF);
        return;
    }
    uint16_t len = (uint16_t)strlen(s);
    ctx_write_u16(buf, len);
    if (len > 0)
    {
        g_byte_array_append(buf, (const uint8_t *)s, len);
    }
}

// ============================================================================
// 发送响应辅助
// ============================================================================

static void send_resp(ipc_message_t *msg, uint32_t msg_type, const char *text)
{
    char *resp_data = g_strdup(text);
    ipc_message_t *resp = ipc_message_create(msg_type, DEV_MODULE_ID_IF, msg->src_module_id, msg->request_id,
                                                   resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        ipc_send_response(g_if_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }
}

// ============================================================================
// Show 输出写入临时 DB
// ============================================================================

#define IF_SHOW_DB "if_show_db"
#define IF_SHOW_META "if_show_meta"
#define IF_SHOW_ROW "if_show_row"
#define IF_SHOW_DETAIL "if_show_detail"

static int if_insert_show_meta(int has_rows)
{
    const char *fields[] = {"title", "has_rows"};
    db_value_t values[] = {db_value_text("Interface Status"), db_value_int(has_rows ? 1 : 0)};
    int ret = db_insert(IF_SHOW_DB, IF_SHOW_META, fields, values, 2);
    db_value_free(&values[0]);
    return ret;
}

static int if_insert_show_row(const char *name, const char *type, const char *state, const char *ip_address)
{
    const char *fields[] = {"name", "type", "state", "ip_address"};
    db_value_t values[] = {db_value_text(name), db_value_text(type), db_value_text(state),
                              db_value_text(ip_address)};
    int ret = db_insert(IF_SHOW_DB, IF_SHOW_ROW, fields, values, 4);
    db_value_free(&values[0]);
    db_value_free(&values[1]);
    db_value_free(&values[2]);
    db_value_free(&values[3]);
    return ret;
}

static int if_insert_show_detail(const char *name, const char *type, const char *state, const char *ip_address,
                                 const char *netmask, const char *mac, int mtu)
{
    const char *fields[] = {"name", "type", "state", "ip_address", "netmask", "mac", "mtu"};
    db_value_t values[] = {db_value_text(name), db_value_text(type), db_value_text(state),
                              db_value_text(ip_address), db_value_text(netmask), db_value_text(mac),
                              db_value_int(mtu)};
    int ret = db_insert(IF_SHOW_DB, IF_SHOW_DETAIL, fields, values, 7);
    db_value_free(&values[0]);
    db_value_free(&values[1]);
    db_value_free(&values[2]);
    db_value_free(&values[3]);
    db_value_free(&values[4]);
    db_value_free(&values[5]);
    return ret;
}

// ============================================================================
// DB payload 预解析辅助
// ============================================================================

typedef struct
{
    gboolean has_action_show;
    gboolean has_name;
    gboolean has_ctx_name;
    gboolean has_ip;
    gboolean has_netmask;
    char name[32];
    char ctx_name[32];
} if_payload_info_t;

static void if_collect_payload_info(cfg_db_payload_parser_t *parser, if_payload_info_t *info)
{
    memset(info, 0, sizeof(*info));

    uint8_t ff;
    char *fn;
    db_value_t val;
    while (cfg_db_payload_next(parser, &ff, &fn, &val) == 1)
    {
        if (CFG_DB_IS_CONTEXT(ff))
        {
            if (strcmp(fn, "name") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
            {
                strncpy(info->ctx_name, val.data.text, sizeof(info->ctx_name) - 1);
                info->has_ctx_name = TRUE;
            }
        }
        else
        {
            if (strcmp(fn, "action") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
            {
                if (strcmp(val.data.text, "show") == 0)
                {
                    info->has_action_show = TRUE;
                }
            }
            else if (strcmp(fn, "name") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
            {
                strncpy(info->name, val.data.text, sizeof(info->name) - 1);
                info->has_name = TRUE;
            }
            else if (strcmp(fn, "ip_address") == 0)
            {
                info->has_ip = TRUE;
            }
            else if (strcmp(fn, "netmask") == 0)
            {
                info->has_netmask = TRUE;
            }
        }
        g_free(fn);
        db_value_free(&val);
    }
}

// ============================================================================
// DB table 载荷处理函数
// ============================================================================

/**
 * @brief 处理 interface 进入命令（if GE-x → 视图切换）
 *
 * 该命令的参数是关键字（GE-1 等），没有 field_name。
 * 通过检查载荷的 flags 中是否有 no_cmd，以及上下文字段来分辨操作。
 * 但 if GE-x 的关键字名实际作为 "值" 传递到 cfg_id 匹配元素中，
 * 而新载荷中关键字没有 field_name，所以我们无法直接从载荷获取接口名。
 *
 * 解决方案：interface 进入命令在新载荷中 table_name = "if_interface"，
 * 且 is_no_cmd = false，且 num_fields = 0（无参数字段）。
 * 这时候接口名信息在命令树匹配结果的关键字 value 中，但不会传递到载荷。
 *
 * 因此对于 interface 进入命令（group 1），如果载荷没有字段，
 * 我们需要从上下文或其他方式获取接口名。
 * 但实际上 cfg 端 dispatch 时已经把所有匹配到的关键字以 cfg_id + value 形式
 * 放在了旧 TLV 中。新载荷只有带 field_name 的参数。
 *
 * 鉴于 interface 选择命令的特殊性（选择的是关键字而非参数），
 * 我们保持旧 TLV 处理方式或在载荷中扩展一个特殊字段。
 *
 * 更简单的做法：在 commands.xml 中为 GE-x 关键字元素添加 field="name"，
 * 这样它们作为"伪参数"传递。但它们是 keyword 类型...
 *
 * 实际做法：interface 命令的 group 没有参数字段，但有 db/table。
 * 新载荷中只有 flags + db + table + 0 fields。
 * 模块根据 "进入时无字段 + table=if_interface" 来识别这是 interface 选择。
 * 但我们需要知道选择了哪个接口...
 *
 * 最务实的做法：对于 interface 命令，当 num_fields == 0 时回退到旧 TLV。
 * 但新格式中不再有旧 TLV 数据。
 *
 * 最终方案：保持接口选择命令不变（它的 group 有 db/table 但关键字无 field）。
 * 我们暂时让 interface 模块在遇到 table=if_interface 且 num_fields=0 时
 * 返回错误，因为完整解决需要改造 XML（让 GE-x 变成参数类型）。
 *
 * 但这不理想。更好的方案是让 dispatch 在命令只有关键字匹配（无 field_name 参数）
 * 的情况下，仍然走旧 TLV。实际上 dispatch.c 已经做了这个判断：
 * 如果 result->db_name 存在就用新格式。但 group 1 的 GE-x 是关键字没有 field，
 * 新格式中 num_fields=0。
 *
 * 让我们在这种情况下处理：检测到 table=if_interface + num_fields=0 + not no_cmd
 * → 这是 interface 进入命令，但我们无法获取接口名。
 *
 * 实际上，最简单的改法是在 cli_dispatch.c 中：
 * 当 result->db_name 存在但所有元素都没有 field_name 时，回退到旧 TLV。
 * 但我不想在这个 Phase 中改 dispatch 逻辑。
 *
 * 因此暂时：对于 interface 进入命令（无字段），直接从旧 TLV 兼容处理，
 * 其他有字段的命令（ip address、shutdown、show）用新载荷。
 */

// 旧 TLV 处理函数（仅用于 interface 选择和 show 带关键字名）

static int handle_interface_cmd_legacy(ipc_message_t *msg, cfg_tlv_parser_t *parser)
{
    char ifname[32] = {0};

    uint32_t cfg_id;
    const uint8_t *value;
    uint16_t len;
    int tlv_ret;
    while ((tlv_ret = cfg_tlv_parser_next(parser, &cfg_id, &value, &len)) == 1)
    {
        switch (cfg_id)
        {
            case 1:
                strcpy(ifname, "GE-1");
                break;
            case 2:
                strcpy(ifname, "GE-2");
                break;
            case 3:
                strcpy(ifname, "GE-3");
                break;
            case 4:
                strcpy(ifname, "GE-4");
                break;
        }
    }

    if (ifname[0] == '\0')
    {
        send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "Error: No interface specified\r\n");
        return ERRCODE_FAIL;
    }

    const char *phys_name = if_map_get_physical(ifname);
    if (!if_exists(phys_name))
    {
        char err[128];
        snprintf(err, sizeof(err), "Error: Interface %s does not exist\r\n", ifname);
        send_resp(msg, CFG_MSG_TYPE_CLI_RESP, err);
        return ERRCODE_FAIL;
    }

    /* 更新全局接口上下文 */
    extern char g_current_interface[IFNAMSIZ];
    strncpy(g_current_interface, ifname, IFNAMSIZ - 1);

    /* 发送 VIEW_CHG 响应 + 新格式上下文 */
    char filled_prompt[CFG_CLI_MAX_PROMPT_LEN];
    snprintf(filled_prompt, sizeof(filled_prompt), "<NetNexus(config-if-%s)>", ifname);

    /* 构建新格式上下文: name 字段 */
    GByteArray *ctx_buf = g_byte_array_new();
    ctx_write_u16(ctx_buf, 1); /* num_fields = 1 */
    ctx_write_u8(ctx_buf, 0);  /* field_flags = 0 */
    ctx_write_string(ctx_buf, "name");
    ctx_write_u8(ctx_buf, (uint8_t)DB_TYPE_TEXT);
    ctx_write_string(ctx_buf, ifname);

    uint32_t total_len = CFG_CLI_MAX_PROMPT_LEN + ctx_buf->len;
    char *msg_out = g_malloc0(total_len);
    memcpy(msg_out, filled_prompt, CFG_CLI_MAX_PROMPT_LEN);
    memcpy(msg_out + CFG_CLI_MAX_PROMPT_LEN, ctx_buf->data, ctx_buf->len);
    g_byte_array_free(ctx_buf, TRUE);

    ipc_message_t *resp = ipc_message_create(CFG_MSG_TYPE_CLI_VIEW_CHG, DEV_MODULE_ID_IF, msg->src_module_id,
                                                   msg->request_id, msg_out, total_len, g_free);
    if (resp)
    {
        ipc_send_response(g_if_local->ipc_ctx, resp);
        ipc_message_free(resp);
    }

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理接口配置命令（ip address / shutdown / no shutdown）
 */
static int handle_if_config_db(ipc_message_t *msg, cfg_db_payload_parser_t *parser)
{
    gboolean is_no = CFG_DB_IS_NO_CMD(parser);
    char ip[20] = {0};
    char mask[20] = {0};
    gboolean has_ip = FALSE;
    gboolean has_shutdown = FALSE;
    char ctx_ifname[32] = {0};

    /* 解析字段 */
    uint8_t ff;
    char *fn;
    db_value_t val;
    while (cfg_db_payload_next(parser, &ff, &fn, &val) == 1)
    {
        if (CFG_DB_IS_CONTEXT(ff))
        {
            /* 上下文字段 */
            if (strcmp(fn, "name") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
            {
                strncpy(ctx_ifname, val.data.text, sizeof(ctx_ifname) - 1);
            }
        }
        else
        {
            /* 命令参数 */
            if (strcmp(fn, "ip_address") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
            {
                strncpy(ip, val.data.text, sizeof(ip) - 1);
                has_ip = TRUE;
            }
            else if (strcmp(fn, "netmask") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
            {
                strncpy(mask, val.data.text, sizeof(mask) - 1);
            }
        }
        g_free(fn);
        db_value_free(&val);
    }

    /* 判断是否有接口名 */
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
            char where[64];
            snprintf(where, sizeof(where), "name = '%s'", ifname);
            const char *field_names[] = {"ip_address", "netmask"};
            db_value_t values[] = {db_value_text(ip), db_value_text(mask)};
            db_update(parser->db_name, parser->table_name, field_names, values, 2, where);
            db_value_free(&values[0]);
            db_value_free(&values[1]);

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
        /* shutdown / no shutdown（通过 flags 中的 no_cmd 判断） */
        has_shutdown = TRUE;
        int state = is_no ? 1 : 0; /* no shutdown → UP(1), shutdown → DOWN(0) */
        if (if_set_state(phys_name, state) == ERRCODE_SUCCESS)
        {
            char where[64];
            snprintf(where, sizeof(where), "name = '%s'", ifname);
            const char *field_names[] = {"shutdown"};
            db_value_t values[] = {db_value_int(state ? 0 : 1)};
            db_update(parser->db_name, parser->table_name, field_names, values, 1, where);

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
 */
static int handle_if_show_db(ipc_message_t *msg, cfg_db_payload_parser_t *parser)
{
    char ifname[32] = {0};

    /* 解析可选接口名 */
    uint8_t ff;
    char *fn;
    db_value_t val;
    while (cfg_db_payload_next(parser, &ff, &fn, &val) == 1)
    {
        if (!CFG_DB_IS_CONTEXT(ff) && strcmp(fn, "name") == 0 && val.type == DB_TYPE_TEXT && val.data.text)
        {
            strncpy(ifname, val.data.text, sizeof(ifname) - 1);
        }
        g_free(fn);
        db_value_free(&val);
    }

    if (ifname[0] != '\0')
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

            if (if_insert_show_detail(ifname, type_str, state_str, ip_str, mask_str, mac_str, info.mtu) !=
                ERRCODE_SUCCESS)
            {
                send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "Error: Failed to write show output\r\n");
                return ERRCODE_FAIL;
            }
        }
        else
        {
            char err[128];
            snprintf(err, sizeof(err), "Error: Interface %s not found\r\n", ifname);
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, err);
            return ERRCODE_FAIL;
        }
    }
    else
    {
        /* 显示所有接口 */
        int row_count = g_interface_map.count;
        if (if_insert_show_meta(row_count > 0) != ERRCODE_SUCCESS)
        {
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "Error: Failed to write show output\r\n");
            return ERRCODE_FAIL;
        }

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
                if (if_insert_show_row(logical_name, type_str, state_str, ip_str) != ERRCODE_SUCCESS)
                {
                    send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "Error: Failed to write show output\r\n");
                    return ERRCODE_FAIL;
                }
            }
            else
            {
                if (if_insert_show_row(logical_name, "-", "DOWN", "-") != ERRCODE_SUCCESS)
                {
                    send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "Error: Failed to write show output\r\n");
                    return ERRCODE_FAIL;
                }
            }
        }
    }

    /* 成功，返回空响应，CFG 端渲染模板输出 */
    send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief 使用 DB table 载荷格式分发命令
 */
static int if_dispatch_db_payload(ipc_message_t *msg, cfg_db_payload_parser_t *parser)
{
    if (strcmp(parser->table_name, "if_interface") != 0)
    {
        printf("[if_cfg] 未知表名: %s\n", parser->table_name);
        send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "IF Error: Unknown table.\r\n");
        return ERRCODE_FAIL;
    }

    /* 预解析字段以判断命令类型 */
    const uint8_t *orig_data = parser->_reader_data;
    uint32_t orig_len = parser->_reader_len;

    if_payload_info_t info;
    if_collect_payload_info(parser, &info);

    cfg_db_payload_parser_t new_parser;
    if (cfg_db_payload_init(&new_parser, orig_data, orig_len) != 0)
    {
        send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "IF Error: Failed to re-parse payload.\r\n");
        return ERRCODE_FAIL;
    }

    int ret;
    if (info.has_action_show)
    {
        ret = handle_if_show_db(msg, &new_parser);
    }
    else if (info.has_name)
    {
        ret = handle_if_config_db(msg, &new_parser);
    }
    else
    {
        ret = handle_if_config_db(msg, &new_parser);
    }

    cfg_db_payload_cleanup(&new_parser);
    return ret;
}

// ============================================================================
// 主入口
// ============================================================================

int if_cli_handle_continue(ipc_message_t *msg)
{
    send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "");
    return ERRCODE_SUCCESS;
}

int if_cli_handle_message(ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    /* 尝试新 DB table 载荷格式 */
    cfg_db_payload_parser_t parser;
    if (cfg_db_payload_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) == 0)
    {
        printf("[if_cfg] 收到 DB table 载荷 (db=%s, table=%s, fields=%u)\n", parser.db_name, parser.table_name,
               parser.num_fields);

        /* interface 选择命令无参数字段，但需要从旧 TLV 获取接口名 */
        /* 检测：table=if_interface + num_fields=0 + not no_cmd → interface 选择或 show */
        /* 对于 interface 选择（进入配置视图），我们需要回退到旧 TLV 以获取关键字 cfg_id */
        /* 但新载荷格式中没有旧 TLV 数据了... */
        /* 解决方案：在 cli_dispatch.c 中，当命令没有任何带 field_name 的参数时，  */
        /* 仍然走旧 TLV。但 Phase 3 的 dispatch 逻辑是看 db_name 是否存在。 */
        /* 这里的问题是 group 1 有 db/table 但关键字无 field。 */
        /* 我们在此模块中暂时处理：如果 num_fields == 0 且没有 no_cmd，尝试旧 TLV */
        if (parser.num_fields == 0 && !CFG_DB_IS_NO_CMD(&parser))
        {
            cfg_db_payload_cleanup(&parser);
            /* 尝试旧 TLV 解析 */
            cfg_tlv_parser_t tlv_parser;
            if (cfg_tlv_parser_init(&tlv_parser, (const uint8_t *)msg->payload, msg->payload_len) == 0)
            {
                uint32_t group_id = tlv_parser.group_id;
                printf("[if_cfg] 回退 TLV (group_id=%u)\n", group_id);
                if (group_id == 1)
                {
                    return handle_interface_cmd_legacy(msg, &tlv_parser);
                }
                else if (group_id == 3)
                {
                    /* show if - 使用旧 TLV 解析接口名 */
                    char ifname[32] = {0};
                    uint32_t cfg_id;
                    const uint8_t *value;
                    uint16_t len;
                    int tlv_ret;
                    while ((tlv_ret = cfg_tlv_parser_next(&tlv_parser, &cfg_id, &value, &len)) == 1)
                    {
                        switch (cfg_id)
                        {
                            case 1:
                                strcpy(ifname, "GE-1");
                                break;
                            case 2:
                                strcpy(ifname, "GE-2");
                                break;
                            case 3:
                                strcpy(ifname, "GE-3");
                                break;
                            case 4:
                                strcpy(ifname, "GE-4");
                                break;
                        }
                    }

                    if (ifname[0] != '\0')
                    {
                        /* show if <name> - 显示单个接口 */
                        const char *phys_name = if_map_get_physical(ifname);
                        if_info_t info;
                        if (if_get_info(phys_name, &info) == ERRCODE_SUCCESS)
                        {
                            char buf[1024];
                            snprintf(buf, sizeof(buf),
                                     "Interface %s:\r\n"
                                     "  Type: %s\r\n"
                                     "  State: %s\r\n"
                                     "  IP: %s\r\n"
                                     "  Netmask: %s\r\n"
                                     "  MAC: %02x:%02x:%02x:%02x:%02x:%02x\r\n"
                                     "  MTU: %d\r\n",
                                     ifname, if_type_to_string(info.type),
                                     info.state == IF_STATE_UP ? "UP" : "DOWN",
                                     info.ip_address[0] ? info.ip_address : "not configured",
                                     info.netmask[0] ? info.netmask : "not configured", info.mac[0], info.mac[1],
                                     info.mac[2], info.mac[3], info.mac[4], info.mac[5], info.mtu);
                            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, buf);
                        }
                        else
                        {
                            char err[128];
                            snprintf(err, sizeof(err), "Error: Interface %s not found\r\n", ifname);
                            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, err);
                        }
                        return ERRCODE_SUCCESS;
                    }
                    else
                    {
                        /* show if - 无参数，回到 DB 格式处理显示所有 */
                        cfg_db_payload_parser_t parser2;
                        if (cfg_db_payload_init(&parser2, (const uint8_t *)msg->payload, msg->payload_len) == 0)
                        {
                            int ret = handle_if_show_db(msg, &parser2);
                            cfg_db_payload_cleanup(&parser2);
                            return ret;
                        }
                        send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "");
                        return ERRCODE_SUCCESS;
                    }
                }
            }
            send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "IF Error: Unsupported message format.\r\n");
            return ERRCODE_FAIL;
        }

        int result = if_dispatch_db_payload(msg, &parser);
        cfg_db_payload_cleanup(&parser);
        return result;
    }

    /* 回退到旧 TLV 格式 */
    printf("[if_cfg] 回退到旧 TLV 格式\n");
    cfg_tlv_parser_t tlv_parser;
    if (cfg_tlv_parser_init(&tlv_parser, (const uint8_t *)msg->payload, msg->payload_len) == 0)
    {
        uint32_t group_id = tlv_parser.group_id;
        if (group_id == 1)
        {
            return handle_interface_cmd_legacy(msg, &tlv_parser);
        }
    }

    send_resp(msg, CFG_MSG_TYPE_CLI_RESP, "IF Error: Unsupported message format.\r\n");
    return ERRCODE_FAIL;
}
