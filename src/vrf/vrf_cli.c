/**
 * @file   vrf_cli.c
 * @brief  VRF CLI 配置命令处理：IPC 线程解析 TLV，构造 apply 派发给 worker
 * @author jhb
 * @date   2026/03/05
 */
#include "vrf_cli.h"

#include <arpa/inet.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "log.h"
#include "vrf.h"
#include "vrf_db.h"
#include "vrf_main.h"
#include "work/vrf_cfg_apply.h"
#include "work/vrf_worker.h"

/** 命令组 ID（与 commands.xml 对齐） */
#define VRF_CLI_GROUP_CREATE 1
#define VRF_CLI_GROUP_AF 3
#define VRF_CLI_GROUP_RD 4
#define VRF_CLI_GROUP_RT 5

/** 参数 cfg_id */
#define VRF_CFGID_VRF_NAME 1
#define VRF_CFGID_RD_RT_STR 1
#define VRF_CFGID_RT_DIRECTION 2

// ============================================================================
// 响应辅助
// ============================================================================

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    char *data = g_strdup(text ? text : "");
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_VRF, msg->src_module_id,
                                                     msg->request_id, data, strlen(data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(vrf_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(data);
    }
}

// ============================================================================
// RD/RT 字符串解析（RFC 4364 type 0/1）
// ============================================================================

static int parse_rd_rt(const char *s, uint8_t out[8])
{
    if (!s || !out)
    {
        return -1;
    }
    const char *colon = strchr(s, ':');
    if (!colon || colon == s || colon[1] == '\0')
    {
        return -1;
    }

    char left[64];
    size_t left_len = (size_t)(colon - s);
    if (left_len >= sizeof(left))
    {
        return -1;
    }
    memcpy(left, s, left_len);
    left[left_len] = '\0';
    const char *right = colon + 1;
    memset(out, 0, 8);

    if (strchr(left, '.'))
    {
        struct in_addr ip;
        if (inet_pton(AF_INET, left, &ip) != 1)
        {
            return -1;
        }
        char *endp = NULL;
        unsigned long val = strtoul(right, &endp, 10);
        if (!endp || *endp != '\0' || val > 0xFFFFu)
        {
            return -1;
        }
        out[0] = 0x00;
        out[1] = 0x01;
        memcpy(out + 2, &ip.s_addr, 4);
        out[6] = (uint8_t)(val >> 8);
        out[7] = (uint8_t)val;
        return 0;
    }

    char *endp = NULL;
    unsigned long asn = strtoul(left, &endp, 10);
    if (!endp || *endp != '\0' || asn > 0xFFFFu)
    {
        return -1;
    }
    endp = NULL;
    unsigned long val = strtoul(right, &endp, 10);
    if (!endp || *endp != '\0' || val > 0xFFFFFFFFu)
    {
        return -1;
    }
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = (uint8_t)(asn >> 8);
    out[3] = (uint8_t)asn;
    out[4] = (uint8_t)(val >> 24);
    out[5] = (uint8_t)(val >> 16);
    out[6] = (uint8_t)(val >> 8);
    out[7] = (uint8_t)val;
    return 0;
}

// ============================================================================
// TLV 上下文 / 参数提取
// ============================================================================

/**
 * @brief 从 CLI_TLV_TYPE_CTX_STR 上下文条目复制字符串到缓冲区
 *
 * cli_tlv_entry_get_text 仅认 DB_TYPE_TEXT，对 ctx_str(0x81) 返回 NULL；
 * ctx_str 必须直接读 entry->value/length。
 *
 * @return 1 复制成功，0 类型不匹配
 */
static int ctx_str_copy(const cli_tlv_entry_t *entry, char *out, size_t out_cap)
{
    if (!entry || entry->type != CLI_TLV_TYPE_CTX_STR || !entry->value || out_cap == 0)
    {
        return 0;
    }
    uint16_t copy_len = entry->length;
    if (copy_len >= out_cap)
    {
        copy_len = (uint16_t)(out_cap - 1);
    }
    memcpy(out, entry->value, copy_len);
    out[copy_len] = '\0';
    return 1;
}

static int parse_af_ctx(cli_tlv_parser_t *parser, char *vrf_name, size_t vrf_name_size, uint16_t *afi_out,
                        uint8_t *safi_out)
{
    cli_tlv_entry_t entry;
    int got_vrf = 0;
    int got_afi = 0;

    *safi_out = (uint8_t)VRF_SAFI_UNICAST;

    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_VRF_NAME)
            {
                if (ctx_str_copy(&entry, vrf_name, vrf_name_size))
                {
                    got_vrf = 1;
                }
            }
            else if (entry.cfg_id == CLI_CTX_ID_VRF_AFI)
            {
                uint32_t v = 0;
                if (cli_tlv_entry_get_u32(&entry, &v) == 0)
                {
                    *afi_out = (uint16_t)v;
                    got_afi = 1;
                }
            }
            else if (entry.cfg_id == CLI_CTX_ID_VRF_SAFI)
            {
                uint32_t v = 0;
                if (cli_tlv_entry_get_u32(&entry, &v) == 0)
                {
                    *safi_out = (uint8_t)v;
                }
            }
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_rewind(parser);
    return (got_vrf && got_afi) ? 0 : -1;
}

static int parse_vrf_name(cli_tlv_parser_t *parser, uint32_t cfg_id, char *out, size_t cap)
{
    cli_tlv_entry_t entry;
    int got = 0;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == cfg_id)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                snprintf(out, cap, "%s", s);
                got = 1;
            }
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_rewind(parser);
    return got;
}

static int parse_param_str(cli_tlv_parser_t *parser, uint32_t cfg_id, char *out, size_t cap)
{
    return parse_vrf_name(parser, cfg_id, out, cap);
}

static int parse_rt_dir(cli_tlv_parser_t *parser)
{
    int dir = 2;
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == VRF_CFGID_RT_DIRECTION)
        {
            const char *s = cli_tlv_entry_get_text(&entry);
            if (s)
            {
                if (strcmp(s, "import") == 0)
                {
                    dir = 0;
                }
                else if (strcmp(s, "export") == 0)
                {
                    dir = 1;
                }
                else
                {
                    dir = 2;
                }
            }
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_rewind(parser);
    return dir;
}

// ============================================================================
// 命令处理：构造 apply 同步 dispatch
// ============================================================================

static int dispatch_apply(vrf_apply_cmd_t *cmd)
{
    int rc = vrf_worker_dispatch_apply(cmd);
    if (rc != ERRCODE_SUCCESS)
    {
        return -1;
    }
    return cmd->rc;
}

static int handle_create(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char vrf_name[VRF_NAME_MAX_LEN] = {0};
    if (!parse_param_str(parser, VRF_CFGID_VRF_NAME, vrf_name, sizeof(vrf_name)) || vrf_name[0] == '\0')
    {
        send_resp(msg, "VRF Error: Missing VRF name\r\n");
        return ERRCODE_FAIL;
    }
    if (strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        send_resp(msg, is_no ? "VRF Error: Public VRF cannot be deleted\r\n"
                             : "VRF Error: Public VRF cannot be manually created\r\n");
        return ERRCODE_FAIL;
    }

    vrf_apply_cmd_t cmd = {0};
    cmd.op = is_no ? VRF_APPLY_OP_VRF_DELETE : VRF_APPLY_OP_VRF_CREATE;
    g_strlcpy(cmd.vrf_name, vrf_name, sizeof(cmd.vrf_name));
    int rc = dispatch_apply(&cmd);
    if (rc != 0)
    {
        send_resp(msg, is_no ? "VRF Error: Delete failed\r\n" : "VRF Error: Create failed\r\n");
        return ERRCODE_FAIL;
    }
    /* worker 把分配/查到的 vrf_id 回传到 cmd.vrf_id，这里负责持久化到 DB */
    if (is_no)
    {
        (void)vrf_db_delete_vrf(cmd.vrf_id);
    }
    else
    {
        if (vrf_db_insert_vrf(cmd.vrf_id, cmd.vrf_name, cmd.l3vrf_table_id) != 0)
        {
            LOG_WARN("VRF: DB insert failed for vrf=%s", cmd.vrf_name);
        }
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

/**
 * @brief AF 关键字 cfg-id → (afi, safi) 映射
 *
 * 与 commands.xml group 3 的关键字 cfg-id 一致：
 *   cfg-id=1 → ipv4-unicast
 *   cfg-id=2 → ipv6-unicast
 * 后续新增 vpn/labeled 等 SAFI 时在此追加，不再写死 SAFI。
 *
 * @return 0 命中并填充，-1 未识别
 */
static int af_cfgid_to_pair(uint32_t cfg_id, uint16_t *afi_out, uint8_t *safi_out)
{
    switch (cfg_id)
    {
        case 1:
            *afi_out = VRF_AFI_IPV4;
            *safi_out = (uint8_t)VRF_SAFI_UNICAST;
            return 0;
        case 2:
            *afi_out = VRF_AFI_IPV6;
            *safi_out = (uint8_t)VRF_SAFI_UNICAST;
            return 0;
        default:
            return -1;
    }
}

static int handle_af(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char vrf_name[VRF_NAME_MAX_LEN] = {0};
    uint16_t afi = 0;
    uint8_t safi = 0;

    /* AF 命令本身切换视图，CLI_CTX_ID_VRF_AFI/SAFI 此刻尚未注入；
     * (afi, safi) 由关键字 cfg-id 唯一决定（见 af_cfgid_to_pair），
     * vrf_name 仍来自父视图已累积的 CLI_CTX_ID_VRF_NAME 上下文。 */
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_VRF_NAME)
            {
                (void)ctx_str_copy(&entry, vrf_name, sizeof(vrf_name));
            }
        }
        else
        {
            (void)af_cfgid_to_pair(entry.cfg_id, &afi, &safi);
        }
        cli_tlv_entry_free(&entry);
    }

    if (vrf_name[0] == '\0' || afi == 0 || safi == 0)
    {
        send_resp(msg, "VRF Error: Address-family context missing\r\n");
        return ERRCODE_FAIL;
    }

    vrf_apply_cmd_t cmd = {0};
    cmd.op = is_no ? VRF_APPLY_OP_AF_DELETE : VRF_APPLY_OP_AF_CREATE;
    g_strlcpy(cmd.vrf_name, vrf_name, sizeof(cmd.vrf_name));
    cmd.afi = afi;
    cmd.safi = safi;
    int rc = dispatch_apply(&cmd);
    if (rc == VRF_APPLY_RC_FAIL)
    {
        send_resp(msg, is_no ? "" : "VRF Error: AF create failed\r\n");
        return is_no ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }
    if (rc == VRF_APPLY_RC_NOOP)
    {
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }
    if (is_no)
    {
        (void)vrf_db_delete_af(cmd.vrf_id, cmd.afi, cmd.safi);
    }
    else
    {
        (void)vrf_db_set_af_rd(cmd.vrf_id, cmd.afi, cmd.safi, NULL);
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_rd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char vrf_name[VRF_NAME_MAX_LEN] = {0};
    uint16_t afi = 0;
    uint8_t safi = 0;
    if (parse_af_ctx(parser, vrf_name, sizeof(vrf_name), &afi, &safi) != 0)
    {
        send_resp(msg, "VRF Error: Address-family context missing\r\n");
        return ERRCODE_FAIL;
    }
    if (strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        send_resp(msg, "VRF Error: Public VRF cannot configure RD\r\n");
        return ERRCODE_FAIL;
    }

    vrf_apply_cmd_t cmd = {0};
    g_strlcpy(cmd.vrf_name, vrf_name, sizeof(cmd.vrf_name));
    cmd.afi = afi;
    cmd.safi = safi;

    if (is_no)
    {
        cmd.op = VRF_APPLY_OP_RD_CLEAR;
    }
    else
    {
        char rd_str[64] = {0};
        if (!parse_param_str(parser, VRF_CFGID_RD_RT_STR, rd_str, sizeof(rd_str)) || rd_str[0] == '\0')
        {
            send_resp(msg, "VRF Error: Missing RD\r\n");
            return ERRCODE_FAIL;
        }
        if (parse_rd_rt(rd_str, cmd.rd.bytes) != 0)
        {
            send_resp(msg, "VRF Error: Invalid RD format (expect ASN:NN or IP:NN)\r\n");
            return ERRCODE_FAIL;
        }
        cmd.op = VRF_APPLY_OP_RD_SET;
    }

    int rc = dispatch_apply(&cmd);
    if (rc == VRF_APPLY_RC_FAIL)
    {
        send_resp(msg, cmd.errmsg[0] ? cmd.errmsg : "VRF Error: RD apply failed\r\n");
        return ERRCODE_FAIL;
    }
    if (rc == VRF_APPLY_RC_NOOP)
    {
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }
    (void)vrf_db_set_af_rd(cmd.vrf_id, cmd.afi, cmd.safi, is_no ? NULL : &cmd.rd);
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int handle_rt(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char vrf_name[VRF_NAME_MAX_LEN] = {0};
    uint16_t afi = 0;
    uint8_t safi = 0;
    if (parse_af_ctx(parser, vrf_name, sizeof(vrf_name), &afi, &safi) != 0)
    {
        send_resp(msg, "VRF Error: Address-family context missing\r\n");
        return ERRCODE_FAIL;
    }
    if (strcmp(vrf_name, VRF_PUBLIC_VRF_NAME) == 0)
    {
        send_resp(msg, "VRF Error: Public VRF cannot configure RT\r\n");
        return ERRCODE_FAIL;
    }

    char rt_str[64] = {0};
    if (!parse_param_str(parser, VRF_CFGID_RD_RT_STR, rt_str, sizeof(rt_str)) || rt_str[0] == '\0')
    {
        send_resp(msg, "VRF Error: Missing RT\r\n");
        return ERRCODE_FAIL;
    }

    vrf_apply_cmd_t cmd = {0};
    cmd.op = VRF_APPLY_OP_RT_MODIFY;
    g_strlcpy(cmd.vrf_name, vrf_name, sizeof(cmd.vrf_name));
    cmd.afi = afi;
    cmd.safi = safi;
    cmd.add = is_no ? 0 : 1;
    cmd.direction = (uint8_t)parse_rt_dir(parser);
    if (parse_rd_rt(rt_str, cmd.rt.bytes) != 0)
    {
        send_resp(msg, "VRF Error: Invalid RT format (expect ASN:NN or IP:NN)\r\n");
        return ERRCODE_FAIL;
    }

    if (dispatch_apply(&cmd) != 0)
    {
        send_resp(msg, "VRF Error: RT apply failed\r\n");
        return ERRCODE_FAIL;
    }
    /* both 方向写两次 */
    if (cmd.direction == 0 || cmd.direction == 2)
    {
        (void)vrf_db_modify_rt(cmd.vrf_id, cmd.afi, cmd.safi, 0, cmd.add ? 1 : 0, &cmd.rt);
    }
    if (cmd.direction == 1 || cmd.direction == 2)
    {
        (void)vrf_db_modify_rt(cmd.vrf_id, cmd.afi, cmd.safi, 1, cmd.add ? 1 : 0, &cmd.rt);
    }
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 入口
// ============================================================================

int vrf_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }
    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        send_resp(msg, "VRF Error: Failed to parse command\r\n");
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    switch (parser.group_id)
    {
        case VRF_CLI_GROUP_CREATE:
            rc = handle_create(msg, &parser);
            break;
        case VRF_CLI_GROUP_AF:
            rc = handle_af(msg, &parser);
            break;
        case VRF_CLI_GROUP_RD:
            rc = handle_rd(msg, &parser);
            break;
        case VRF_CLI_GROUP_RT:
            rc = handle_rt(msg, &parser);
            break;
        default:
            send_resp(msg, "VRF Error: Unknown command\r\n");
            break;
    }
    cli_tlv_cleanup(&parser);
    return rc;
}
