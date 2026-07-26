/**
 * @file   snmp_cli.c
 * @brief  SNMP CLI command handling
 */
#include "snmp_cli.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "errcode.h"
#include "snmp_db.h"
#include "snmp_main.h"

void snmp_cli_send_response(dev_ipc_message_t *msg, const char *text)
{
    snmp_cli_send_response_typed(msg, CLI_MSG_TYPE_RESP, text);
}

void snmp_cli_send_response_typed(dev_ipc_message_t *msg, uint32_t msg_type, const char *text)
{
    const char *safe_text = text ? text : "";
    char *resp_data = g_strdup(safe_text);
    dev_ipc_message_t *resp = dev_ipc_message_create(msg_type, DEV_MODULE_ID_SNMP, msg->src_module_id, msg->request_id,
                                                     resp_data, strlen(resp_data) + 1, g_free);
    if (resp)
    {
        dev_ipc_send_response(snmp_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static int handle_trap_server(dev_ipc_message_t *msg, cli_tlv_parser_t *parser)
{
    gboolean is_no = (parser->flags & CLI_PAYLOAD_FLAG_NO_CMD) != 0;
    snmp_config_msg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.trap_port = SNMP_TRAP_DEFAULT_PORT;

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
            const char *host = cli_tlv_entry_get_text(&entry);
            if (host)
            {
                g_strlcpy(cfg.trap_host, host, sizeof(cfg.trap_host));
            }
        }
        else if (entry.cfg_id == 2)
        {
            int64_t port = cli_tlv_entry_get_int(&entry);
            if (port > 0 && port <= 65535)
            {
                cfg.trap_port = (uint32_t)port;
            }
        }
        cli_tlv_entry_free(&entry);
    }

    if (is_no)
    {
        if (snmp_db_disable_trap() != ERRCODE_SUCCESS)
        {
            snmp_cli_send_response(msg, "SNMP Error: failed to persist trap config.\r\n");
            return ERRCODE_FAIL;
        }
        memset(&cfg, 0, sizeof(cfg));
        cfg.trap_port = SNMP_TRAP_DEFAULT_PORT;
        snmp_agent_apply_config(&cfg);
        snmp_cli_send_response_typed(msg, CLI_MSG_TYPE_RESP_EXITING,
                                     "SNMP: trap server disabled, process exiting.\r\n");
        kill(getpid(), SIGTERM);
        return ERRCODE_SUCCESS;
    }

    if (cfg.trap_host[0] == '\0')
    {
        snmp_cli_send_response(msg, "SNMP Error: trap server required.\r\n");
        return ERRCODE_FAIL;
    }

    cfg.trap_enabled = 1u;
    if (snmp_db_set_config(&cfg) != ERRCODE_SUCCESS)
    {
        snmp_cli_send_response(msg, "SNMP Error: failed to persist trap config.\r\n");
        return ERRCODE_FAIL;
    }
    snmp_agent_apply_config(&cfg);

    char resp[256];
    snprintf(resp, sizeof(resp), "SNMP: trap server set to %s:%u.\r\n", cfg.trap_host, (unsigned)cfg.trap_port);
    snmp_cli_send_response(msg, resp);
    return ERRCODE_SUCCESS;
}

int snmp_cli_handle_config_msg(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload)
    {
        return ERRCODE_FAIL;
    }

    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, (const uint8_t *)msg->payload, msg->payload_len) != 0)
    {
        snmp_cli_send_response(msg, "SNMP Error: failed to parse command payload.\r\n");
        return ERRCODE_FAIL;
    }

    int rc = ERRCODE_FAIL;
    switch (parser.group_id)
    {
        case SNMP_CLI_GROUP_ID_TRAP_SERVER:
            rc = handle_trap_server(msg, &parser);
            break;
        default:
            snmp_cli_send_response(msg, "SNMP Error: unknown command.\r\n");
            break;
    }

    cli_tlv_cleanup(&parser);
    return rc;
}
