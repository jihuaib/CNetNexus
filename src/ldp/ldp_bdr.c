/**
 * @file   ldp_bdr.c
 * @brief  LDP 配置回放（show current-configuration）
 *
 * 顶层 ldp 块包含全局参数；接口级使能/计时器作为接口 anchor 贡献者输出。
 *
 * @author jhb
 * @date   2026/05/05
 */
#include "ldp_bdr.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "ldp.h"
#include "ldp_db.h"
#include "ldp_main.h"
#include "log.h"

static void ldp_bdr_send_resp(dev_ipc_message_t *msg, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(CLI_MSG_TYPE_RESP, DEV_MODULE_ID_LDP, msg->src_module_id,
                                                     msg->request_id, resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(ldp_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static void append_global_block(GString *out, const ldp_proto_cfg_t *cfg)
{
    if (!cfg || !cfg->admin_up)
    {
        return;
    }

    g_string_append(out, "!\r\n");
    g_string_append(out, "ldp\r\n");
    if (cfg->lsr_id != 0u)
    {
        g_string_append_printf(out, " lsr-id %u.%u.%u.%u\r\n", (cfg->lsr_id >> 24) & 0xFFu, (cfg->lsr_id >> 16) & 0xFFu,
                               (cfg->lsr_id >> 8) & 0xFFu, cfg->lsr_id & 0xFFu);
    }
    if (cfg->hello_interval_ms != LDP_DEFAULT_HELLO_INTERVAL_MS)
    {
        g_string_append_printf(out, " hello-interval %u\r\n", cfg->hello_interval_ms);
    }
    if (cfg->hold_time_ms != LDP_DEFAULT_HOLD_TIME_MS)
    {
        g_string_append_printf(out, " hold-time %u\r\n", cfg->hold_time_ms);
    }
    if (cfg->keepalive_ms != LDP_DEFAULT_KEEPALIVE_INTERVAL_MS)
    {
        g_string_append_printf(out, " keepalive-interval %u\r\n", cfg->keepalive_ms);
    }
    g_string_append(out, "!\r\n");
}

static void append_if_anchor_entries(GString *out)
{
    dev_ipc_context_t *ctx = ldp_local_ipc_ctx();
    if (!ctx)
    {
        return;
    }

    db_result_t *result = NULL;
    if (db_rpc_query(ctx, LDP_TABLE_INTERFACE, NULL, 0, NULL, &result) != ERRCODE_SUCCESS || !result)
    {
        if (result)
        {
            db_result_free(result);
        }
        return;
    }

    for (uint32_t i = 0; i < result->num_rows; i++)
    {
        db_row_t *row = result->rows[i];
        const char *ifname = db_row_get_text(row, "ifname", NULL);
        if (!ifname || ifname[0] == '\0')
        {
            continue;
        }
        if (!db_row_get_int(row, "enabled", 0))
        {
            continue;
        }

        GString *body = g_string_new("");
        g_string_append(body, " ldp enable\r\n");

        int64_t hello_ms = db_row_get_int(row, "hello_interval_ms", 0);
        int64_t hold_ms = db_row_get_int(row, "hold_time_ms", 0);
        if (hello_ms > 0)
        {
            g_string_append_printf(body, " ldp hello-interval %u\r\n", (uint32_t)hello_ms);
        }
        if (hold_ms > 0)
        {
            g_string_append_printf(body, " ldp hold-time %u\r\n", (uint32_t)hold_ms);
        }

        char key[CLI_CFG_ANCHOR_KEY_MAX];
        snprintf(key, sizeof(key), "iface/%s", ifname);
        cli_cfg_anchor_emit_body(out, key, body->str);
        g_string_free(body, TRUE);
    }

    db_result_free(result);
}

static int ldp_bdr_show_config_full(dev_ipc_message_t *msg)
{
    GString *out = g_string_new("");
    if (!out)
    {
        ldp_bdr_send_resp(msg, "");
        return ERRCODE_FAIL;
    }

    ldp_proto_cfg_t cfg;
    if (ldp_db_get_proto_cfg(&cfg) == ERRCODE_SUCCESS)
    {
        append_global_block(out, &cfg);
    }
    append_if_anchor_entries(out);

    ldp_bdr_send_resp(msg, out->str);
    g_string_free(out, TRUE);
    return ERRCODE_SUCCESS;
}

static int ldp_bdr_show_config_scoped(dev_ipc_message_t *msg, const cli_show_scope_t *scope)
{
    GString *out = g_string_new("");
    if (!out)
    {
        ldp_bdr_send_resp(msg, "");
        return ERRCODE_FAIL;
    }

    if (strcmp(scope->view_name, CLI_VIEW_LDP) == 0)
    {
        ldp_proto_cfg_t cfg;
        if (ldp_db_get_proto_cfg(&cfg) == ERRCODE_SUCCESS)
        {
            append_global_block(out, &cfg);
        }
    }
    else if (strcmp(scope->view_name, CLI_VIEW_IF) == 0 || strcmp(scope->view_name, CLI_VIEW_IF_LOOP) == 0)
    {
        /* M1: 接口范围 show this 暂不细分到具体 ifname，回放整体接口贡献 */
        append_if_anchor_entries(out);
    }

    ldp_bdr_send_resp(msg, out->str);
    g_string_free(out, TRUE);
    return ERRCODE_SUCCESS;
}

int ldp_bdr_handle_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("LDP BDR: invalid SHOW_CONFIG scope payload");
        ldp_bdr_send_resp(msg, "");
        return ERRCODE_FAIL;
    }

    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS)
    {
        return ldp_bdr_show_config_scoped(msg, &scope);
    }
    return ldp_bdr_show_config_full(msg);
}
