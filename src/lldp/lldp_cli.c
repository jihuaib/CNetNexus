/**
 * @file   lldp_cli.c
 * @brief  LLDP CLI 配置命令处理
 * @author jhb
 * @date   2026/06/07
 */
#include "lldp_cli.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "cli.h"
#include "errcode.h"
#include "if.h"
#include "lldp.h"
#include "lldp_db.h"
#include "lldp_main.h"
#include "work/lldp_worker.h"

#define LLDP_IF_LOOP_ID_MIN 1u
#define LLDP_IF_LOOP_ID_MAX 1024u

static void send_resp_typed(dev_ipc_message_t *msg, uint32_t msg_type, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(msg_type, DEV_MODULE_ID_LLDP, msg->src_module_id, msg->request_id,
                                                     resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(lldp_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static void send_resp(dev_ipc_message_t *msg, const char *text)
{
    send_resp_typed(msg, CLI_MSG_TYPE_RESP, text);
}

static const char *if_ctx_idx_to_name(uint32_t if_idx)
{
    switch (if_idx)
    {
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

static int dispatch_proto_apply(void)
{
    lldp_proto_cfg_t cfg;
    if (lldp_db_get_proto_cfg(&cfg) != ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    lldp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    apply.op = LLDP_APPLY_OP_PROTO_SET;
    apply.u.proto = cfg;
    return lldp_worker_dispatch_apply(&apply);
}

static int dispatch_if_apply(const char *ifname, int enabled)
{
    lldp_apply_cmd_t apply;
    memset(&apply, 0, sizeof(apply));
    if (enabled)
    {
        lldp_if_cfg_t cfg;
        if (lldp_db_get_interface(ifname, &cfg) != ERRCODE_SUCCESS)
        {
            return ERRCODE_FAIL;
        }
        apply.op = LLDP_APPLY_OP_IF_SET;
        apply.u.if_set = cfg;
    }
    else
    {
        apply.op = LLDP_APPLY_OP_IF_DEL;
        g_strlcpy(apply.u.if_del.ifname, ifname, sizeof(apply.u.if_del.ifname));
    }
    return lldp_worker_dispatch_apply(&apply);
}

static int handle_proto_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    int has_timer_kw = 0;
    int has_hold_kw = 0;
    uint32_t value = 0u;
    int has_value = 0;

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            has_timer_kw = 1;
        }
        else if (entry.cfg_id == 2)
        {
            has_hold_kw = 1;
        }
        else if (entry.cfg_id == 3 || entry.cfg_id == 4)
        {
            int64_t v = cli_tlv_entry_get_int(&entry);
            if (v >= 0 && v <= UINT32_MAX)
            {
                value = (uint32_t)v;
                has_value = 1;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    int kw_count = has_timer_kw + has_hold_kw;
    if (kw_count > 1)
    {
        send_resp(msg, "LLDP Error: Invalid protocol command\r\n");
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    if (kw_count == 0)
    {
        rc = lldp_db_set_proto_admin(is_no ? 0u : 1u);
    }
    else if (has_timer_kw)
    {
        if (!is_no && (!has_value || value < LLDP_MIN_TX_INTERVAL_SEC || value > LLDP_MAX_TX_INTERVAL_SEC))
        {
            send_resp(msg, "LLDP Error: Invalid transmit interval\r\n");
            return ERRCODE_FAIL;
        }
        rc = lldp_db_set_proto_tx_interval(is_no ? LLDP_DEFAULT_TX_INTERVAL_SEC : value);
    }
    else if (has_hold_kw)
    {
        if (!is_no && (!has_value || value < LLDP_MIN_HOLD_MULTIPLIER || value > LLDP_MAX_HOLD_MULTIPLIER))
        {
            send_resp(msg, "LLDP Error: Invalid hold multiplier\r\n");
            return ERRCODE_FAIL;
        }
        rc = lldp_db_set_proto_hold_multiplier(is_no ? LLDP_DEFAULT_HOLD_MULTIPLIER : value);
    }

    if (rc != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LLDP Error: Failed to persist protocol config\r\n");
        return ERRCODE_FAIL;
    }
    if (dispatch_proto_apply() != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LLDP Error: Failed to apply protocol config\r\n");
        return ERRCODE_FAIL;
    }

    if (kw_count == 0 && is_no)
    {
        gboolean has_config = FALSE;
        if (lldp_db_has_config(&has_config) != ERRCODE_SUCCESS)
        {
            send_resp(msg, "LLDP Error: Failed to verify remaining configuration\r\n");
            return ERRCODE_FAIL;
        }
        if (!has_config)
        {
            send_resp_typed(msg, CLI_MSG_TYPE_RESP_EXITING, "LLDP: admin disabled, process exiting.\r\n");
            kill(getpid(), SIGTERM);
            return ERRCODE_SUCCESS;
        }
        /* 接口 override 仍需 BDR owner 在线；仅关闭全局协议，不退出。 */
        send_resp(msg, "");
        return ERRCODE_SUCCESS;
    }

    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

static int resolve_ifname_from_ctx(cli_tlv_parser_t *parser, char *ifname, size_t ifname_len)
{
    uint32_t if_idx = 0u;
    uint32_t loop_id = 0u;

    cli_tlv_rewind(parser);
    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            if (entry.cfg_id == CLI_CTX_ID_IF_IDX)
            {
                if_idx = cli_tlv_entry_get_ctx_uint32(&entry);
            }
            else if (entry.cfg_id == CLI_CTX_ID_IF_LOOP_IDX)
            {
                loop_id = cli_tlv_entry_get_ctx_uint32(&entry);
            }
        }
        cli_tlv_entry_free(&entry);
    }
    cli_tlv_rewind(parser);

    if (loop_id >= LLDP_IF_LOOP_ID_MIN && loop_id <= LLDP_IF_LOOP_ID_MAX)
    {
        snprintf(ifname, ifname_len, "loop%u", loop_id);
        return ERRCODE_SUCCESS;
    }
    const char *name = if_ctx_idx_to_name(if_idx);
    if (!name)
    {
        return ERRCODE_FAIL;
    }
    g_strlcpy(ifname, name, ifname_len);
    return ERRCODE_SUCCESS;
}

static int handle_if_view_cmd(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    const int is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    char ifname[IF_LOGICAL_NAME_MAX] = {0};
    if (resolve_ifname_from_ctx(parser, ifname, sizeof(ifname)) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LLDP Error: No interface context\r\n");
        return ERRCODE_FAIL;
    }

    int has_enable_kw = 0;
    int has_admin_kw = 0;
    int has_port_desc_kw = 0;
    uint8_t admin_status = 0u;
    char port_desc[LLDP_PORT_DESC_MAX] = {0};

    cli_tlv_entry_t entry;
    while (cli_tlv_next(parser, &entry) == 1)
    {
        if (CLI_TLV_IS_CTX(&entry))
        {
            cli_tlv_entry_free(&entry);
            continue;
        }
        if (entry.cfg_id == 1)
        {
            has_enable_kw = 1;
        }
        else if (entry.cfg_id == 2)
        {
            has_admin_kw = 1;
        }
        else if (entry.cfg_id == 3)
        {
            admin_status = LLDP_IF_ADMIN_TX_RX;
        }
        else if (entry.cfg_id == 4)
        {
            admin_status = LLDP_IF_ADMIN_RX_ONLY;
        }
        else if (entry.cfg_id == 5)
        {
            admin_status = LLDP_IF_ADMIN_TX_ONLY;
        }
        else if (entry.cfg_id == 6)
        {
            admin_status = LLDP_IF_ADMIN_DISABLED;
        }
        else if (entry.cfg_id == 7)
        {
            has_port_desc_kw = 1;
        }
        else if (entry.cfg_id == 9)
        {
            const char *text = cli_tlv_entry_get_text(&entry);
            if (text)
            {
                g_strlcpy(port_desc, text, sizeof(port_desc));
            }
        }
        cli_tlv_entry_free(&entry);
    }

    int kw_count = has_enable_kw + has_admin_kw + has_port_desc_kw;
    if (kw_count != 1)
    {
        send_resp(msg, "LLDP Error: Invalid interface command\r\n");
        return ERRCODE_FAIL;
    }

    lldp_if_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (lldp_db_get_interface(ifname, &cfg) != ERRCODE_SUCCESS)
    {
        g_strlcpy(cfg.ifname, ifname, sizeof(cfg.ifname));
        cfg.enabled = 1u;
        cfg.admin_status = LLDP_IF_ADMIN_TX_RX;
    }

    if (has_enable_kw)
    {
        cfg.enabled = is_no ? 0u : 1u;
    }
    else if (has_admin_kw)
    {
        cfg.admin_status = is_no ? LLDP_IF_ADMIN_TX_RX : admin_status;
        if (!is_no && cfg.admin_status == 0u)
        {
            send_resp(msg, "LLDP Error: Invalid admin status\r\n");
            return ERRCODE_FAIL;
        }
    }
    else if (has_port_desc_kw)
    {
        if (is_no)
        {
            cfg.port_desc[0] = '\0';
        }
        else if (port_desc[0] == '\0')
        {
            send_resp(msg, "LLDP Error: Missing port description\r\n");
            return ERRCODE_FAIL;
        }
        else
        {
            g_strlcpy(cfg.port_desc, port_desc, sizeof(cfg.port_desc));
        }
    }

    const gboolean implicit_default = lldp_db_interface_is_implicit_default(&cfg);
    if (lldp_db_set_interface(ifname, &cfg) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LLDP Error: Failed to persist interface config\r\n");
        return ERRCODE_FAIL;
    }
    if (dispatch_if_apply(ifname, implicit_default ? 0 : 1) != ERRCODE_SUCCESS)
    {
        send_resp(msg, "LLDP Error: Failed to apply interface config\r\n");
        return ERRCODE_FAIL;
    }

    /*
     * 接口命令把最后一项 override 恢复为隐式默认时，DB marker 会立即清空，
     * 但进程暂不退出。旧 cfg 常按 `lldp enable` 后接 admin/description 回放；
     * 若在第一条命令后退出，后续命令会在重连窗口丢失。显式 `no lldp`
     * 仍可退出空模块，冷启动也会因 marker 为空保持 on-demand/down。
     */
    send_resp(msg, "");
    return ERRCODE_SUCCESS;
}

int lldp_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        send_resp(msg, "LLDP Error: Invalid command payload\r\n");
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    switch (parser.group_id)
    {
        case LLDP_CLI_GROUP_ID_PROTO:
            rc = handle_proto_cmd(msg, &parser);
            break;
        case LLDP_CLI_GROUP_ID_IF_VIEW:
            rc = handle_if_view_cmd(msg, &parser);
            break;
        default:
            send_resp(msg, "LLDP Error: Unknown command group\r\n");
            rc = ERRCODE_FAIL;
            break;
    }

    cli_tlv_cleanup(&parser);
    return rc;
}
