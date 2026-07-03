/**
 * @file   snmp_bdr.c
 * @brief  SNMP configuration export
 */
#include "snmp_bdr.h"

#include <string.h>

#include "cli.h"
#include "errcode.h"
#include "log.h"
#include "snmp_cli.h"
#include "snmp_db.h"

int snmp_bdr_handle_show_config(dev_ipc_message_t *msg)
{
    cli_show_scope_t scope;
    if (cli_show_scope_payload_parse((const uint8_t *)msg->payload, msg->payload_len, &scope) != 0)
    {
        LOG_WARN("SNMP BDR: invalid SHOW_CONFIG scope payload");
        snmp_cli_send_response(msg, "");
        return ERRCODE_FAIL;
    }
    if (scope.mode == CLI_SHOW_SCOPE_MODE_THIS)
    {
        snmp_cli_send_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    snmp_config_msg_t cfg;
    if (snmp_db_get_config(&cfg) != ERRCODE_SUCCESS || !cfg.trap_enabled || cfg.trap_host[0] == '\0')
    {
        snmp_cli_send_response(msg, "");
        return ERRCODE_SUCCESS;
    }

    GString *out = g_string_new("!\r\n");
    if (cfg.trap_port == SNMP_TRAP_DEFAULT_PORT)
    {
        g_string_append_printf(out, "snmp trap server %s\r\n", cfg.trap_host);
    }
    else
    {
        g_string_append_printf(out, "snmp trap server %s port %u\r\n", cfg.trap_host, (unsigned)cfg.trap_port);
    }

    snmp_cli_send_response(msg, out->str);
    g_string_free(out, TRUE);
    return ERRCODE_SUCCESS;
}
