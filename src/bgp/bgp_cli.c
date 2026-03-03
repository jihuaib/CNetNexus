/**
 * @file   bgp_cli.c
 * @brief  BGP 模块 CLI 命令处理
 * @author jhb
 * @date   2026/01/22
 */
#include "bgp_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "bgp_db.h"
#include "bgp_main.h"
#include "bgp_protocol.h"
#include "bgp_vrf.h"
#include "cli.h"
#include "db.h"
#include "dev.h"
#include "errcode.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 发送 CLI 响应辅助
// ============================================================================

static void bgp_send_cli_response(dev_ipc_message_t *msg, const char *text)
{
    char *resp_data = g_strdup(text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_RESP, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_bgp_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

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

static void ctx_write_i64(GByteArray *buf, int64_t v)
{
    uint32_t hi = htonl((uint32_t)(v >> 32));
    uint32_t lo = htonl((uint32_t)(v & 0xFFFFFFFF));
    g_byte_array_append(buf, (const uint8_t *)&hi, 4);
    g_byte_array_append(buf, (const uint8_t *)&lo, 4);
}

// ============================================================================
// 视图切换辅助
// ============================================================================

/**
 * @brief 获取视图提示符模板（通过 IPC 从 CFG 模块获取）
 * @param view_id     视图 ID
 * @param msg         原始消息（用于 request_id）
 * @param view_name   输出视图名称缓冲区
 * @param fallback    获取失败时使用的回退模板
 */
static void bgp_get_view_prompt(uint32_t view_id, dev_ipc_message_t *msg, char *view_name, const char *fallback)
{
    if (g_bgp_local->dev_ipc_ctx && dev_ipc_is_connected(g_bgp_local->dev_ipc_ctx, DEV_MODULE_ID_CFG))
    {
        uint32_t view_id_be = htonl(view_id);
        uint32_t *view_id_copy = g_malloc(sizeof(view_id_be));
        memcpy(view_id_copy, &view_id_be, sizeof(view_id_be));
        dev_ipc_message_t *req = dev_ipc_message_create(CFG_MSG_TYPE_CLI_CONTINUE, DEV_MODULE_ID_BGP, DEV_MODULE_ID_CFG,
                                                        msg->request_id, view_id_copy, sizeof(view_id_be), g_free);

        if (req)
        {
            req->msg_type = DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_CLI, 0x0010); /* GET_VIEW_PROMPT */
            dev_ipc_message_t *resp = dev_ipc_query(g_bgp_local->dev_ipc_ctx, DEV_MODULE_ID_CFG, req, 1000);
            if (resp && resp->payload && resp->payload_len > 0)
            {
                snprintf(view_name, CFG_CLI_MAX_VIEW_LEN, "%s", (char *)resp->payload);
                dev_ipc_message_free(resp);
            }
            else
            {
                snprintf(view_name, CFG_CLI_MAX_VIEW_LEN, "%s", fallback);
                if (resp)
                {
                    dev_ipc_message_free(resp);
                }
            }
            dev_ipc_message_free(req);
        }
        else
        {
            snprintf(view_name, CFG_CLI_MAX_VIEW_LEN, "%s", fallback);
        }
    }
    else
    {
        snprintf(view_name, CFG_CLI_MAX_VIEW_LEN, "%s", fallback);
    }
}

/**
 * @brief 发送视图切换消息
 * @param msg        原始消息
 * @param ctx_buf    上下文 TLV 数据（已序列化）
 * @param out_prompt 提示符字符串
 */
static void bgp_send_view_change(dev_ipc_message_t *msg, GByteArray *ctx_buf, const char *out_prompt)
{
    uint32_t total_len = CLI_CLI_MAX_PROMPT_LEN + ctx_buf->len;
    char *msg_out = g_malloc0(total_len);
    memcpy(msg_out, out_prompt, CLI_CLI_MAX_PROMPT_LEN);
    memcpy(msg_out + CLI_CLI_MAX_PROMPT_LEN, ctx_buf->data, ctx_buf->len);

    dev_ipc_message_t *resp = dev_ipc_message_create(CFG_MSG_TYPE_CLI_VIEW_CHG, DEV_MODULE_ID_BGP, msg->src_module_id,
                                                     msg->request_id, msg_out, total_len, g_free);
    if (resp)
    {
        dev_ipc_send_response(g_bgp_local->dev_ipc_ctx, resp);
        dev_ipc_message_free(resp);
    }
}

// ============================================================================
// 从上下文 TLV 中提取 as_number
// ============================================================================

/**
 * @brief 从 TLV parser 中提取上下文中的 as_number
 * @param parser TLV 解析器
 * @return as_number，未找到返回 0
 */
static uint32_t bgp_extract_ctx_as_number(cli_tlv_parser_t *parser)
{
    uint32_t as_number = 0;
    cli_tlv_entry_t entry;

    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id) && CFG_TLV_CONTEXT_ID(entry.cfg_id) == 2)
        {
            as_number = (uint32_t)cli_tlv_entry_get_int(&entry);
        }
        cli_tlv_entry_free(&entry);
    }

    return as_number;
}

// ============================================================================
// 命令处理函数
// ============================================================================

/**
 * @brief 处理 bgp 配置命令（bgp <as-number> / no bgp [as-number]）
 *
 * group_id=1, cfg_id: 1=no, 2=as_number
 */
static int handle_bgp_protocol(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = FALSE;
    uint32_t as_number = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* no 前缀 */
                is_no = TRUE;
                break;
            case 2: /* as_number 参数 */
                as_number = (uint32_t)cli_tlv_entry_get_int(&entry);
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    /* 删除场景 */
    if (is_no)
    {
        if (g_bgp_local->protocol == NULL)
        {
            bgp_send_cli_response(msg, "");
            return ERRCODE_FAIL;
        }
        if (as_number != 0)
        {
            if (g_bgp_local->protocol->as_number != as_number)
            {
                bgp_send_cli_response(msg, "BGP Error: AS number mismatch.\r\n");
                return ERRCODE_FAIL;
            }
        }

        bgp_protocol_destroy(g_bgp_local->protocol);
        g_bgp_local->protocol = NULL;

        (void)bgp_db_del_as(g_bgp_local->dev_ipc_ctx);

        bgp_send_cli_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    if (g_bgp_local->protocol == NULL)
    {
        g_bgp_local->protocol = bgp_protocol_create(as_number);
        if (bgp_db_set_as(g_bgp_local->dev_ipc_ctx, as_number) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
            return ERRCODE_FAIL;
        }
    }
    else
    {
        if (g_bgp_local->protocol->as_number != as_number)
        {
            bgp_send_cli_response(msg, "BGP Error: AS number mismatch.\r\n");
            return ERRCODE_FAIL;
        }
    }

    /* 发送 VIEW_CHG 响应 */
    char view_name[CFG_CLI_MAX_VIEW_LEN];
    bgp_get_view_prompt(CLI_VIEW_BGP, msg, view_name, "<NetNexus(bgp-%u)>");

    char out_prompt[CLI_CLI_MAX_PROMPT_LEN];
    snprintf(out_prompt, CLI_CLI_MAX_PROMPT_LEN, view_name, as_number);

    GByteArray *ctx_buf = g_byte_array_new();
    ctx_write_u16(ctx_buf, 1);
    ctx_write_u32(ctx_buf, 2);
    ctx_write_u8(ctx_buf, (uint8_t)DB_TYPE_INTEGER);
    ctx_write_u16(ctx_buf, 8);
    ctx_write_i64(ctx_buf, (int64_t)as_number);

    bgp_send_view_change(msg, ctx_buf, out_prompt);
    g_byte_array_free(ctx_buf, TRUE);

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> as <as-num> / no neighbor <ip> 命令
 *
 * group_id=3, cfg_id: 1=no, 2=ip-address, 3=as-number
 */
static int handle_bgp_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = FALSE;
    uint32_t remote_as = 0;
    int has_remote_as = 0;
    char ip_buf[64] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* no 前缀 */
                is_no = TRUE;
                break;
            case 2: /* ip-address 参数 */
            {
                const char *ip_str = cli_tlv_entry_get_text(&entry);
                if (ip_str)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", ip_str);
                }
                break;
            }
            case 3: /* as-number 参数 */
                remote_as = (uint32_t)cli_tlv_entry_get_int(&entry);
                has_remote_as = 1;
                break;
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    resp_buf[0] = '\0';

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured. Run 'bgp <as-number>' first.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(g_bgp_local->protocol, BGP_VRF_PUBLIC_ID);

    if (is_no)
    {
        if (ip_buf[0] == '\0')
        {
            bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
            return ERRCODE_FAIL;
        }

        net_addr_t ip_addr;
        if (net_addr_from_str(ip_buf, &ip_addr) != 0)
        {
            bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
            return ERRCODE_FAIL;
        }

        if (vrf0)
        {
            bgp_session_t *del_sess = bgp_vrf_find_session(vrf0, &ip_addr);
            if (del_sess)
            {
                bgp_vrf_af_disable_neighbor(vrf0, BGP_AFI_IPV4, BGP_SAFI_UNICAST, &ip_addr);
                bgp_server_stop_session_conns(del_sess);
            }
            bgp_vrf_del_session(vrf0, &ip_addr);
        }

        int rows = bgp_db_del_session(g_bgp_local->dev_ipc_ctx, BGP_VRF_PUBLIC_NAME, ip_buf);
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s deleted (%d row).\r\n", ip_buf, rows > 0 ? rows : 0);
        bgp_send_cli_response(msg, resp_buf);
        return ERRCODE_SUCCESS;
    }

    /* neighbor <ip> as <as-num> — 创建/更新 session */
    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }
    if (!has_remote_as)
    {
        bgp_send_cli_response(msg, "BGP Error: Missing remote AS number.\r\n");
        return ERRCODE_FAIL;
    }

    net_addr_t ip_addr;
    if (net_addr_from_str(ip_buf, &ip_addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_session(g_bgp_local->dev_ipc_ctx, BGP_VRF_PUBLIC_NAME, ip_buf, remote_as) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    if (vrf0)
    {
        bgp_session_t *existing = bgp_vrf_find_session(vrf0, &ip_addr);
        if (existing)
        {
            existing->remote_as = remote_as;
        }
        else
        {
            bgp_session_t *sess = bgp_session_create(&ip_addr, remote_as);
            bgp_vrf_add_session(vrf0, sess);
            LOG_INFO("BGP: neighbor %s 已配置，等待地址族使能后再启动监听", ip_buf);
        }
    }

    snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s AS %u configured.\r\n", ip_buf, remote_as);
    bgp_send_cli_response(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 address-family ipv4-unicast 命令
 *
 * group_id=4, cfg_id: 1=ipv4-unicast (keyword)
 */
static int handle_bgp_addr_family(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    uint32_t as_number = bgp_extract_ctx_as_number(parser);

    char view_name[CFG_CLI_MAX_VIEW_LEN];
    bgp_get_view_prompt(CLI_VIEW_BGP_AF_IPV4, msg, view_name, "<NetNexus(bgp-%u-af-ipv4)>");

    char out_prompt[CLI_CLI_MAX_PROMPT_LEN];
    snprintf(out_prompt, CLI_CLI_MAX_PROMPT_LEN, view_name, as_number);

    GByteArray *ctx_buf = g_byte_array_new();
    ctx_write_u16(ctx_buf, 1);
    ctx_write_u32(ctx_buf, 2);
    ctx_write_u8(ctx_buf, (uint8_t)DB_TYPE_INTEGER);
    ctx_write_u16(ctx_buf, 8);
    ctx_write_i64(ctx_buf, (int64_t)as_number);

    bgp_send_view_change(msg, ctx_buf, out_prompt);
    g_byte_array_free(ctx_buf, TRUE);

    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 neighbor <ip> enable / no neighbor <ip> 命令（地址族视图）
 *
 * group_id=5, cfg_id: 1=no, 2=ip-address, 3=enable (keyword)
 */
static int handle_bgp_af_neighbor(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = FALSE;
    char ip_buf[64] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CFG_TLV_IS_CONTEXT(entry.cfg_id))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }

        switch (entry.cfg_id)
        {
            case 1: /* no 前缀 */
                is_no = TRUE;
                break;
            case 2: /* ip-address 参数 */
            {
                const char *ip_str = cli_tlv_entry_get_text(&entry);
                if (ip_str)
                {
                    snprintf(ip_buf, sizeof(ip_buf), "%s", ip_str);
                }
                break;
            }
            default:
                break;
        }
        cli_tlv_entry_free(&entry);
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    resp_buf[0] = '\0';

    if (ip_buf[0] == '\0')
    {
        bgp_send_cli_response(msg, "BGP Error: Missing neighbor IP address.\r\n");
        return ERRCODE_FAIL;
    }

    if (!g_bgp_local->protocol)
    {
        bgp_send_cli_response(msg, "BGP Error: BGP not configured.\r\n");
        return ERRCODE_FAIL;
    }

    net_addr_t ip_addr;
    if (net_addr_from_str(ip_buf, &ip_addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Invalid IP address.\r\n");
        return ERRCODE_FAIL;
    }

    bgp_vrf_t *vrf0 = bgp_protocol_get_vrf(g_bgp_local->protocol, BGP_VRF_PUBLIC_ID);

    if (is_no)
    {
        bgp_session_t *sess = vrf0 ? bgp_vrf_find_session(vrf0, &ip_addr) : NULL;

        if (vrf0)
        {
            bgp_vrf_af_disable_neighbor(vrf0, BGP_AFI_IPV4, BGP_SAFI_UNICAST, &ip_addr);
        }

        if (sess && vrf0 && !bgp_vrf_neighbor_has_any_af(vrf0, &ip_addr))
        {
            bgp_server_stop_session_conns(sess);
        }

        int rows = bgp_db_del_neighbor(g_bgp_local->dev_ipc_ctx, ip_buf, "ipv4-unicast");
        snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s disabled for ipv4-unicast (%d row).\r\n", ip_buf,
                 rows > 0 ? rows : 0);
        bgp_send_cli_response(msg, resp_buf);
        return ERRCODE_SUCCESS;
    }

    /* neighbor <ip> enable — 使能地址族邻居 */
    if (!vrf0 || !bgp_vrf_find_session(vrf0, &ip_addr))
    {
        bgp_send_cli_response(msg,
                              "BGP Error: Neighbor session not configured. Run 'neighbor <ip> as <as>' first.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_db_set_neighbor(g_bgp_local->dev_ipc_ctx, ip_buf, "ipv4-unicast") != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Database write failed.\r\n");
        return ERRCODE_FAIL;
    }

    if (bgp_vrf_af_enable_neighbor(vrf0, BGP_AFI_IPV4, BGP_SAFI_UNICAST, &ip_addr) != 0)
    {
        bgp_send_cli_response(msg, "BGP Error: Failed to enable AF neighbor.\r\n");
        return ERRCODE_FAIL;
    }

    /* 若是该 session 的第一个 AF peer，启动主动 TCP 连接 */
    bgp_session_t *sess = bgp_vrf_find_session(vrf0, &ip_addr);
    if (sess && g_list_length(sess->peers) == 1)
    {
        bgp_server_start_active_conn(sess);
    }

    snprintf(resp_buf, sizeof(resp_buf), "BGP: Neighbor %s enabled for ipv4-unicast.\r\n", ip_buf);
    bgp_send_cli_response(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

/**
 * @brief 处理 show bgp peer 命令
 *
 * group_id=2，直接构建格式化文本返回
 */
static int handle_bgp_peer_show(dev_ipc_message_t *msg)
{
    db_result_t *result = NULL;
    int ret = bgp_db_query(g_bgp_local->dev_ipc_ctx, &result);

    if (ret != 0 || !result)
    {
        bgp_send_cli_response(msg, "BGP Error: Database query failed.\r\n");
        return ERRCODE_FAIL;
    }

    char resp_buf[CLI_MAX_RESP_LEN];
    size_t offset = 0;

    if (result->num_rows == 0)
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "No BGP configuration found.\r\n");
    }
    else
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\nBGP Information:\r\n");
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "============================\r\n");

        for (uint32_t i = 0; i < result->num_rows; i++)
        {
            db_row_t *row = result->rows[i];
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                char value_str[256] = {0};
                switch (row->values[j].type)
                {
                    case DB_TYPE_INTEGER:
                        snprintf(value_str, sizeof(value_str), "%ld", row->values[j].data.i64);
                        break;
                    case DB_TYPE_REAL:
                        snprintf(value_str, sizeof(value_str), "%.6g", row->values[j].data.real);
                        break;
                    case DB_TYPE_TEXT:
                        if (row->values[j].data.text)
                        {
                            snprintf(value_str, sizeof(value_str), "%s", row->values[j].data.text);
                        }
                        break;
                    default:
                        snprintf(value_str, sizeof(value_str), "NULL");
                        break;
                }
                CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  %-20s: %s\r\n", row->field_names[j], value_str);
            }
        }
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\n");
    }

    db_result_free(result);

    /* 查询并显示 session 信息 */
    db_result_t *session_result = NULL;
    ret = bgp_db_query_sessions(g_bgp_local->dev_ipc_ctx, &session_result);
    if (ret == 0 && session_result && session_result->num_rows > 0)
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "BGP Sessions:\r\n");
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "----------------------------\r\n");
        for (uint32_t i = 0; i < session_result->num_rows; i++)
        {
            db_row_t *row = session_result->rows[i];
            const char *ip = "N/A";
            int64_t as_num = 0;
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                if (strcmp(row->field_names[j], "neighbor_ip") == 0 && row->values[j].type == DB_TYPE_TEXT)
                {
                    ip = row->values[j].data.text ? row->values[j].data.text : "N/A";
                }
                else if (strcmp(row->field_names[j], "remote_as") == 0 && row->values[j].type == DB_TYPE_INTEGER)
                {
                    as_num = row->values[j].data.i64;
                }
            }
            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  Neighbor: %-15s  AS: %ld\r\n", ip, as_num);
        }
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\n");
    }
    if (session_result)
    {
        db_result_free(session_result);
    }

    /* 查询并显示 neighbor 信息 */
    db_result_t *neighbor_result = NULL;
    ret = bgp_db_query_neighbors(g_bgp_local->dev_ipc_ctx, &neighbor_result);
    if (ret == 0 && neighbor_result && neighbor_result->num_rows > 0)
    {
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "BGP Address-Family Neighbors:\r\n");
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "----------------------------\r\n");
        for (uint32_t i = 0; i < neighbor_result->num_rows; i++)
        {
            db_row_t *row = neighbor_result->rows[i];
            const char *ip = "N/A";
            const char *afi = "N/A";
            for (uint32_t j = 0; j < row->num_fields; j++)
            {
                if (strcmp(row->field_names[j], "neighbor_ip") == 0 && row->values[j].type == DB_TYPE_TEXT)
                {
                    ip = row->values[j].data.text ? row->values[j].data.text : "N/A";
                }
                else if (strcmp(row->field_names[j], "afi") == 0 && row->values[j].type == DB_TYPE_TEXT)
                {
                    afi = row->values[j].data.text ? row->values[j].data.text : "N/A";
                }
            }
            CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "  Neighbor: %-15s  AFI: %s\r\n", ip, afi);
        }
        CLI_BUF_APPEND(resp_buf, sizeof(resp_buf), offset, "\r\n");
    }
    if (neighbor_result)
    {
        db_result_free(neighbor_result);
    }

    bgp_send_cli_response(msg, resp_buf);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 主入口
// ============================================================================

int bgp_cli_handle_continue(dev_ipc_message_t *msg)
{
    bgp_send_cli_response(msg, "");
    return ERRCODE_SUCCESS;
}

int bgp_cli_handle_message(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        LOG_ERROR("载荷解析失败");
        bgp_send_cli_response(msg, "BGP Error: Failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    LOG_DEBUG("收到 TLV 载荷 (group_id=%u)", parser.group_id);

    int result;
    switch (parser.group_id)
    {
        case BGP_CLI_GROUP_ID_PROTOCOL:
            result = handle_bgp_protocol(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_SHOW:
            result = handle_bgp_peer_show(msg);
            break;
        case BGP_CLI_GROUP_ID_NEIGHBOR:
            result = handle_bgp_neighbor(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_ADDR_FAMILY:
            result = handle_bgp_addr_family(msg, &parser);
            break;
        case BGP_CLI_GROUP_ID_AF_NEIGHBOR:
            result = handle_bgp_af_neighbor(msg, &parser);
            break;
        default:
            LOG_WARN("未知 group_id: %u", parser.group_id);
            bgp_send_cli_response(msg, "BGP Error: Unknown command group.\r\n");
            result = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return result;
}
