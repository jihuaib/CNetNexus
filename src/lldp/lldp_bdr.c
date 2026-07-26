/**
 * @file   lldp_bdr.c
 * @brief  LLDP 配置回放
 * @author jhb
 * @date   2026/06/07
 */
#include "lldp_bdr.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "lldp_db.h"
#include "lldp_main.h"
#include "log.h"

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

static void send_error_resp(dev_ipc_message_t *msg, const char *text)
{
    send_resp_typed(msg, CLI_MSG_TYPE_RESP_ERROR, text);
}

static int append_global(GString *out)
{
    lldp_proto_cfg_t cfg;
    if (lldp_db_get_proto_cfg(&cfg) != ERRCODE_SUCCESS)
    {
        LOG_WARN("LLDP BDR: failed to query protocol configuration");
        return ERRCODE_FAIL;
    }

    if (cfg.admin_up || cfg.tx_interval_sec != LLDP_DEFAULT_TX_INTERVAL_SEC ||
        cfg.hold_multiplier != LLDP_DEFAULT_HOLD_MULTIPLIER)
    {
        g_string_append(out, "!\r\n");
    }
    if (cfg.admin_up)
    {
        g_string_append(out, "lldp\r\n");
    }
    if (cfg.tx_interval_sec != LLDP_DEFAULT_TX_INTERVAL_SEC)
    {
        g_string_append_printf(out, "lldp timer %u\r\n", cfg.tx_interval_sec);
    }
    if (cfg.hold_multiplier != LLDP_DEFAULT_HOLD_MULTIPLIER)
    {
        g_string_append_printf(out, "lldp hold-multiplier %u\r\n", cfg.hold_multiplier);
    }
    return ERRCODE_SUCCESS;
}

static const char *admin_status_str(uint8_t s)
{
    switch (s)
    {
        case LLDP_IF_ADMIN_RX_ONLY:
            return "rxonly";
        case LLDP_IF_ADMIN_TX_ONLY:
            return "txonly";
        case LLDP_IF_ADMIN_DISABLED:
            return "disabled";
        case LLDP_IF_ADMIN_TX_RX:
        default:
            return "txrx";
    }
}

static int append_interfaces(GString *out)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx)
    {
        LOG_WARN("LLDP BDR: local IPC context is unavailable");
        return ERRCODE_FAIL;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, LLDP_TABLE_INTERFACE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS)
    {
        if (result)
        {
            db_result_free(result);
        }
        LOG_WARN("LLDP BDR: failed to query interface configuration");
        return ERRCODE_FAIL;
    }
    if (!result)
    {
        return ERRCODE_SUCCESS;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ifname = db_row_get_text(row, "ifname", NULL);
        if (!ifname)
        {
            continue;
        }
        GString *body = g_string_new("");
        if (db_row_get_int(row, "enabled", 0))
        {
            /*
             * 显式接口行继续输出正向 enable，保持与旧版 cfg/rollback 快照
             * 的 canonical 文本兼容。隐式默认接口没有 DB 行，因此不会输出。
             */
            g_string_append(body, " lldp enable\r\n");
        }
        else
        {
            g_string_append(body, " no lldp enable\r\n");
        }
        uint8_t admin_status = (uint8_t)db_row_get_int(row, "admin_status", LLDP_IF_ADMIN_TX_RX);
        if (admin_status != LLDP_IF_ADMIN_TX_RX)
        {
            g_string_append_printf(body, " lldp admin-status %s\r\n", admin_status_str(admin_status));
        }
        const char *port_desc = db_row_get_text(row, "port_desc", "");
        if (port_desc && port_desc[0] != '\0')
        {
            g_string_append_printf(body, " lldp port-description %s\r\n", port_desc);
        }
        char key[CLI_CFG_ANCHOR_KEY_MAX];
        snprintf(key, sizeof(key), "iface/%s", ifname);
        cli_cfg_anchor_emit_body(out, key, body->str);
        g_string_free(body, TRUE);
    }

    db_result_free(result);
    return ERRCODE_SUCCESS;
}

int lldp_bdr_handle_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("LLDP BDR: invalid SHOW_CONFIG scope payload");
        send_error_resp(msg, "LLDP BDR: invalid SHOW_CONFIG scope payload\r\n");
        return ERRCODE_FAIL;
    }

    GString *out = g_string_new("");
    int rc = ERRCODE_SUCCESS;
    if (scope.mode != CLI_SHOW_SCOPE_MODE_THIS)
    {
        if (append_global(out) != ERRCODE_SUCCESS || append_interfaces(out) != ERRCODE_SUCCESS)
        {
            rc = ERRCODE_FAIL;
        }
    }
    else if (strcmp(scope.view_name, CLI_VIEW_IF) == 0 || strcmp(scope.view_name, CLI_VIEW_IF_LOOP) == 0)
    {
        rc = append_interfaces(out);
    }

    if (rc != ERRCODE_SUCCESS)
    {
        send_error_resp(msg, "LLDP BDR: failed to read configuration\r\n");
        g_string_free(out, TRUE);
        return ERRCODE_FAIL;
    }

    send_resp(msg, out->str);
    g_string_free(out, TRUE);
    return ERRCODE_SUCCESS;
}
